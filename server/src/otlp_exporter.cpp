#include <robotweax/srt_telemetry_otlp.h>

#include <robotweax/srt_telemetry_export.h>

#include "thread_name.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS: SO_NOSIGPIPE is set on the socket instead */
#endif

namespace
{

using Clock = std::chrono::steady_clock;

constexpr std::string_view default_endpoint = "http://127.0.0.1:4318/v1/metrics";
constexpr int max_attempts = 3;
constexpr std::size_t max_response_bytes = 64 * 1024;
/* Granularity at which a blocked attempt notices stop. */
constexpr int poll_slice_ms = 100;

struct Endpoint
{
    std::string host; /* without brackets */
    std::string port;
    std::string path;
    std::string host_header; /* as sent in Host */
};

bool parse_endpoint(const std::string_view url, Endpoint &e)
{
    constexpr std::string_view scheme = "http://";
    if (url.substr(0, scheme.size()) != scheme)
    {
        return false; /* https and anything else: not supported */
    }
    auto rest = url.substr(scheme.size());
    const auto slash = rest.find('/');
    const auto authority = rest.substr(0, slash);
    e.path = slash == std::string_view::npos ? "/v1/metrics" : std::string(rest.substr(slash));
    if (authority.empty() || authority.find('@') != std::string_view::npos)
    {
        return false;
    }
    std::string_view host;
    std::string_view port = "80";
    const bool bracketed = authority.front() == '[';
    if (bracketed)
    {
        const auto close = authority.find(']');
        if (close == std::string_view::npos)
        {
            return false;
        }
        host = authority.substr(1, close - 1);
        const auto after = authority.substr(close + 1);
        if (!after.empty())
        {
            if (after.front() != ':')
            {
                return false;
            }
            port = after.substr(1);
        }
    }
    else
    {
        const auto colon = authority.rfind(':');
        host = authority.substr(0, colon);
        if (colon != std::string_view::npos)
        {
            port = authority.substr(colon + 1);
        }
    }
    unsigned value = 0;
    const auto result = std::from_chars(port.data(), port.data() + port.size(), value);
    if (host.empty() || port.empty() || result.ec != std::errc{} || result.ptr != port.data() + port.size() ||
        value == 0 || value > 65535)
    {
        return false;
    }
    for (const char c : host)
    {
        const auto u = static_cast<unsigned char>(c);
        /* ':' and a zone ('%') only in an IPv6 literal, which needs brackets. */
        if (!(std::isalnum(u) || c == '.' || c == '-' || c == '_' || (bracketed && (c == ':' || c == '%'))))
        {
            return false;
        }
    }
    for (const char c : e.path)
    {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7f)
        {
            return false;
        }
    }
    e.host = std::string(host);
    e.port = std::string(port);
    e.host_header = std::string(authority);
    /* An IPv6 zone: "%25eth0" in a URI (RFC 6874), "%eth0" for the resolver;
     * accept either. */
    if (const auto zone = e.host.find('%'); zone != std::string::npos)
    {
        if (e.host.compare(zone, 3, "%25") == 0 && e.host.size() > zone + 3)
        {
            e.host.erase(zone + 1, 2);
        }
        const auto at = e.host_header.find('%');
        if (e.host_header.compare(at, 3, "%25") != 0 || e.host_header.size() <= at + 3 ||
            e.host_header[at + 3] == ']')
        {
            e.host_header.insert(at + 1, "25");
        }
    }
    return true;
}

bool token_char(const char c)
{
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) || std::strchr("!#$%&'*+-.^_`|~", c) != nullptr;
}

