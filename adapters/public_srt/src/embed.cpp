#include <robotweax/srt_telemetry_embed.h>

#include <robotweax/srt_telemetry_http.h>
#include <robotweax/srt_telemetry_otlp.h>
#include <robotweax/srt_telemetry_sampler.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using SteadyClock = std::chrono::steady_clock;

constexpr std::uint16_t default_port = 9810;

std::uint64_t unix_nano()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

std::uint64_t monotonic_nano()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now().time_since_epoch()).count());
}

bool terminated(const char *text, const std::size_t capacity)
{
    return std::memchr(text, '\0', capacity) != nullptr;
}

template <std::size_t N> bool assign(char (&field)[N], const std::string_view value)
{
    if (value.size() >= N)
    {
        return false;
    }
    std::memcpy(field, value.data(), value.size());
    field[value.size()] = '\0';
    return true;
}

template <typename T> bool parse_number(const std::string_view text, T &value)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

/* "[address:]port", "[v6]:port". */
bool parse_listen(const std::string_view text, robotweax_srt_telemetry_embed_config_v1 &config)
{
    std::string_view address = "127.0.0.1";
    std::string_view port = text;
    if (!text.empty() && text.front() == '[')
    {
        const auto close = text.find(']');
        if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != ':')
        {
            return false;
        }
        address = text.substr(1, close - 1);
        port = text.substr(close + 2);
    }
    else if (const auto colon = text.rfind(':'); colon != std::string_view::npos)
    {
        /* ":port" means every interface, as in SRT URIs. */
        address = colon == 0 ? std::string_view("0.0.0.0") : text.substr(0, colon);
        port = text.substr(colon + 1);
        if (address.find(':') != std::string_view::npos)
        {
            return false; /* IPv6 needs brackets */
        }
    }
    unsigned value = 0;
    if (address.empty() || !parse_number(port, value) || value > 65535)
    {
        return false;
    }
    config.port = static_cast<std::uint16_t>(value);
    return assign(config.bind_address, address);
}

bool read_token_file(const std::string &path, robotweax_srt_telemetry_embed_config_v1 &config)
{
    std::ifstream file(path);
    std::string line;
    if (!file || !std::getline(file, line))
    {
        return false;
    }
    if (line.rfind("\xEF\xBB\xBF", 0) == 0)
    {
        line.erase(0, 3); /* UTF-8 byte order mark */
    }
    const auto first = line.find_first_not_of(" \t");
    const auto last = line.find_last_not_of(" \t\r");
    line = first == std::string::npos ? std::string() : line.substr(first, last - first + 1);
    return !line.empty() && assign(config.bearer_token, line);
}

bool printable(const char *text)
{
    for (auto *p = reinterpret_cast<const unsigned char *>(text); *p != 0; ++p)
    {
        if (*p < 0x20 || *p > 0x7e)
        {
            return false;
        }
    }
    return true;
}

const char *provider_name(const robotweax_srt_telemetry_provider provider)
{
    return provider == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT ? "robotweax-srt" : "haivision-srt";
}

/* A label value from text the peer chose (the stream ID): printable ASCII,
 * at most 255 bytes. */
std::string label_value(const char *text)
{
    std::string out;
    for (auto *p = reinterpret_cast<const unsigned char *>(text);
         *p != 0 && out.size() + 1 < ROBOTWEAX_SRT_TELEMETRY_LABEL_VALUE_CAPACITY; ++p)
    {
        out += *p >= 0x20 && *p <= 0x7e ? static_cast<char>(*p) : '?';
    }
    return out;
}

using Labels = std::vector<std::pair<std::string, std::string>>;

struct Endpoint
{
    std::int32_t socket = -1;
    robotweax_srt_telemetry_endpoint_role role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_UNKNOWN;
    /* The source's labels: a reconnection with the same role and labels
     * continues a lingering source. */
    std::vector<std::pair<std::string, std::string>> labels;
    robotweax_srt_telemetry_public_srt_session *session = nullptr;
    robotweax_srt_telemetry_public_srt_connection_monitor *monitor = nullptr;
    std::uint64_t source_id = 0;
    bool added = false;
    robotweax_srt_telemetry_health_context_v1 context{};
    robotweax_srt_telemetry_health_context_v1 applied{};
    bool has_applied = false;
    /* Continued after a loss: shown as lost until it connects again. */
    bool reconnecting = false;
    /* The current socket has connected; the latest rejection recorded. */
    bool socket_connected = false;
    std::int64_t last_rejection = 0;
    /* Detached: no further SRT calls; shown as lost until expires. */
    bool retired = false;
    SteadyClock::time_point expires;

