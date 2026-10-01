#include <robotweax/srt_telemetry_http.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS: SO_NOSIGPIPE is set on each socket instead */
#endif

namespace
{

constexpr std::size_t max_request_bytes = 16 * 1024;
constexpr std::size_t max_client_frame = 64 * 1024;
constexpr std::size_t feed_skip_bytes = 4u << 20;  /* skip a snapshot above this backlog */
constexpr std::size_t feed_drop_bytes = 16u << 20; /* drop a client above this backlog */
constexpr auto idle_timeout = std::chrono::seconds(30);

using Clock = std::chrono::steady_clock;

/* ------------------------------------------------------------ SHA-1, base64 */

std::array<std::uint8_t, 20> sha1(const std::string_view data)
{
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m(data);
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    m += static_cast<char>(0x80);
    while (m.size() % 64 != 56)
    {
        m += '\0';
    }
    for (int i = 7; i >= 0; --i)
    {
        m += static_cast<char>((bits >> (i * 8)) & 0xff);
    }
    const auto rotl = [](const std::uint32_t x, const int n) { return (x << n) | (x >> (32 - n)); };
    for (std::size_t chunk = 0; chunk < m.size(); chunk += 64)
    {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
        {
            const auto *p = reinterpret_cast<const unsigned char *>(m.data() + chunk + static_cast<std::size_t>(i) * 4);
            w[i] = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
        }
        for (int i = 16; i < 80; ++i)
        {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20)
            {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            }
            else if (i < 40)
            {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            }
            else if (i < 60)
            {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            }
            else
            {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const auto t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::array<std::uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            out[static_cast<std::size_t>(i * 4 + j)] = static_cast<std::uint8_t>((h[i] >> (24 - j * 8)) & 0xff);
        }
    }
    return out;
}

std::string base64(const std::uint8_t *data, const std::size_t size)
{
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < size; i += 3)
    {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (i + 1 < size ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0) |
                                (i + 2 < size ? data[i + 2] : 0);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += i + 1 < size ? table[(n >> 6) & 63] : '=';
        out += i + 2 < size ? table[n & 63] : '=';
    }
    return out;
}

/* ----------------------------------------------------------------- helpers */

std::string lower(std::string_view text)
{
    std::string out(text);
    for (auto &c : out)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    {
        text.remove_suffix(1);
    }
    return text;
}

int hex(const char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

std::string percent_decode(const std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0)
        {
            out += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        }
        else
        {
            out += text[i] == '+' ? ' ' : text[i];
        }
    }
    return out;
}

/* Value of a query parameter; found tells whether it was present. */
std::string query_value(const std::string_view query, const std::string_view key, bool &found)
{
    found = false;
    std::size_t start = 0;
    while (start <= query.size())
    {
        const auto end = std::min(query.find('&', start), query.size());
        const auto part = query.substr(start, end - start);
        const auto eq = part.find('=');
        if (part.substr(0, eq) == key)
        {
            found = true;
            return eq == std::string_view::npos ? std::string() : percent_decode(part.substr(eq + 1));
        }
        start = end + 1;
    }
    return {};
}

bool parse_u64(const std::string &text, std::uint64_t &value)
{
    if (text.empty())
    {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool constant_time_equal(const std::string_view a, const std::string_view b)
{
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        diff |= static_cast<unsigned>(a[i] ^ b[i % std::max<std::size_t>(b.size(), 1)]);
    }
    return diff == 0;
}

void json_string(std::string &out, const std::string_view value)
{
    out += '"';
    for (const char c : value)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (byte < 0x20 || byte >= 0x7f)
        {
            char escape[8];
            std::snprintf(escape, sizeof(escape), "\\u%04x", byte);
            out += escape;
        }
        else
        {
            out += c;
        }
    }
    out += '"';
}

void json_u64(std::string &out, const std::uint64_t value)
{
    char text[24];
    const auto r = std::to_chars(text, text + sizeof(text), value);
    out.append(text, r.ptr);
}

void json_i64(std::string &out, const std::int64_t value)
{
    char text[24];
    const auto r = std::to_chars(text, text + sizeof(text), value);
    out.append(text, r.ptr);
}

void json_double(std::string &out, const double value)
{
    if (!std::isfinite(value))
    {
        out += "null";
        return;
    }
    char text[40];
    const auto r = std::to_chars(text, text + sizeof(text), value);
    out.append(text, r.ptr);
}

const char *severity_name(const robotweax_srt_telemetry_event_severity severity)
{
    switch (severity)
    {
    case ROBOTWEAX_SRT_TELEMETRY_SEVERITY_WARNING:
        return "warning";
    case ROBOTWEAX_SRT_TELEMETRY_SEVERITY_ERROR:
        return "error";
    default:
        return "info";
    }
}