/* "k=v,k=v" into "k: v\r\n" lines. */
bool parse_headers(const std::string_view text, std::string &out)
{
    std::size_t start = 0;
    while (start < text.size())
    {
        auto end = text.find(',', start);
        end = end == std::string_view::npos ? text.size() : end;
        const auto item = text.substr(start, end - start);
        start = end + 1;
        if (item.empty())
        {
            continue;
        }
        const auto eq = item.find('=');
        if (eq == std::string_view::npos || eq == 0)
        {
            return false;
        }
        auto key = item.substr(0, eq);
        auto value = item.substr(eq + 1);
        while (!key.empty() && key.back() == ' ')
        {
            key.remove_suffix(1);
        }
        while (!key.empty() && key.front() == ' ')
        {
            key.remove_prefix(1);
        }
        while (!value.empty() && value.front() == ' ')
        {
            value.remove_prefix(1);
        }
        if (key.empty() || !std::all_of(key.begin(), key.end(), token_char))
        {
            return false;
        }
        while (!value.empty() && value.back() == ' ')
        {
            value.remove_suffix(1);
        }
        /* Values are percent-encoded as in OTEL_EXPORTER_OTLP_HEADERS
         * ("Bearer%20abc"); a '%' without two hex digits stays as it is.
         * The decoded value must be printable ASCII (no CR or LF). */
        std::string decoded;
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            const auto hex = [](const char c) {
                return std::isxdigit(static_cast<unsigned char>(c)) != 0;
            };
            if (value[i] == '%' && i + 2 < value.size() && hex(value[i + 1]) && hex(value[i + 2]))
            {
                unsigned byte = 0;
                std::from_chars(value.data() + i + 1, value.data() + i + 3, byte, 16);
                decoded += static_cast<char>(byte);
                i += 2;
            }
            else
            {
                decoded += value[i];
            }
        }
        for (const char c : decoded)
        {
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x20 || u >= 0x7f)
            {
                return false;
            }
        }
        /* The exporter sets these itself. */
        std::string lower(key);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == "host" || lower == "content-length" || lower == "content-type" || lower == "connection" ||
            lower == "transfer-encoding")
        {
            return false;
        }
        out.append(key);
        out += ": ";
        out += decoded;
        out += "\r\n";
    }
    return true;
}

std::uint64_t system_nano()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

} // namespace

struct robotweax_srt_telemetry_otlp_exporter
{
    robotweax_srt_telemetry_otlp_exporter_config_v1 config{};
    Endpoint endpoint;
    std::string extra_headers;
    std::vector<std::string> resource_storage; /* keys and values */
    std::vector<robotweax_srt_telemetry_label_v1> resource;
    int wake[2] = {-1, -1};
    std::atomic<bool> running{true};
    std::mutex mutex;
    std::condition_variable cv;
    robotweax_srt_telemetry_otlp_exporter_stats_v1 stats{};
    std::thread thread;
};

namespace
{

using Exporter = robotweax_srt_telemetry_otlp_exporter;

enum class Attempt
{
    ok,
    retry,
    fail,
    stopped
};

/* Waits for fd (or the stop pipe) until the deadline. */
int wait_for(const Exporter &x, const int fd, const short events, const Clock::time_point deadline)
{
    while (true)
    {
        if (!x.running)
        {
            return -2;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0)
        {
            return 0;
        }
        pollfd fds[2] = {{fd, events, 0}, {x.wake[0], POLLIN, 0}};
        const int n = ::poll(fds, 2, static_cast<int>(std::min<long long>(left, poll_slice_ms)));
        if (n < 0 && errno != EINTR)
        {
            return -1;
        }
        if (n > 0 && (fds[1].revents & POLLIN) != 0)
        {
            return -2;
        }
        if (n > 0 && fds[0].revents != 0)
        {
            return 1;
        }
    }
}

/* A name lookup on its own thread: getaddrinfo cannot be interrupted, and
 * a slow resolver (mDNS, an unreachable DNS server) must not hold up stop()
 * or exceed the attempt's timeout. An abandoned lookup frees its result
 * when it finishes. */
struct Lookup
{
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool abandoned = false;
    addrinfo *result = nullptr;
};

/* Returns the addresses, or nullptr (timeout, stop or failure; stopped set
 * on stop). Numeric hosts resolve at once, without a thread. */
addrinfo *resolve(const Exporter &x, const Clock::time_point deadline, bool &stopped)
{
    stopped = false;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo *found = nullptr;
    if (::getaddrinfo(x.endpoint.host.c_str(), x.endpoint.port.c_str(), &hints, &found) == 0)
    {
        return found;
    }
    auto lookup = std::make_shared<Lookup>();
    try
    {
        std::thread([lookup, host = x.endpoint.host, port = x.endpoint.port] {
            robotweax_srt_telemetry_internal::name_current_thread("srt-tel-dns");
            addrinfo name_hints{};
            name_hints.ai_family = AF_UNSPEC;
            name_hints.ai_socktype = SOCK_STREAM;
            name_hints.ai_flags = AI_NUMERICSERV;
            addrinfo *result = nullptr;
            if (::getaddrinfo(host.c_str(), port.c_str(), &name_hints, &result) != 0)
            {
                result = nullptr;
            }
            const std::lock_guard<std::mutex> lock(lookup->mutex);
            if (lookup->abandoned)
            {
                if (result != nullptr)
                {
                    ::freeaddrinfo(result);
                }
                return;
            }
            lookup->result = result;
            lookup->done = true;
            lookup->cv.notify_all();
        }).detach();
    }
    catch (const std::exception &)
    {
        return nullptr; /* no thread: retried */
    }
    std::unique_lock<std::mutex> lock(lookup->mutex);
    while (!lookup->done)
    {
        if (!x.running)
        {
            stopped = true;
            break;
        }
        const auto now = Clock::now();
        if (now >= deadline)
        {
            break;
        }
        lookup->cv.wait_for(lock, std::min<Clock::duration>(deadline - now, std::chrono::milliseconds(poll_slice_ms)));
    }
    if (!lookup->done)
    {
        lookup->abandoned = true;
        return nullptr;
    }
    return lookup->result;
}

Attempt post(Exporter &x, const std::string &request, int &status, const Clock::time_point limit)
{
    status = 0;
    const auto deadline = std::min(Clock::now() + std::chrono::milliseconds(x.config.timeout_ms), limit);
    bool stopped = false;
    addrinfo *found = resolve(x, deadline, stopped);
    if (stopped)
    {
        return Attempt::stopped;
    }
    if (found == nullptr)
    {
        return Attempt::retry;
    }
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(found, &::freeaddrinfo);
    int fd = -1;
    for (auto *a = found; a != nullptr && fd < 0; a = a->ai_next)
    {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0)
        {
            continue;
        }
        static_cast<void>(::fcntl(fd, F_SETFD, FD_CLOEXEC));
        static_cast<void>(::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK));
#if defined(SO_NOSIGPIPE)
        const int one = 1;
        static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)));