    ~Endpoint()
    {
        if (monitor != nullptr)
        {
            robotweax_srt_telemetry_public_srt_connection_monitor_destroy(monitor);
        }
        if (session != nullptr)
        {
            robotweax_srt_telemetry_public_srt_session_destroy(session);
        }
    }
};

/* Sampler source: reads the socket's statistics unless it was detached. */
robotweax_srt_telemetry_error endpoint_sample(void *data, const std::uint64_t observed_at_unix_nano,
                                              const std::uint64_t sample_monotonic_nano,
                                              robotweax_srt_telemetry_source_output_v1 *output)
{
    auto &endpoint = *static_cast<Endpoint *>(data);
    if (endpoint.retired)
    {
        return ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE;
    }
    return robotweax_srt_telemetry_public_srt_source_sample(endpoint.session, observed_at_unix_nano,
                                                            sample_monotonic_nano, output);
}

bool same_context(const robotweax_srt_telemetry_health_context_v1 &a,
                  const robotweax_srt_telemetry_health_context_v1 &b)
{
    return a.connection == b.connection && a.flags == b.flags && a.overhead_percent == b.overhead_percent &&
           a.max_bandwidth_bytes_per_second == b.max_bandwidth_bytes_per_second;
}

} // namespace

struct robotweax_srt_telemetry_embed
{
    robotweax_srt_telemetry_embed_config_v1 config{};
    Labels labels;
    std::string service_name;
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    robotweax_srt_telemetry_http_server *server = nullptr;
    robotweax_srt_telemetry_otlp_exporter *exporter = nullptr;
    std::uint64_t period_nano = 100'000'000;

    /* Guards the endpoints and serializes ticks with attach and detach. */
    std::mutex mutex;
    std::condition_variable cv;
    bool running = true;
    std::thread thread;
    std::map<std::int32_t, std::unique_ptr<Endpoint>> active;
    std::vector<std::unique_ptr<Endpoint>> retired;
};

namespace
{

using Embed = robotweax_srt_telemetry_embed;

void record(Embed &embed, Endpoint &endpoint, const robotweax_srt_telemetry_public_srt_connection_result_v1 &result)
{
    robotweax_srt_telemetry_event_v1 events[5];
    std::size_t count = 0;
    if (robotweax_srt_telemetry_public_srt_connection_events(&result, endpoint.source_id, events, 5, &count) ==
        ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            /* A peer that is down or refuses us yields a rejection, and KM
             * and settings changes, on every attempt: keep the first
             * rejection per reason and the changes of connected sockets. */
            const auto kind = events[i].kind;
            if (kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_CONNECTED)
            {
                endpoint.socket_connected = true;
                endpoint.last_rejection = 0;
            }
            else if (kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_REJECTED)
            {
                if (events[i].current_value == endpoint.last_rejection)
                {
                    continue;
                }
                endpoint.last_rejection = events[i].current_value;
            }
            else if (!endpoint.socket_connected && (kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_KM_STATE_CHANGED ||
                                                    kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_SETTINGS_CHANGED ||
                                                    kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_PEER_VERSION_CHANGED))
            {
                continue;
            }
            static_cast<void>(robotweax_srt_telemetry_sampler_record_event(embed.sampler, &events[i]));
        }
    }
}

void apply_context(Embed &embed, Endpoint &endpoint)
{
    if (endpoint.has_applied && same_context(endpoint.applied, endpoint.context))
    {
        return;
    }
    if (robotweax_srt_telemetry_sampler_set_health_context(embed.sampler, endpoint.source_id, &endpoint.context) ==
        ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        endpoint.applied = endpoint.context;
        endpoint.has_applied = true;
    }
}