void event_json(std::string &out, const robotweax_srt_telemetry_event_v1 &e)
{
    out += "{\"sequence\":";
    json_u64(out, e.sequence);
    out += ",\"observed_unix_ms\":";
    json_u64(out, e.observed_at_unix_nano / 1'000'000);
    out += ",\"source_id\":";
    json_u64(out, e.source_id);
    out += ",\"kind\":";
    json_string(out, robotweax_srt_telemetry_event_kind_name(e.kind));
    out += ",\"severity\":";
    json_string(out, severity_name(e.severity));
    out += ",\"previous\":";
    json_i64(out, e.previous_value);
    out += ",\"current\":";
    json_i64(out, e.current_value);
    out += ",\"code\":";
    json_i64(out, e.code);
    out += ",\"detail\":";
    json_string(out, std::string_view(e.detail, strnlen(e.detail, sizeof(e.detail))));
    out += '}';
}

struct CompactValue
{
    std::uint32_t bit;
    const char *name;
    const char *stat;
    float robotweax_srt_telemetry_compact_record_v1::*value;
};

const std::array<CompactValue, 12> compact_values = {{
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_UNIQUE_SENT_BPS, "srt.sender.unique_sent.bitrate", "rate",
     &robotweax_srt_telemetry_compact_record_v1::unique_sent_bits_per_second},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_RETRANSMITTED_BPS, "srt.sender.retransmitted.bitrate", "rate",
     &robotweax_srt_telemetry_compact_record_v1::retransmitted_bits_per_second},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_UNIQUE_RECEIVED_BPS, "srt.receiver.unique_received.bitrate", "rate",
     &robotweax_srt_telemetry_compact_record_v1::unique_received_bits_per_second},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_LINK_CAPACITY_BPS, "srt.connection.link_capacity_estimate", "mean",
     &robotweax_srt_telemetry_compact_record_v1::link_capacity_bits_per_second},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_RTT, "srt.connection.rtt", "mean",
     &robotweax_srt_telemetry_compact_record_v1::rtt_mean_seconds},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_RTT, "srt.connection.rtt", "max",
     &robotweax_srt_telemetry_compact_record_v1::rtt_max_seconds},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECOVERY_HEADROOM, "srt.receiver.recovery_headroom", "min",
     &robotweax_srt_telemetry_compact_record_v1::recovery_headroom_min},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECOVERY_HEADROOM, "srt.receiver.recovery_headroom", "mean",
     &robotweax_srt_telemetry_compact_record_v1::recovery_headroom_mean},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_SEND_BUFFER, "srt.sender.buffer.duration", "max",
     &robotweax_srt_telemetry_compact_record_v1::send_buffer_max_seconds},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECEIVE_BUFFER, "srt.receiver.buffer.duration", "min",
     &robotweax_srt_telemetry_compact_record_v1::receive_buffer_min_seconds},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_SEND_BUFFER_RATIO, "srt.sender.buffer.drop_threshold_utilization", "max",
     &robotweax_srt_telemetry_compact_record_v1::send_buffer_ratio_max},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_FLIGHT_TO_FLOW, "srt.sender.flow_window.utilization", "max",
     &robotweax_srt_telemetry_compact_record_v1::flight_to_flow_max},
}};

struct CompactCount
{
    std::uint32_t bit;
    const char *name;
    std::uint32_t robotweax_srt_telemetry_compact_record_v1::*value;
};

const std::array<CompactCount, 10> compact_counts = {{
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_SENDER_LOSS, "srt.sender.loss_reported.packets",
     &robotweax_srt_telemetry_compact_record_v1::sender_loss_packets},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECEIVER_LOSS, "srt.receiver.lost.packets",
     &robotweax_srt_telemetry_compact_record_v1::receiver_loss_packets},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_RETRANSMITTED, "srt.sender.retransmitted.packets",
     &robotweax_srt_telemetry_compact_record_v1::retransmitted_packets},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECEIVED_RETRANSMITTED, "srt.receiver.retransmitted.packets",
     &robotweax_srt_telemetry_compact_record_v1::received_retransmitted_packets},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_SENDER_DROP, "srt.sender.dropped.packets",
     &robotweax_srt_telemetry_compact_record_v1::sender_drop_packets},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECEIVER_DROP, "srt.receiver.dropped.packets",
     &robotweax_srt_telemetry_compact_record_v1::receiver_drop_packets},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_UNDECRYPTABLE, "srt.receiver.undecryptable.packets",
     &robotweax_srt_telemetry_compact_record_v1::undecryptable_packets},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_BELATED, "srt.receiver.belated.packets",
     &robotweax_srt_telemetry_compact_record_v1::belated_packets},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_FILTER_RECOVERED, "srt.receiver.fec_recovered.packets",
     &robotweax_srt_telemetry_compact_record_v1::filter_recovered_packets},
    {ROBOTWEAX_SRT_TELEMETRY_COMPACT_FILTER_UNRECOVERED, "srt.receiver.fec_unrecovered.packets",
     &robotweax_srt_telemetry_compact_record_v1::filter_unrecovered_packets},
}};