#endif
        if (::connect(fd, a->ai_addr, a->ai_addrlen) != 0 && errno != EINPROGRESS)
        {
            ::close(fd);
            fd = -1;
            continue;
        }
        const int ready = wait_for(x, fd, POLLOUT, deadline);
        int error = 0;
        socklen_t size = sizeof(error);
        if (ready == -2)
        {
            ::close(fd);
            return Attempt::stopped;
        }
        if (ready != 1 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0)
        {
            ::close(fd);
            fd = -1;
        }
    }
    if (fd < 0)
    {
        return Attempt::retry;
    }
    const auto finish = [&](const Attempt a)
    {
        ::close(fd);
        return a;
    };
    std::size_t sent = 0;
    /* A collector may answer (413, 400…) and close before reading the whole
     * request: then sending fails, but its status line may be readable. */
    bool send_failed = false;
    while (sent < request.size())
    {
        const auto n = ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (n > 0)
        {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            send_failed = true;
            break;
        }
        const int ready = wait_for(x, fd, POLLOUT, deadline);
        if (ready == -2)
        {
            return finish(Attempt::stopped);
        }
        if (ready != 1)
        {
            return finish(Attempt::retry);
        }
    }
    {
        std::lock_guard lock(x.mutex);
        x.stats.bytes_sent += sent;
    }
    /* Status line: "HTTP/1.1 200 ..."; the rest is read and dropped. */
    std::string response;
    char buffer[4096];
    while (response.find("\r\n") == std::string::npos && response.size() < max_response_bytes)
    {
        const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0)
        {
            response.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0)
        {
            break;
        }
        if ((errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) || send_failed)
        {
            break; /* judged by what arrived, if anything */
        }
        const int ready = wait_for(x, fd, POLLIN, deadline);
        if (ready == -2)
        {
            return finish(Attempt::stopped);
        }
        if (ready != 1)
        {
            return finish(Attempt::retry);
        }
    }
    if (response.size() < 12 || response.compare(0, 5, "HTTP/") != 0)
    {
        return finish(Attempt::retry);
    }
    const auto space = response.find(' ');
    if (space == std::string::npos || space + 4 > response.size())
    {
        return finish(Attempt::retry);
    }
    const auto code = std::string_view(response).substr(space + 1, 3);
    int value = 0;
    std::from_chars(code.data(), code.data() + code.size(), value);
    status = value;
    if (value >= 200 && value < 300)
    {
        return finish(Attempt::ok);
    }
    /* OTLP/HTTP: retry 429, 502, 503, 504; others are permanent. */
    return finish(value == 429 || value == 502 || value == 503 || value == 504 ? Attempt::retry : Attempt::fail);
}

int collect(void *user, const char *data, const std::size_t size)
{
    try
    {
        static_cast<std::string *>(user)->append(data, size);
        return 0;
    }
    catch (...)
    {
        return 1;
    }
}