/* Polls the connection monitor: events, health context. Lock held. */
void poll(Embed &embed, Endpoint &endpoint, const std::uint64_t wall, const std::uint64_t mono,
          robotweax_srt_telemetry_public_srt_connection_result_v1 *out = nullptr)
{
    robotweax_srt_telemetry_public_srt_connection_result_v1 result{};
    result.struct_size = sizeof(result);
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    if (robotweax_srt_telemetry_public_srt_connection_monitor_poll(endpoint.monitor, wall, mono, &result) !=
        ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return;
    }
    if (endpoint.added)
    {
        record(embed, endpoint, result);
    }
    static_cast<void>(robotweax_srt_telemetry_public_srt_health_context(&result, &endpoint.context));
    if (endpoint.context.connection == ROBOTWEAX_SRT_TELEMETRY_CONNECTION_CONNECTED)
    {
        endpoint.reconnecting = false;
    }
    else if (endpoint.reconnecting && endpoint.context.connection == ROBOTWEAX_SRT_TELEMETRY_CONNECTION_CONNECTING)
    {
        /* A new attempt on a lost source: still lost until it connects, so
         * a peer that stays down reads as Failing, not as connecting. */
        endpoint.context.connection = ROBOTWEAX_SRT_TELEMETRY_CONNECTION_LOST;
    }
    if (endpoint.added)
    {
        apply_context(embed, endpoint);
    }
    if (out != nullptr)
    {
        *out = result;
    }
}

void remove(Embed &embed, Endpoint &endpoint)
{
    if (endpoint.added)
    {
        static_cast<void>(robotweax_srt_telemetry_sampler_remove_source(embed.sampler, endpoint.source_id));
        endpoint.added = false;
    }
}

void run(Embed &embed)
{
    const auto period = std::chrono::nanoseconds(embed.period_nano);
    auto next = SteadyClock::now() + period;
    std::unique_lock lock(embed.mutex);
    while (true)
    {
        embed.cv.wait_until(lock, next, [&] { return !embed.running; });
        if (!embed.running)
        {
            return;
        }
        const auto now = SteadyClock::now();
        const auto wall = unix_nano();
        const auto mono = monotonic_nano();
        for (auto &[socket, endpoint] : embed.active)
        {
            poll(embed, *endpoint, wall, mono);
        }
        for (auto it = embed.retired.begin(); it != embed.retired.end();)
        {
            if (now >= (*it)->expires)
            {
                remove(embed, **it);
                it = embed.retired.erase(it);
            }
            else
            {
                ++it;
            }
        }
        static_cast<void>(robotweax_srt_telemetry_sampler_tick(embed.sampler, wall, mono));
        /* Fixed cadence; after an overrun, continue from now (the windows
         * flag the gap). */
        next += period;
        if (next <= SteadyClock::now())
        {
            next = SteadyClock::now() + period;
        }
    }
}

/* The sampler's label rules, checked here so that a bad label fails start
 * or attach rather than every later add_source: keys [A-Za-z_][A-Za-z0-9_]*
 * under 64 bytes, values printable ASCII under 256 bytes, keys unique
 * (also against the labels already in use). */
bool valid_labels(const robotweax_srt_telemetry_label_v1 *labels, const std::size_t count,
                  const std::vector<std::pair<std::string, std::string>> &existing = {})
{
    if (count > 0 && labels == nullptr)
    {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        const char *key = labels[i].key;
        const char *value = labels[i].value;
        if (key == nullptr || value == nullptr || key[0] == '\0' ||
            std::strlen(key) >= ROBOTWEAX_SRT_TELEMETRY_LABEL_KEY_CAPACITY ||
            std::strlen(value) >= ROBOTWEAX_SRT_TELEMETRY_LABEL_VALUE_CAPACITY || !printable(value))
        {
            return false;
        }
        for (const char *c = key; *c != '\0'; ++c)
        {
            const bool letter = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || *c == '_';
            if (!(letter || (c != key && *c >= '0' && *c <= '9')))
            {
                return false;
            }
        }
        for (std::size_t j = 0; j < i; ++j)
        {
            if (std::strcmp(labels[j].key, key) == 0)
            {
                return false;
            }
        }
        for (const auto &label : existing)
        {
            if (label.first == key)
            {
                return false;
            }
        }
    }
    return true;
}