void record_json(std::string &out, const robotweax_srt_telemetry_compact_record_v1 &r)
{
    out += "{\"start_unix_ms\":";
    json_u64(out, r.start_unix_nano / 1'000'000);
    out += ",\"window_ms\":";
    json_u64(out, r.window_milliseconds);
    out += ",\"samples\":";
    json_u64(out, r.sample_count);
    out += ",\"quality\":";
    json_u64(out, r.quality_flags);
    out += ",\"health\":{\"verdict\":";
    json_string(out, robotweax_srt_telemetry_health_verdict_name(
                         static_cast<robotweax_srt_telemetry_health_verdict>(r.health_verdict)));
    out += ",\"reason\":";
    json_string(out, robotweax_srt_telemetry_health_reason_name(
                         static_cast<robotweax_srt_telemetry_health_reason>(r.health_reason)));
    out += "},\"metrics\":{";
    bool first_metric = true;
    const char *open = nullptr;
    for (const auto &v : compact_values)
    {
        if ((r.available & v.bit) == 0)
        {
            continue;
        }
        if (open == nullptr || std::strcmp(open, v.name) != 0)
        {
            if (open != nullptr)
            {
                out += '}';
            }
            out += first_metric ? "" : ",";
            first_metric = false;
            json_string(out, v.name);
            out += ":{";
            open = v.name;
        }
        else
        {
            out += ',';
        }
        json_string(out, v.stat);
        out += ':';
        json_double(out, static_cast<double>(r.*(v.value)));
    }
    if (open != nullptr)
    {
        out += '}';
    }
    for (const auto &c : compact_counts)
    {
        if ((r.available & c.bit) == 0)
        {
            continue;
        }
        out += first_metric ? "" : ",";
        first_metric = false;
        json_string(out, c.name);
        out += ":{\"total\":";
        json_u64(out, r.*(c.value));
        out += '}';
    }
    out += "}}";
}

/* ------------------------------------------------------------- connection */

struct Connection
{
    int fd = -1;
    std::string in;
    std::string out;
    bool websocket = false;
    bool close_after_write = false;
    /* Close at once, without draining (socket error). */
    bool abort = false;
    /* Write side shut down; unread input is discarded until the peer
     * closes or the drain time ends, so the response is not lost to a
     * reset. */
    bool draining = false;
    Clock::time_point drain_since;
    Clock::time_point last_activity = Clock::now();
    std::uint64_t events_after = 0;
    bool events_started = false;
    /* Feed window length (0: default). */
    std::uint64_t window_nano = 0;
};

void ws_frame(std::string &out, const std::uint8_t opcode, const std::string_view payload)
{
    out += static_cast<char>(0x80 | opcode);
    const auto n = payload.size();
    if (n < 126)
    {
        out += static_cast<char>(n);
    }
    else if (n <= 0xffff)
    {
        out += static_cast<char>(126);
        out += static_cast<char>((n >> 8) & 0xff);
        out += static_cast<char>(n & 0xff);
    }
    else
    {
        out += static_cast<char>(127);
        for (int i = 7; i >= 0; --i)
        {
            out += static_cast<char>((static_cast<std::uint64_t>(n) >> (i * 8)) & 0xff);
        }
    }
    out.append(payload);
}

const char *status_text(const int status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Error";
    }
}

void respond(Connection &c, const int status, const std::string_view type, const std::string_view body,
             const std::string_view extra_headers = {})
{
    std::string head = "HTTP/1.1 ";
    head += std::to_string(status);
    head += ' ';
    head += status_text(status);
    head += "\r\nContent-Type: ";
    head += type;
    head += "\r\nContent-Length: ";
    head += std::to_string(body.size());
    head += "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n";
    head += extra_headers;
    head += "\r\n";
    c.out += head;
    c.out.append(body);
    c.close_after_write = true;
}

struct Request
{
    std::string method;
    std::string path;
    std::string query;
    std::string version;
    std::vector<std::pair<std::string, std::string>> headers; /* names lower case */

    const std::string *header(const std::string_view name) const
    {
        for (const auto &h : headers)
        {
            if (h.first == name)
            {
                return &h.second;
            }
        }
        return nullptr;
    }
};

bool parse_request(const std::string_view text, Request &request)
{
    const auto line_end = text.find("\r\n");
    if (line_end == std::string_view::npos)
    {
        return false;
    }
    const auto line = text.substr(0, line_end);
    const auto s1 = line.find(' ');
    const auto s2 = line.rfind(' ');
    if (s1 == std::string_view::npos || s2 == s1)
    {
        return false;
    }
    request.method = std::string(line.substr(0, s1));
    const auto target = line.substr(s1 + 1, s2 - s1 - 1);
    request.version = std::string(line.substr(s2 + 1));
    if (request.version.rfind("HTTP/1.", 0) != 0 || target.empty() || target.front() != '/')
    {
        return false;
    }
    const auto q = target.find('?');
    request.path = std::string(target.substr(0, q));
    request.query = q == std::string_view::npos ? std::string() : std::string(target.substr(q + 1));
    std::size_t pos = line_end + 2;
    while (pos < text.size())
    {
        const auto end = text.find("\r\n", pos);
        if (end == std::string_view::npos || end == pos)
        {
            break;
        }
        const auto header = text.substr(pos, end - pos);
        const auto colon = header.find(':');
        if (colon == std::string_view::npos)
        {
            return false;
        }
        request.headers.emplace_back(lower(trim(header.substr(0, colon))), std::string(trim(header.substr(colon + 1))));
        pos = end + 2;
    }
    return true;
}