void export_once(Exporter &x)
{
    robotweax_srt_telemetry_render_options_v1 options;
    robotweax_srt_telemetry_render_options_default(ROBOTWEAX_SRT_TELEMETRY_RENDER_OTLP_JSON, &options);
    options.flags = x.config.render_flags;
    options.window_nano = x.config.window_nano;
    std::string body;
    const auto error = robotweax_srt_telemetry_sampler_render_otlp_to(
        x.config.sampler, &options, x.resource.empty() ? nullptr : x.resource.data(), x.resource.size(), &collect,
        &body);
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        std::lock_guard lock(x.mutex);
        x.stats.exports_failed += 1;
        x.stats.last_error = error;
        x.stats.last_http_status = 0;
        return;
    }
    std::string request = "POST ";
    request += x.endpoint.path;
    request += " HTTP/1.1\r\nHost: ";
    request += x.endpoint.host_header;
    request += "\r\nUser-Agent: robotweax-srt-telemetry/";
    request += robotweax_srt_telemetry_version();
    request += "\r\nContent-Type: application/json\r\nContent-Length: ";
    request += std::to_string(body.size());
    request += "\r\nConnection: close\r\n";
    request += x.extra_headers;
    request += "\r\n";
    request += body;

    /* The first attempt gets its whole timeout; retries only the time left
     * in this interval, so exports keep their cadence. */
    const auto interval_end = Clock::now() + std::chrono::milliseconds(x.config.interval_ms);
    for (int attempt = 0; attempt < max_attempts; ++attempt)
    {
        int status = 0;
        const auto result = post(x, request, status, attempt == 0 ? Clock::time_point::max() : interval_end);
        if (result == Attempt::stopped)
        {
            return;
        }
        {
            std::lock_guard lock(x.mutex);
            x.stats.last_http_status = status;
            if (result == Attempt::ok)
            {
                x.stats.exports_succeeded += 1;
                x.stats.last_error = ROBOTWEAX_SRT_TELEMETRY_OK;
                x.stats.last_success_unix_nano = system_nano();
                return;
            }
            x.stats.last_error =
                status != 0 ? ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE : ROBOTWEAX_SRT_TELEMETRY_IO_FAILURE;
        }
        /* Back off 1 s, then 2 s (shorter for short intervals). */
        const auto backoff = std::chrono::milliseconds(
            std::min<std::uint32_t>(1000u << attempt, std::max<std::uint32_t>(x.config.interval_ms / 4, 50)));
        {
            std::lock_guard lock(x.mutex);
            if (result == Attempt::fail || attempt + 1 == max_attempts ||
                Clock::now() + backoff >= interval_end)
            {
                x.stats.exports_failed += 1;
                return;
            }
            x.stats.retries += 1;
        }
        std::unique_lock lock(x.mutex);
        if (x.cv.wait_for(lock, backoff, [&] { return !x.running; }))
        {
            return;
        }
    }
}

void run(Exporter &x)
{
    auto next = Clock::now() + std::chrono::milliseconds(x.config.interval_ms);
    while (x.running)
    {
        {
            std::unique_lock lock(x.mutex);
            x.cv.wait_until(lock, next, [&] { return !x.running; });
        }
        if (!x.running)
        {
            break;
        }
        export_once(x);
        /* Fixed cadence; after a long attempt, start again from now. */
        next += std::chrono::milliseconds(x.config.interval_ms);
        if (next < Clock::now())
        {
            next = Clock::now() + std::chrono::milliseconds(x.config.interval_ms);
        }
    }
}

bool printable(const char *text, const std::size_t capacity)
{
    const auto n = strnlen(text, capacity);
    if (n == capacity)
    {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c > 0x7e)
        {
            return false;
        }
    }
    return true;
}

} // namespace