bool has_key(const Labels &labels, const std::string_view key)
{
    return std::any_of(labels.begin(), labels.end(), [&](const auto &label) { return label.first == key; });
}

void teardown(Embed *embed)
{
    if (embed->thread.joinable())
    {
        {
            std::lock_guard lock(embed->mutex);
            embed->running = false;
        }
        embed->cv.notify_all();
        embed->thread.join();
    }
    /* The exporter and the server read the sampler: stop them first. */
    robotweax_srt_telemetry_otlp_exporter_stop(embed->exporter);
    robotweax_srt_telemetry_http_server_stop(embed->server);
    if (embed->sampler != nullptr)
    {
        for (auto &[socket, endpoint] : embed->active)
        {
            remove(*embed, *endpoint);
        }
        for (auto &endpoint : embed->retired)
        {
            remove(*embed, *endpoint);
        }
    }
    embed->active.clear();
    embed->retired.clear();
    robotweax_srt_telemetry_sampler_destroy(embed->sampler);
    delete embed;
}

} // namespace

extern "C" void robotweax_srt_telemetry_embed_config_default(robotweax_srt_telemetry_embed_config_v1 *config)
{
    if (config == nullptr)
    {
        return;
    }
    *config = {};
    config->struct_size = sizeof(*config);
    config->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config->provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
    std::snprintf(config->bind_address, sizeof(config->bind_address), "127.0.0.1");
    config->port = default_port;
    config->otlp_interval_ms = 10'000;
    config->history_seconds = 3600;
    config->journal_capacity = 4096;
    config->linger_seconds = 60;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_embed_config_parse(robotweax_srt_telemetry_embed_config_v1 *config, const char *spec)
{
    if (config == nullptr || spec == nullptr || config->struct_size != sizeof(*config))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    /* Work on a copy: a bad spec leaves the configuration unchanged. */
    auto parsed = *config;
    std::string hosts;
    const std::string_view text = spec;
    std::size_t start = 0;
    bool first = true;
    while (start <= text.size())
    {
        auto end = text.find(',', start);
        end = end == std::string_view::npos ? text.size() : end;
        const auto item = text.substr(start, end - start);
        start = end + 1;
        if (first)
        {
            first = false;
            if (item == "off")
            {
                parsed.flags |= ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP;
            }
            else if (!parse_listen(item, parsed))
            {
                return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
            }
            continue;
        }
        const auto eq = item.find('=');
        const auto key = item.substr(0, eq);
        const auto value = eq == std::string_view::npos ? std::string_view{} : item.substr(eq + 1);
        const bool has_value = eq != std::string_view::npos && !value.empty();
        bool ok = true;
        if (key == "name" && has_value)
        {
            ok = assign(parsed.name, value);
        }
        else if (key == "token" && has_value)
        {
            ok = assign(parsed.bearer_token, value);
        }
        else if (key == "token_file" && has_value)
        {
            ok = read_token_file(std::string(value), parsed);
        }
        else if (key == "host" && has_value)
        {
            hosts += hosts.empty() ? "" : ",";
            hosts += value;
        }
        else if (key == "otlp" && has_value)
        {
            ok = assign(parsed.otlp_endpoint, value);
        }
        else if (key == "otlp_interval_ms" && has_value)
        {
            ok = parse_number(value, parsed.otlp_interval_ms);
        }
        else if (key == "history_s" && has_value)
        {
            ok = parse_number(value, parsed.history_seconds);
        }
        else if (key == "linger_s" && has_value)
        {
            ok = parse_number(value, parsed.linger_seconds);
        }
        else if (key == "stream_id" && eq == std::string_view::npos)
        {
            parsed.flags |= ROBOTWEAX_SRT_TELEMETRY_EMBED_STREAM_ID_LABEL;
        }
        else if (key == "no_ui" && eq == std::string_view::npos)
        {
            parsed.flags |= ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_UI;
        }
        else
        {
            ok = false; /* unknown option, or one without its value */
        }
        if (!ok)
        {
            return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
        }
    }
    if (!hosts.empty() && !assign(parsed.allowed_hosts, hosts))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if ((parsed.flags & ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP) != 0 && parsed.otlp_endpoint[0] == '\0')
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT; /* off without otlp= would output nothing */
    }
    *config = parsed;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_embed_start(const robotweax_srt_telemetry_embed_config_v1 *config,
                                    robotweax_srt_telemetry_embed **embed)
{
    if (embed == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *embed = nullptr;
    if (config == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (config->struct_size != sizeof(*config) || config->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    if ((config->provider != ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT &&
         config->provider != ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT) ||
        config->bistats == nullptr || config->getsockstate == nullptr || config->getrejectreason == nullptr ||
        config->getsockflag == nullptr || config->reserved != 0 ||
        !terminated(config->bind_address, sizeof(config->bind_address)) ||
        !terminated(config->bearer_token, sizeof(config->bearer_token)) ||
        !terminated(config->allowed_hosts, sizeof(config->allowed_hosts)) ||
        !terminated(config->otlp_endpoint, sizeof(config->otlp_endpoint)) ||
        !terminated(config->service_name, sizeof(config->service_name)) || !printable(config->service_name) ||
        !terminated(config->name, sizeof(config->name)) || !printable(config->name) ||
        config->label_count > ROBOTWEAX_SRT_TELEMETRY_EMBED_MAX_HOST_LABELS ||
        !valid_labels(config->labels, config->label_count) ||
        (config->flags & ~(ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP | ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_UI |
                           ROBOTWEAX_SRT_TELEMETRY_EMBED_STREAM_ID_LABEL)) != 0)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }

    Embed *created = nullptr;
    try
    {
        created = new Embed;
        created->config = *config;
        created->config.labels = nullptr;
        created->config.label_count = 0;
        for (std::size_t i = 0; i < config->label_count; ++i)
        {
            created->labels.emplace_back(config->labels[i].key, config->labels[i].value);
        }
        created->service_name = config->service_name[0] != '\0' ? config->service_name : "srt-telemetry";
    }
    catch (const std::bad_alloc &)
    {
        delete created;
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }

    robotweax_srt_telemetry_sampler_config_v1 sampler_config{};
    robotweax_srt_telemetry_sampler_config_default(&sampler_config);
    created->period_nano = sampler_config.period_nano;
    auto error = robotweax_srt_telemetry_sampler_create(&sampler_config, &created->sampler);
    if (error == ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        error = robotweax_srt_telemetry_sampler_enable_health(created->sampler, 1);
    }
    if (error == ROBOTWEAX_SRT_TELEMETRY_OK && config->history_seconds > 0)
    {
        error = robotweax_srt_telemetry_sampler_enable_history(created->sampler, config->history_seconds);
    }
    if (error == ROBOTWEAX_SRT_TELEMETRY_OK && config->journal_capacity > 0)
    {
        error = robotweax_srt_telemetry_sampler_enable_journal(created->sampler, config->journal_capacity);
    }
    if (error == ROBOTWEAX_SRT_TELEMETRY_OK && (config->flags & ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP) == 0)
    {
        robotweax_srt_telemetry_http_server_config_v1 http{};
        robotweax_srt_telemetry_http_server_config_default(created->sampler, &http);
        if (!assign(http.bind_address, config->bind_address) || !assign(http.bearer_token, config->bearer_token) ||
            !assign(http.allowed_hosts, config->allowed_hosts))
        {
            error = ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
        }
        else
        {
            http.port = config->port;
            if ((config->flags & ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_UI) != 0)
            {
                http.flags |= ROBOTWEAX_SRT_TELEMETRY_HTTP_NO_UI;
            }
            error = robotweax_srt_telemetry_http_server_start(&http, &created->server);
        }
    }
    if (error == ROBOTWEAX_SRT_TELEMETRY_OK && config->otlp_endpoint[0] != '\0')
    {
        const robotweax_srt_telemetry_label_v1 resource[] = {{"service.name", created->service_name.c_str()}};
        robotweax_srt_telemetry_otlp_exporter_config_v1 otlp{};
        robotweax_srt_telemetry_otlp_exporter_config_default(created->sampler, &otlp);
        if (!assign(otlp.endpoint, config->otlp_endpoint))
        {
            error = ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
        }
        else
        {
            otlp.interval_ms = config->otlp_interval_ms;
            otlp.resource_attributes = resource;
            otlp.resource_attribute_count = 1;
            error = robotweax_srt_telemetry_otlp_exporter_start(&otlp, &created->exporter);
        }
    }
    if (error == ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        try
        {
            created->thread = std::thread([created] { run(*created); });
        }
        catch (const std::exception &)
        {
            error = ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
        }
    }
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        teardown(created);
        return error;
    }
    *embed = created;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_embed_attach(robotweax_srt_telemetry_embed *embed, const std::int32_t socket,
                                     const robotweax_srt_telemetry_endpoint_role role,
                                     const robotweax_srt_telemetry_label_v1 *labels, const std::size_t label_count)
{
    if (embed == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_OK;
    }
    if ((role != ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER && role != ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER) ||
        !valid_labels(labels, label_count, embed->labels) ||
        embed->labels.size() + label_count > ROBOTWEAX_SRT_TELEMETRY_EMBED_MAX_HOST_LABELS)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    std::lock_guard lock(embed->mutex);
    if (embed->active.count(socket) != 0)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    std::unique_ptr<Endpoint> endpoint;
    Labels all;
    try
    {
        endpoint = std::make_unique<Endpoint>();
        all = embed->labels;
        for (std::size_t i = 0; i < label_count; ++i)
        {
            all.emplace_back(labels[i].key, labels[i].value);
        }
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    endpoint->socket = socket;
    const auto &config = embed->config;

    robotweax_srt_telemetry_public_srt_config_v2 session{};
    session.struct_size = sizeof(session);
    session.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    session.socket = socket;
    session.provider = config.provider;
    session.capabilities = ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1 | ROBOTWEAX_SRT_TELEMETRY_CAP_FEC_STATS_V1;
    session.qualified_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_V1_5_5_CANDIDATE_FIELDS;
    session.qualified_ext_fields = config.provider == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT
                                       ? ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_ROBOTWEAX_EXT_CANDIDATE_FIELDS
                                       : ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_HAIVISION_V1_5_5_EXT_CANDIDATE_FIELDS;
    session.bistats = config.bistats;
    auto error = robotweax_srt_telemetry_public_srt_session_create_v2(&session, &endpoint->session);
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return error;
    }
    robotweax_srt_telemetry_public_srt_connection_config_v1 monitor{};
    monitor.struct_size = sizeof(monitor);
    monitor.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    monitor.socket = socket;
    monitor.provider = config.provider;
    monitor.getsockstate = config.getsockstate;
    monitor.getrejectreason = config.getrejectreason;
    monitor.getsockflag = config.getsockflag;
    monitor.capture_stream_id = (config.flags & ROBOTWEAX_SRT_TELEMETRY_EMBED_STREAM_ID_LABEL) != 0 ? 1 : 0;
    error = robotweax_srt_telemetry_public_srt_connection_monitor_create(&monitor, &endpoint->monitor);
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return error;
    }
    robotweax_srt_telemetry_health_context_init(&endpoint->context);

    /* First poll: the state and, when captured, the stream ID for a label.
     * Its events (CONNECTED or REJECTED) are recorded once the source has an
     * ID. */
    robotweax_srt_telemetry_public_srt_connection_result_v1 first{};
    poll(*embed, *endpoint, unix_nano(), monotonic_nano(), &first);
    try
    {
        if (config.name[0] != '\0' && !has_key(all, "stream"))
        {
            all.emplace_back("stream", config.name);
        }
        if (!has_key(all, "provider"))
        {
            all.emplace_back("provider", provider_name(config.provider));
        }
        if ((first.snapshot.available_fields & ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_STREAM_ID) != 0 &&
            first.snapshot.stream_id[0] != '\0' && !has_key(all, "stream_id"))
        {
            all.emplace_back("stream_id", label_value(first.snapshot.stream_id));
        }
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    endpoint->role = role;
    endpoint->labels = std::move(all);

    /* A reconnection: the same role and labels as a lingering source. It
     * continues that source (one row, continuous history) on the new
     * socket; the counters restart there, which the windows flag. */
    for (auto it = embed->retired.rbegin(); it != embed->retired.rend(); ++it)
    {
        auto &lingering = **it;
        if (lingering.role != role || lingering.labels != endpoint->labels)
        {
            continue;
        }
        std::swap(lingering.session, endpoint->session);
        std::swap(lingering.monitor, endpoint->monitor);
        lingering.socket = socket;
        lingering.context = endpoint->context;
        lingering.reconnecting = endpoint->context.connection != ROBOTWEAX_SRT_TELEMETRY_CONNECTION_CONNECTED;
        if (lingering.reconnecting)
        {
            lingering.context.connection = ROBOTWEAX_SRT_TELEMETRY_CONNECTION_LOST;
        }
        lingering.socket_connected = false;
        lingering.retired = false;
        try
        {
            embed->active.emplace(socket, std::move(*it));
        }
        catch (const std::bad_alloc &)
        {
            /* Not continued: drop it rather than leave it half-switched. */
            remove(*embed, lingering);
            embed->retired.erase(std::next(it).base());
            return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
        }
        embed->retired.erase(std::next(it).base());
        auto &continued = *embed->active.at(socket);
        record(*embed, continued, first);
        apply_context(*embed, continued);
        return ROBOTWEAX_SRT_TELEMETRY_OK;
    }

    std::vector<robotweax_srt_telemetry_label_v1> view;
    try
    {
        for (const auto &[key, value] : endpoint->labels)
        {
            view.push_back({key.c_str(), value.c_str()});
        }
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    robotweax_srt_telemetry_source_config_v1 source{};
    robotweax_srt_telemetry_public_srt_source_config(endpoint->session, role, view.data(),
                                                     static_cast<std::uint32_t>(view.size()), &source);
    source.sample = &endpoint_sample;
    source.source_data = endpoint.get();
    error = robotweax_srt_telemetry_sampler_add_source(embed->sampler, &source, &endpoint->source_id);
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return error;
    }
    endpoint->added = true;
    record(*embed, *endpoint, first);
    apply_context(*embed, *endpoint);
    try
    {
        embed->active.emplace(socket, std::move(endpoint));
    }
    catch (const std::bad_alloc &)
    {
        remove(*embed, *endpoint);
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_embed_detach(robotweax_srt_telemetry_embed *embed,
                                                                             const std::int32_t socket)
{
    if (embed == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_OK;
    }
    std::lock_guard lock(embed->mutex);
    const auto found = embed->active.find(socket);
    if (found == embed->active.end())
    {
        return ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND;
    }
    auto endpoint = std::move(found->second);
    embed->active.erase(found);
    /* A last look while the socket still exists: a break or rejection the
     * ticks have not seen yet gets its event. */
    poll(*embed, *endpoint, unix_nano(), monotonic_nano());
    if (embed->config.linger_seconds == 0)
    {
        remove(*embed, *endpoint);
        return ROBOTWEAX_SRT_TELEMETRY_OK;
    }
    /* Shown as lost until the linger ends; the socket is not read again. */
    endpoint->retired = true;
    endpoint->context.connection = ROBOTWEAX_SRT_TELEMETRY_CONNECTION_LOST;
    apply_context(*embed, *endpoint);
    endpoint->expires = SteadyClock::now() + std::chrono::seconds(embed->config.linger_seconds);
    try
    {
        embed->retired.push_back(std::move(endpoint));
    }
    catch (const std::bad_alloc &)
    {
        remove(*embed, *endpoint);
    }
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_sampler *robotweax_srt_telemetry_embed_sampler(robotweax_srt_telemetry_embed *embed)
{
    return embed == nullptr ? nullptr : embed->sampler;
}

extern "C" std::uint16_t robotweax_srt_telemetry_embed_http_port(robotweax_srt_telemetry_embed *embed)
{
    return embed == nullptr || embed->server == nullptr ? 0 : robotweax_srt_telemetry_http_server_port(embed->server);
}

extern "C" void robotweax_srt_telemetry_embed_stop(robotweax_srt_telemetry_embed *embed)
{
    if (embed != nullptr)
    {
        teardown(embed);
    }
}