bool header_has_token(const std::string *value, const std::string_view token)
{
    if (value == nullptr)
    {
        return false;
    }
    const auto v = lower(*value);
    std::size_t start = 0;
    while (start <= v.size())
    {
        const auto end = std::min(v.find(',', start), v.size());
        if (trim(std::string_view(v).substr(start, end - start)) == token)
        {
            return true;
        }
        start = end + 1;
    }
    return false;
}

} // namespace

struct robotweax_srt_telemetry_http_server
{
    robotweax_srt_telemetry_http_server_config_v1 config{};
    int listen_fd = -1;
    int wake[2] = {-1, -1};
    std::uint16_t port = 0;
    std::atomic<bool> running{false};
    std::thread thread;
    std::vector<std::unique_ptr<Connection>> connections;
    Clock::time_point next_feed = Clock::now();
};

namespace
{

using Server = robotweax_srt_telemetry_http_server;

int collect(void *user, const char *data, const std::size_t size)
{
    static_cast<std::string *>(user)->append(data, size);
    return 0;
}

bool render(Server &server, const robotweax_srt_telemetry_render_format format, const std::uint64_t window_nano,
            std::string &out, int &status)
{
    robotweax_srt_telemetry_render_options_v1 options;
    robotweax_srt_telemetry_render_options_default(format, &options);
    options.window_nano = window_nano;
    const auto error = robotweax_srt_telemetry_sampler_render_to(server.config.sampler, &options, &collect, &out);
    if (error == ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return true;
    }
    status = error == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT ? 400 : 500;
    return false;
}

bool authorized(const Server &server, const Request &request, const bool allow_query_token)
{
    const std::string_view token = server.config.bearer_token;
    if (token.empty())
    {
        return true;
    }
    if (const auto *auth = request.header("authorization"); auth != nullptr)
    {
        const std::string_view value = *auth;
        if (value.size() > 7 && lower(value.substr(0, 7)) == "bearer " &&
            constant_time_equal(trim(value.substr(7)), token))
        {
            return true;
        }
    }
    if (allow_query_token)
    {
        bool found = false;
        const auto q = query_value(request.query, "token", found);
        return found && constant_time_equal(q, token);
    }
    return false;
}

/* A browser page from another origin must not read the feed (cross-site
 * WebSocket hijacking): the Origin, when sent, must name this host. */
bool same_origin(const Request &request)
{
    const auto *origin = request.header("origin");
    if (origin == nullptr)
    {
        return true;
    }
    const auto *host = request.header("host");
    if (host == nullptr)
    {
        return false;
    }
    std::string_view o = *origin;
    const auto scheme = o.find("://");
    if (scheme == std::string_view::npos)
    {
        return false;
    }
    o.remove_prefix(scheme + 3);
    return lower(o) == lower(*host);
}

void handle_request(Server &server, Connection &c, const Request &request)
{
    if (request.method != "GET")
    {
        respond(c, 405, "text/plain; charset=utf-8", "only GET is served\n", "Allow: GET\r\n");
        return;
    }
    const bool live = request.path == "/api/v1/live";
    if (request.path == "/healthz")
    {
        respond(c, 200, "text/plain; charset=utf-8", "ok\n");
        return;
    }
    if (!authorized(server, request, live))
    {
        respond(c, 401, "text/plain; charset=utf-8", "authorization required\n",
                "WWW-Authenticate: Bearer realm=\"srt-telemetry\"\r\n");
        return;
    }
    int status = 500;
    if (request.path == "/metrics")
    {
        std::string body;
        if (!render(server, ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS, 0, body, status))
        {
            respond(c, status, "text/plain; charset=utf-8", "render failed\n");
            return;
        }
        respond(c, 200, "text/plain; version=0.0.4; charset=utf-8", body);
        return;
    }
    if (request.path == "/api/v1/snapshot")
    {
        bool found = false;
        const auto window = query_value(request.query, "window", found);
        std::uint64_t seconds = 0;
        if (found && (!parse_u64(window, seconds) || seconds == 0 || seconds > 86'400))
        {
            respond(c, 400, "text/plain; charset=utf-8", "window must be a configured length in seconds\n");
            return;
        }
        std::string body;
        if (!render(server, ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON, seconds * 1'000'000'000ull, body, status))
        {
            respond(c, status, "text/plain; charset=utf-8",
                    status == 400 ? "window must be a configured length in seconds\n" : "render failed\n");
            return;
        }
        respond(c, 200, "application/json", body);
        return;
    }
    if (request.path == "/api/v1/events")
    {
        bool found = false;
        const auto after_text = query_value(request.query, "after", found);
        std::uint64_t after = 0;
        if (found && !parse_u64(after_text, after))
        {
            respond(c, 400, "text/plain; charset=utf-8", "after must be a sequence number\n");
            return;
        }
        std::vector<robotweax_srt_telemetry_event_v1> events(1000);
        std::size_t count = 0;
        std::uint64_t overwritten = 0;
        const auto error = robotweax_srt_telemetry_sampler_journal_read(server.config.sampler, after, events.data(),
                                                                        events.size(), &count, &overwritten);
        if (error == ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND)
        {
            respond(c, 404, "text/plain; charset=utf-8", "the event journal is not enabled\n");
            return;
        }
        if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            respond(c, 500, "text/plain; charset=utf-8", "journal read failed\n");
            return;
        }
        std::string body = "{\"overwritten\":";
        json_u64(body, overwritten);
        body += ",\"more\":";
        body += count == events.size() ? "true" : "false";
        body += ",\"events\":[";
        for (std::size_t i = 0; i < count; ++i)
        {
            body += i == 0 ? "" : ",";
            event_json(body, events[i]);
        }
        body += "]}\n";
        respond(c, 200, "application/json", body);
        return;
    }
    constexpr std::string_view sources_prefix = "/api/v1/sources/";
    constexpr std::string_view history_suffix = "/history";
    if (request.path.rfind(sources_prefix, 0) == 0 && request.path.size() > sources_prefix.size() + history_suffix.size() &&
        std::string_view(request.path).ends_with(history_suffix))
    {
        const auto id_text = request.path.substr(sources_prefix.size(),
                                                 request.path.size() - sources_prefix.size() - history_suffix.size());
        std::uint64_t id = 0;
        bool found = false;
        const auto since_text = query_value(request.query, "since_ms", found);
        std::uint64_t since_ms = 0;
        if (!parse_u64(id_text, id) || (found && !parse_u64(since_text, since_ms)))
        {
            respond(c, 400, "text/plain; charset=utf-8", "bad source id or since_ms\n");
            return;
        }
        if (!found)
        {
            /* Default: the last hour. */
            const auto now_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                               std::chrono::system_clock::now().time_since_epoch())
                                                               .count());
            since_ms = now_ms > 3'600'000 ? now_ms - 3'600'000 : 0;
        }
        std::vector<robotweax_srt_telemetry_compact_record_v1> records(86'400);
        std::size_t count = 0;
        const auto error = robotweax_srt_telemetry_sampler_history_read(
            server.config.sampler, id, since_ms * 1'000'000, records.data(), records.size(), &count);
        if (error == ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND)
        {
            respond(c, 404, "text/plain; charset=utf-8", "no such source\n");
            return;
        }
        if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            respond(c, 500, "text/plain; charset=utf-8", "history read failed\n");
            return;
        }
        std::string body = "{\"source_id\":";
        json_u64(body, id);
        body += ",\"records\":[";
        for (std::size_t i = 0; i < count; ++i)
        {
            body += i == 0 ? "" : ",";
            record_json(body, records[i]);
        }
        body += "]}\n";
        respond(c, 200, "application/json", body);
        return;
    }
    if (live)
    {
        if ((server.config.flags & ROBOTWEAX_SRT_TELEMETRY_HTTP_NO_LIVE_FEED) != 0)
        {
            respond(c, 404, "text/plain; charset=utf-8", "the live feed is disabled\n");
            return;
        }
        const auto *key = request.header("sec-websocket-key");
        const auto *version = request.header("sec-websocket-version");
        if (!header_has_token(request.header("upgrade"), "websocket") ||
            !header_has_token(request.header("connection"), "upgrade") || key == nullptr || version == nullptr ||
            *version != "13")
        {
            respond(c, 400, "text/plain; charset=utf-8", "WebSocket upgrade required (version 13)\n");
            return;
        }
        if (!same_origin(request))
        {
            respond(c, 403, "text/plain; charset=utf-8", "cross-origin feed refused\n");
            return;
        }
        bool found = false;
        const auto window = query_value(request.query, "window", found);
        std::uint64_t seconds = 0;
        if (found)
        {
            robotweax_srt_telemetry_window_summary_v1 probe{};
            if (!parse_u64(window, seconds) || seconds == 0 || seconds > 86'400 ||
                robotweax_srt_telemetry_sampler_window_read(server.config.sampler, UINT64_MAX, seconds * 1'000'000'000ull,
                                                           &probe) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT)
            {
                respond(c, 400, "text/plain; charset=utf-8", "window must be a configured length in seconds\n");
                return;
            }
        }
        c.window_nano = seconds * 1'000'000'000ull;
        const auto digest = sha1(*key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: ";
        c.out += base64(digest.data(), digest.size());
        c.out += "\r\n\r\n";
        c.websocket = true;
        /* The first snapshot goes out on the next feed tick, at once. */
        server.next_feed = Clock::now();
        return;
    }
    respond(c, 404, "text/plain; charset=utf-8", "not found\n");
}

/* Parses complete client frames. Returns false to drop the connection. */
bool handle_ws_input(Connection &c)
{
    while (c.in.size() >= 2)
    {
        const auto b0 = static_cast<std::uint8_t>(c.in[0]);
        const auto b1 = static_cast<std::uint8_t>(c.in[1]);
        const auto opcode = static_cast<std::uint8_t>(b0 & 0x0f);
        if ((b1 & 0x80) == 0)
        {
            return false; /* client frames must be masked */
        }
        std::uint64_t length = b1 & 0x7f;
        std::size_t pos = 2;
        if (length == 126)
        {
            if (c.in.size() < 4)
            {
                return true;
            }
            length = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(c.in[2])) << 8) |
                     static_cast<std::uint8_t>(c.in[3]);
            pos = 4;
        }
        else if (length == 127)
        {
            if (c.in.size() < 10)
            {
                return true;
            }
            length = 0;
            for (int i = 0; i < 8; ++i)
            {
                length = (length << 8) | static_cast<std::uint8_t>(c.in[2 + static_cast<std::size_t>(i)]);
            }
            pos = 10;
        }
        if (length > max_client_frame)
        {
            return false;
        }
        if (c.in.size() < pos + 4 + length)
        {
            return true;
        }
        std::string payload = c.in.substr(pos + 4, static_cast<std::size_t>(length));
        for (std::size_t i = 0; i < payload.size(); ++i)
        {
            payload[i] = static_cast<char>(payload[i] ^ c.in[pos + (i % 4)]);
        }
        c.in.erase(0, pos + 4 + static_cast<std::size_t>(length));
        if (opcode == 0x8) /* close: echo and finish */
        {
            ws_frame(c.out, 0x8, payload.substr(0, std::min<std::size_t>(payload.size(), 2)));
            c.close_after_write = true;
            return true;
        }
        if (opcode == 0x9) /* ping */
        {
            ws_frame(c.out, 0xA, payload);
        }
        /* Text, binary, continuation and pong frames are ignored. */
    }
    return true;
}

void feed(Server &server)
{
    bool any = false;
    for (const auto &c : server.connections)
    {
        any = any || (c->websocket && !c->close_after_write);
    }
    const auto now = Clock::now();
    if (!any)
    {
        return;
    }
    /* Events: new journal entries for each client, every loop. */
    for (const auto &c : server.connections)
    {
        if (!c->websocket || c->close_after_write)
        {
            continue;
        }
        std::vector<robotweax_srt_telemetry_event_v1> events(256);
        std::size_t count = 0;
        if (!c->events_started)
        {
            /* Start from the current end: clients load the past from
             * /api/v1/events. */
            std::uint64_t after = c->events_after;
            while (robotweax_srt_telemetry_sampler_journal_read(server.config.sampler, after, events.data(),
                                                                events.size(), &count, nullptr) ==
                       ROBOTWEAX_SRT_TELEMETRY_OK &&
                   count > 0)
            {
                after = events[count - 1].sequence;
            }
            c->events_after = after;
            c->events_started = true;
            continue;
        }
        if (robotweax_srt_telemetry_sampler_journal_read(server.config.sampler, c->events_after, events.data(),
                                                         events.size(), &count, nullptr) != ROBOTWEAX_SRT_TELEMETRY_OK ||
            count == 0)
        {
            continue;
        }
        std::string message = "{\"type\":\"events\",\"events\":[";
        for (std::size_t i = 0; i < count; ++i)
        {
            message += i == 0 ? "" : ",";
            event_json(message, events[i]);
        }
        message += "]}";
        c->events_after = events[count - 1].sequence;
        ws_frame(c->out, 0x1, message);
    }
    if (now < server.next_feed)
    {
        return;
    }
    server.next_feed = now + std::chrono::milliseconds(server.config.live_interval_ms);
    /* One rendering per requested window length. */
    std::vector<std::pair<std::uint64_t, std::string>> frames;
    for (const auto &c : server.connections)
    {
        if (!c->websocket || c->close_after_write)
        {
            continue;
        }
        const std::string *frame_for = nullptr;
        for (const auto &[window, frame] : frames)
        {
            if (window == c->window_nano)
            {
                frame_for = &frame;
            }
        }
        if (frame_for == nullptr)
        {
            std::string snapshot;
            int status = 0;
            if (!render(server, ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON, c->window_nano, snapshot, status))
            {
                continue;
            }
            while (!snapshot.empty() && snapshot.back() == '\n')
            {
                snapshot.pop_back();
            }
            std::string message = "{\"type\":\"snapshot\",\"data\":";
            message += snapshot;
            message += '}';
            std::string frame;
            ws_frame(frame, 0x1, message);
            frames.emplace_back(c->window_nano, std::move(frame));
            frame_for = &frames.back().second;
        }
        const auto &frame = *frame_for;
        if (c->out.size() > feed_drop_bytes)
        {
            c->close_after_write = true; /* too slow: let it drain, then close */
            c->out.clear();
            continue;
        }
        if (c->out.size() <= feed_skip_bytes)
        {
            c->out += frame;
        }
    }
}

void set_nonblocking(const int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    static_cast<void>(fcntl(fd, F_SETFL, flags | O_NONBLOCK));
}

void run(Server &server)
{
    std::vector<pollfd> fds;
    while (server.running.load())
    {
        fds.clear();
        fds.push_back({server.wake[0], POLLIN, 0});
        const bool accepting = server.connections.size() < server.config.max_connections + 1;
        fds.push_back({server.listen_fd, static_cast<short>(accepting ? POLLIN : 0), 0});
        for (const auto &c : server.connections)
        {
            short events = c->close_after_write && !c->draining ? 0 : POLLIN;
            if (!c->out.empty())
            {
                events = static_cast<short>(events | POLLOUT);
            }
            fds.push_back({c->fd, events, 0});
        }
        static_cast<void>(::poll(fds.data(), static_cast<nfds_t>(fds.size()), 100));
        if ((fds[0].revents & POLLIN) != 0)
        {
            char drain[64];
            while (::read(server.wake[0], drain, sizeof(drain)) > 0)
            {
            }
        }
        if (!server.running.load())
        {
            break;
        }
        const auto now = Clock::now();
        /* Existing connections: read, then write. */
        for (std::size_t i = 0; i < server.connections.size(); ++i)
        {
            auto &c = *server.connections[i];
            const auto revents = fds[i + 2].revents;
            if ((revents & (POLLERR | POLLNVAL)) != 0)
            {
                c.close_after_write = true;
                c.abort = true;
                c.out.clear();
                continue;
            }
            if (c.draining && (revents & (POLLIN | POLLHUP)) != 0)
            {
                char discard[8192];
                while (true)
                {
                    const auto n = ::recv(c.fd, discard, sizeof(discard), 0);
                    if (n > 0)
                    {
                        continue;
                    }
                    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                    {
                        c.abort = true;
                    }
                    break;
                }
                continue;
            }
            if ((revents & (POLLIN | POLLHUP)) != 0 && !c.close_after_write)
            {
                char buffer[8192];
                while (true)
                {
                    const auto n = ::recv(c.fd, buffer, sizeof(buffer), 0);
                    if (n > 0)
                    {
                        c.in.append(buffer, static_cast<std::size_t>(n));
                        c.last_activity = now;
                        if (!c.websocket && c.in.size() > max_request_bytes)
                        {
                            break;
                        }
                        continue;
                    }
                    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                    {
                        c.close_after_write = true;
                        c.abort = true;
                        c.out.clear();
                    }
                    break;
                }
                if (c.websocket)
                {
                    if (!handle_ws_input(c))
                    {
                        c.close_after_write = true;
                        c.out.clear();
                    }
                }
                else if (!c.close_after_write)
                {
                    const auto end = c.in.find("\r\n\r\n");
                    if ((end == std::string::npos && c.in.size() > max_request_bytes) ||
                        (end != std::string::npos && end + 4 > max_request_bytes))
                    {
                        respond(c, 413, "text/plain; charset=utf-8", "request too large\n");
                        c.in.clear();
                    }
                    else if (end != std::string::npos)
                    {
                        Request request;
                        if (!parse_request(std::string_view(c.in).substr(0, end + 2), request))
                        {
                            respond(c, 400, "text/plain; charset=utf-8", "bad request\n");
                        }
                        else
                        {
                            handle_request(server, c, request);
                        }
                        c.in.clear();
                    }
                }
            }
        }
        feed(server);
        for (auto &cp : server.connections)
        {
            auto &c = *cp;
            while (!c.out.empty())
            {
                const auto n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
                if (n > 0)
                {
                    c.out.erase(0, static_cast<std::size_t>(n));
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                {
                    break;
                }
                c.out.clear();
                c.close_after_write = true;
                c.abort = true;
                break;
            }
            if (!c.websocket && !c.close_after_write && now - c.last_activity > idle_timeout)
            {
                c.close_after_write = true;
            }
        }
        /* Close finished connections. */
        for (auto &c : server.connections)
        {
            if (c->close_after_write && c->out.empty() && !c->abort && !c->draining)
            {
                static_cast<void>(::shutdown(c->fd, SHUT_WR));
                c->draining = true;
                c->drain_since = now;
            }
        }
        server.connections.erase(std::remove_if(server.connections.begin(), server.connections.end(),
                                                [now](const std::unique_ptr<Connection> &c)
                                                {
                                                    const bool done =
                                                        c->abort || (c->draining && now - c->drain_since >
                                                                                        std::chrono::seconds(1));
                                                    if (done)
                                                    {
                                                        ::close(c->fd);
                                                    }
                                                    return done;
                                                }),
                                 server.connections.end());
        /* New connections. */
        if ((fds[1].revents & POLLIN) != 0)
        {
            while (true)
            {
                const int fd = ::accept(server.listen_fd, nullptr, nullptr);
                if (fd < 0)
                {
                    break;
                }
                set_nonblocking(fd);
                const int one = 1;
                static_cast<void>(::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)));
#ifdef SO_NOSIGPIPE
                static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)));
#endif
                auto c = std::make_unique<Connection>();
                c->fd = fd;
                if (server.connections.size() >= server.config.max_connections)
                {
                    respond(*c, 503, "text/plain; charset=utf-8", "too many connections\n", "Retry-After: 5\r\n");
                    static_cast<void>(::send(fd, c->out.data(), c->out.size(), MSG_NOSIGNAL));
                    ::close(fd);
                    continue;
                }
                server.connections.push_back(std::move(c));
            }
        }
    }
    for (auto &c : server.connections)
    {
        if (c->websocket)
        {
            std::string bye;
            ws_frame(bye, 0x8, std::string_view("\x03\xe9", 2)); /* 1001 going away */
            static_cast<void>(::send(c->fd, bye.data(), bye.size(), MSG_NOSIGNAL));
        }
        ::close(c->fd);
    }
    server.connections.clear();
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
        if (c < 0x21 || c > 0x7e)
        {
            return false;
        }
    }
    return true;
}

} // namespace