extern "C" void
robotweax_srt_telemetry_otlp_exporter_config_default(robotweax_srt_telemetry_sampler *sampler,
                                                     robotweax_srt_telemetry_otlp_exporter_config_v1 *config)
{
    if (config == nullptr)
    {
        return;
    }
    *config = {};
    config->struct_size = sizeof(*config);
    config->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config->sampler = sampler;
    std::snprintf(config->endpoint, sizeof(config->endpoint), "%.*s", static_cast<int>(default_endpoint.size()),
                  default_endpoint.data());
    config->interval_ms = 10'000;
    config->timeout_ms = 5'000;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_otlp_exporter_start(const robotweax_srt_telemetry_otlp_exporter_config_v1 *config,
                                            robotweax_srt_telemetry_otlp_exporter **exporter)
{
    if (config == nullptr || exporter == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *exporter = nullptr;
    if (config->struct_size < sizeof(*config) || config->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    constexpr std::uint32_t known_flags =
        ROBOTWEAX_SRT_TELEMETRY_RENDER_NO_INDICATORS | ROBOTWEAX_SRT_TELEMETRY_RENDER_NO_COUNTERS;
    if (config->sampler == nullptr || config->reserved != 0 || (config->render_flags & ~known_flags) != 0 ||
        config->interval_ms < 100 || config->interval_ms > 3'600'000 || config->timeout_ms < 100 ||
        config->timeout_ms > 60'000 || !printable(config->endpoint, sizeof(config->endpoint)) ||
        !printable(config->headers, sizeof(config->headers)) || config->resource_attribute_count > 32 ||
        (config->resource_attribute_count > 0 && config->resource_attributes == nullptr))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    auto created = std::unique_ptr<Exporter>(new (std::nothrow) Exporter);
    if (!created)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    try
    {
        created->config = *config;
        created->config.struct_size = sizeof(created->config);
        if (!parse_endpoint(created->config.endpoint, created->endpoint) ||
            !parse_headers(created->config.headers, created->extra_headers))
        {
            return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
        }
        /* Copy the resource attributes: the caller's array may go away. */
        created->resource_storage.reserve(2 * config->resource_attribute_count);
        for (std::size_t i = 0; i < config->resource_attribute_count; ++i)
        {
            const auto &a = config->resource_attributes[i];
            if (a.key == nullptr || a.value == nullptr || a.key[0] == '\0' || strnlen(a.key, 256) > 255 ||
                strnlen(a.value, 256) > 255)
            {
                return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
            }
            created->resource_storage.emplace_back(a.key);
            created->resource_storage.emplace_back(a.value);
        }
        for (std::size_t i = 0; i < config->resource_attribute_count; ++i)
        {
            created->resource.push_back(
                {created->resource_storage[2 * i].c_str(), created->resource_storage[2 * i + 1].c_str()});
        }
        created->config.resource_attributes = nullptr;
        created->config.resource_attribute_count = created->resource.size();
        /* Check the render options once, so a bad window fails here. */
        robotweax_srt_telemetry_render_options_v1 options;
        robotweax_srt_telemetry_render_options_default(ROBOTWEAX_SRT_TELEMETRY_RENDER_OTLP_JSON, &options);
        options.flags = config->render_flags;
        options.window_nano = config->window_nano;
        std::string probe;
        const auto error = robotweax_srt_telemetry_sampler_render_otlp_to(
            config->sampler, &options, created->resource.empty() ? nullptr : created->resource.data(),
            created->resource.size(), &collect, &probe);
        if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            return error;
        }
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    created->stats.struct_size = sizeof(created->stats);
    created->stats.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    created->stats.last_error = ROBOTWEAX_SRT_TELEMETRY_OK;
    if (::pipe(created->wake) != 0)
    {
        return ROBOTWEAX_SRT_TELEMETRY_IO_FAILURE;
    }
    for (const int fd : created->wake)
    {
        static_cast<void>(::fcntl(fd, F_SETFD, FD_CLOEXEC));
    }
    try
    {
        created->thread = std::thread([x = created.get()] {
            robotweax_srt_telemetry_internal::name_current_thread("srt-tel-otlp");
            run(*x);
        });
    }
    catch (...)
    {
        ::close(created->wake[0]);
        ::close(created->wake[1]);
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    *exporter = created.release();
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_otlp_exporter_stats(robotweax_srt_telemetry_otlp_exporter *exporter,
                                            robotweax_srt_telemetry_otlp_exporter_stats_v1 *stats)
{
    if (exporter == nullptr || stats == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (stats->struct_size < sizeof(*stats) || stats->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    std::lock_guard lock(exporter->mutex);
    *stats = exporter->stats;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" void robotweax_srt_telemetry_otlp_exporter_stop(robotweax_srt_telemetry_otlp_exporter *exporter)
{
    if (exporter == nullptr)
    {
        return;
    }
    {
        std::lock_guard lock(exporter->mutex);
        exporter->running = false;
    }
    exporter->cv.notify_all();
    const char byte = 1;
    if (::write(exporter->wake[1], &byte, 1) < 0)
    {
        /* The pipe is never full here; the stop flag is set either way. */
    }
    if (exporter->thread.joinable())
    {
        exporter->thread.join();
    }
    ::close(exporter->wake[0]);
    ::close(exporter->wake[1]);
    delete exporter;
}