extern "C" void
robotweax_srt_telemetry_http_server_config_default(robotweax_srt_telemetry_sampler *sampler,
                                                   robotweax_srt_telemetry_http_server_config_v1 *config)
{
    if (config == nullptr)
    {
        return;
    }
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config->sampler = sampler;
    std::strcpy(config->bind_address, "127.0.0.1");
    config->max_connections = 32;
    config->live_interval_ms = 1000;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_http_server_start(const robotweax_srt_telemetry_http_server_config_v1 *config,
                                          robotweax_srt_telemetry_http_server **server)
{
    if (server == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *server = nullptr;
    if (config == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (config->struct_size < sizeof(*config) || config->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    if (config->sampler == nullptr || config->reserved != 0 ||
        (config->flags & ~ROBOTWEAX_SRT_TELEMETRY_HTTP_NO_LIVE_FEED) != 0 || config->max_connections == 0 ||
        config->max_connections > 1024 || config->live_interval_ms < 50 || config->live_interval_ms > 60'000 ||
        !printable(config->bind_address, sizeof(config->bind_address)) ||
        strnlen(config->bearer_token, sizeof(config->bearer_token)) == sizeof(config->bearer_token) ||
        (config->bearer_token[0] != '\0' && !printable(config->bearer_token, sizeof(config->bearer_token))))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    sockaddr_storage address{};
    socklen_t address_size = 0;
    int family = AF_INET;
    {
        auto *v4 = reinterpret_cast<sockaddr_in *>(&address);
        auto *v6 = reinterpret_cast<sockaddr_in6 *>(&address);
        if (::inet_pton(AF_INET, config->bind_address, &v4->sin_addr) == 1)
        {
            v4->sin_family = AF_INET;
            v4->sin_port = htons(config->port);
            address_size = sizeof(sockaddr_in);
        }
        else if (::inet_pton(AF_INET6, config->bind_address, &v6->sin6_addr) == 1)
        {
            family = AF_INET6;
            v6->sin6_family = AF_INET6;
            v6->sin6_port = htons(config->port);
            address_size = sizeof(sockaddr_in6);
        }
        else
        {
            return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
        }
    }
    auto created = std::unique_ptr<robotweax_srt_telemetry_http_server>(new (std::nothrow)
                                                                            robotweax_srt_telemetry_http_server);
    if (!created)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    created->config = *config;
    created->config.struct_size = sizeof(created->config);
    const auto fail = [&](const robotweax_srt_telemetry_error error)
    {
        if (created->listen_fd >= 0)
        {
            ::close(created->listen_fd);
        }
        for (const int fd : created->wake)
        {
            if (fd >= 0)
            {
                ::close(fd);
            }
        }
        return error;
    };
    created->listen_fd = ::socket(family, SOCK_STREAM, 0);
    if (created->listen_fd < 0 || ::pipe(created->wake) != 0)
    {
        return fail(ROBOTWEAX_SRT_TELEMETRY_IO_FAILURE);
    }
    const int one = 1;
    static_cast<void>(::setsockopt(created->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)));
    if (::bind(created->listen_fd, reinterpret_cast<sockaddr *>(&address), address_size) != 0 ||
        ::listen(created->listen_fd, 64) != 0)
    {
        return fail(ROBOTWEAX_SRT_TELEMETRY_IO_FAILURE);
    }
    sockaddr_storage bound{};
    socklen_t bound_size = sizeof(bound);
    static_cast<void>(::getsockname(created->listen_fd, reinterpret_cast<sockaddr *>(&bound), &bound_size));
    created->port = ntohs(family == AF_INET ? reinterpret_cast<sockaddr_in *>(&bound)->sin_port
                                            : reinterpret_cast<sockaddr_in6 *>(&bound)->sin6_port);
    set_nonblocking(created->listen_fd);
    set_nonblocking(created->wake[0]);
    set_nonblocking(created->wake[1]);
    created->running = true;
    try
    {
        auto *raw = created.get();
        created->thread = std::thread([raw] { run(*raw); });
    }
    catch (...)
    {
        return fail(ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE);
    }
    *server = created.release();
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" std::uint16_t robotweax_srt_telemetry_http_server_port(const robotweax_srt_telemetry_http_server *server)
{
    return server == nullptr ? 0 : server->port;
}

extern "C" void robotweax_srt_telemetry_http_server_stop(robotweax_srt_telemetry_http_server *server)
{
    if (server == nullptr)
    {
        return;
    }
    server->running = false;
    const char byte = 1;
    if (::write(server->wake[1], &byte, 1) < 0)
    {
        /* The pipe is never full here; the poll timeout ends the loop anyway. */
    }
    if (server->thread.joinable())
    {
        server->thread.join();
    }
    ::close(server->listen_fd);
    ::close(server->wake[0]);
    ::close(server->wake[1]);
    delete server;
}
