#include <robotweax/srt_telemetry_http.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

int failures = 0;

void check(const bool condition, const std::string_view message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::uint64_t second = UINT64_C(1'000'000'000);
constexpr std::uint64_t base_unix = UINT64_C(1'800'000'000) * second;

struct Fake
{
    std::uint64_t packets = 0;
    robotweax_srt_telemetry_delta_engine *engine = nullptr;
};

robotweax_srt_telemetry_error fake_sample(void *data, const std::uint64_t unix_nano, const std::uint64_t monotonic_nano,
                                          robotweax_srt_telemetry_source_output_v1 *output)
{
    auto &f = *static_cast<Fake *>(data);
    f.packets += 30;
    auto &raw = output->raw;
    raw.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    raw.observed_at_unix_nano = unix_nano;
    raw.sample_monotonic_nano = monotonic_nano;
    raw.available_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_LOSS_PACKET_TOTAL |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_DROP_PACKET_TOTAL |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS | ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_SECONDS |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_TSBPD_DELAY_SECONDS;
    raw.unique_received_packet_total = f.packets;
    raw.unique_received_wire_byte_total = f.packets * 1316;
    raw.rtt_seconds = 0.02;
    raw.receive_buffer_seconds = 0.118;
    raw.receiver_tsbpd_delay_seconds = 0.12;
    return robotweax_srt_telemetry_delta_engine_update(f.engine, &raw, &output->delta);
}

int connect_to(const std::uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0)
    {
        ::close(fd);
        return -1;
    }
    timeval tv{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

void send_all(const int fd, const std::string_view data)
{
    std::size_t sent = 0;
    while (sent < data.size())
    {
        const auto n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0)
        {
            return;
        }
        sent += static_cast<std::size_t>(n);
    }
}

/* Reads until the server closes (Connection: close). */
std::string read_all(const int fd)
{
    std::string out;
    char buffer[8192];
    while (true)
    {
        const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0)
        {
            break;
        }
        out.append(buffer, static_cast<std::size_t>(n));
    }
    return out;
}

struct Response
{
    int status = 0;
    std::string head;
    std::string body;
};

Response request(const std::uint16_t port, const std::string &raw)
{
    Response r;
    const int fd = connect_to(port);
    if (fd < 0)
    {
        return r;
    }
    send_all(fd, raw);
    const auto text = read_all(fd);
    ::close(fd);
    const auto end = text.find("\r\n\r\n");
    if (text.size() > 12 && end != std::string::npos)
    {
        r.status = std::atoi(text.substr(9, 3).c_str());
        r.head = text.substr(0, end);
        r.body = text.substr(end + 4);
    }
    return r;
}

Response get(const std::uint16_t port, const std::string &target, const std::string &headers = "")
{
    return request(port, "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n" + headers +
                             "\r\n");
}

std::string upgrade_request(const std::uint16_t port, const std::string &target, const std::string &extra = "")
{
    return "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
           "\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" +
           extra + "\r\n";
}

/* Reads the 101 response head. */
std::string read_head(const int fd)
{
    std::string head;
    char c;
    while (head.find("\r\n\r\n") == std::string::npos && ::recv(fd, &c, 1, 0) == 1)
    {
        head += c;
    }
    return head;
}

struct Frame
{
    int opcode = -1;
    std::string payload;
};

bool read_exact(const int fd, char *data, std::size_t size)
{
    while (size > 0)
    {
        const auto n = ::recv(fd, data, size, 0);
        if (n <= 0)
        {
            return false;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

Frame read_frame(const int fd)
{
    Frame f;
    unsigned char h[2];
    if (!read_exact(fd, reinterpret_cast<char *>(h), 2))
    {
        return f;
    }
    std::uint64_t length = h[1] & 0x7f;
    if (length == 126)
    {
        unsigned char e[2];
        read_exact(fd, reinterpret_cast<char *>(e), 2);
        length = (static_cast<std::uint64_t>(e[0]) << 8) | e[1];
    }
    else if (length == 127)
    {
        unsigned char e[8];
        read_exact(fd, reinterpret_cast<char *>(e), 8);
        length = 0;
        for (const auto b : e)
        {
            length = (length << 8) | b;
        }
    }
    f.payload.resize(length);
    if (length > 0 && !read_exact(fd, f.payload.data(), length))
    {
        return f;
    }
    f.opcode = h[0] & 0x0f;
    return f;
}

void send_frame(const int fd, const int opcode, const std::string &payload)
{
    std::string frame;
    frame += static_cast<char>(0x80 | opcode);
    frame += static_cast<char>(0x80 | payload.size());
    const char mask[4] = {0x11, 0x22, 0x33, 0x44};
    frame.append(mask, 4);
    for (std::size_t i = 0; i < payload.size(); ++i)
    {
        frame += static_cast<char>(payload[i] ^ mask[i % 4]);
    }
    send_all(fd, frame);
}

struct Setup
{
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    Fake fake;
    std::uint64_t id = 0;
    std::uint64_t tick_index = 0;

    Setup()
    {
        robotweax_srt_telemetry_sampler_config_v1 config;
        robotweax_srt_telemetry_sampler_config_default(&config);
        robotweax_srt_telemetry_sampler_create(&config, &sampler);
        robotweax_srt_telemetry_sampler_enable_health(sampler, 1);
        robotweax_srt_telemetry_sampler_enable_history(sampler, 120);
        robotweax_srt_telemetry_sampler_enable_journal(sampler, 256);
        robotweax_srt_telemetry_delta_engine_create(&fake.engine);
        add();
        tick(125); /* 12.5 s: the first 10 s window has closed */
    }
    ~Setup()
    {
        robotweax_srt_telemetry_sampler_destroy(sampler);
        robotweax_srt_telemetry_delta_engine_destroy(fake.engine);
    }
    std::uint64_t add()
    {
        const robotweax_srt_telemetry_label_v1 labels[] = {{"site", "studio-2"}};
        robotweax_srt_telemetry_source_config_v1 cfg{};
        cfg.struct_size = sizeof(cfg);
        cfg.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        cfg.sample = &fake_sample;
        cfg.source_data = &fake;
        cfg.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;
        cfg.labels = labels;
        cfg.label_count = 1;
        std::uint64_t added = 0;
        robotweax_srt_telemetry_sampler_add_source(sampler, &cfg, &added);
        if (id == 0)
        {
            id = added;
        }
        return added;
    }
    void tick(const int count)
    {
        for (int i = 0; i < count; ++i, ++tick_index)
        {
            robotweax_srt_telemetry_sampler_tick(sampler, base_unix + tick_index * second / 10,
                                                 second + tick_index * second / 10);
        }
    }
};

void test_config()
{
    Setup s;
    robotweax_srt_telemetry_http_server_config_v1 config;
    robotweax_srt_telemetry_http_server *server = nullptr;
    robotweax_srt_telemetry_http_server_config_default(s.sampler, &config);
    std::strcpy(config.bind_address, "not-an-address");
    check(robotweax_srt_telemetry_http_server_start(&config, &server) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "bad bind address");
    robotweax_srt_telemetry_http_server_config_default(nullptr, &config);
    check(robotweax_srt_telemetry_http_server_start(&config, &server) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "sampler required");
    robotweax_srt_telemetry_http_server_config_default(s.sampler, &config);
    std::strcpy(config.bearer_token, "has space");
    check(robotweax_srt_telemetry_http_server_start(&config, &server) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "token must be printable without spaces");
    robotweax_srt_telemetry_http_server_config_default(s.sampler, &config);
    check(robotweax_srt_telemetry_http_server_start(&config, &server) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_http_server_port(server) != 0,
          "start on a free port");
    robotweax_srt_telemetry_http_server *second_server = nullptr;
    config.port = robotweax_srt_telemetry_http_server_port(server);
    check(robotweax_srt_telemetry_http_server_start(&config, &second_server) == ROBOTWEAX_SRT_TELEMETRY_IO_FAILURE,
          "port in use");
    robotweax_srt_telemetry_http_server_stop(server);
}

void test_endpoints()
{
    Setup s;
    robotweax_srt_telemetry_http_server_config_v1 config;
    robotweax_srt_telemetry_http_server_config_default(s.sampler, &config);
    robotweax_srt_telemetry_http_server *server = nullptr;
    robotweax_srt_telemetry_http_server_start(&config, &server);
    const auto port = robotweax_srt_telemetry_http_server_port(server);

    auto r = get(port, "/healthz");
    check(r.status == 200 && r.body == "ok\n", "healthz");
    r = get(port, "/metrics");
    check(r.status == 200 && r.head.find("Content-Type: text/plain; version=0.0.4") != std::string::npos &&
              r.body.find("srt_receiver_recovery_headroom_ratio{source_id=\"1\",role=\"receiver\",site=\"studio-2\"") !=
                  std::string::npos &&
              r.head.find("Content-Length: " + std::to_string(r.body.size())) != std::string::npos,
          "metrics");
    r = get(port, "/api/v1/snapshot");
    check(r.status == 200 && r.head.find("application/json") != std::string::npos &&
              r.body.find("\"schema\":\"srt-telemetry.snapshot.v1\"") != std::string::npos,
          "snapshot");
    r = get(port, "/api/v1/snapshot?window=1");
    check(r.status == 200 && r.body.find("\"window_seconds\":1,") != std::string::npos, "snapshot of the 1 s window");
    r = get(port, "/api/v1/snapshot?window=7");
    check(r.status == 400, "unconfigured window: 400");
    r = get(port, "/api/v1/snapshot?window=abc");
    check(r.status == 400, "non-numeric window: 400");
    r = get(port, "/api/v1/events");
    check(r.status == 200 && r.body.find("\"kind\":\"source_added\"") != std::string::npos &&
              r.body.find("\"overwritten\":0") != std::string::npos,
          "events");
    r = get(port, "/api/v1/events?after=1");
    check(r.status == 200 && r.body.find("\"sequence\":1,") == std::string::npos, "events after a sequence");
    r = get(port, "/api/v1/sources/1/history?since_ms=0");
    check(r.status == 200 && r.body.find("\"records\":[{\"start_unix_ms\":1800000000000") != std::string::npos &&
              r.body.find("\"srt.receiver.recovery_headroom\":{\"min\":") != std::string::npos &&
              r.body.find("\"health\":{\"verdict\":") != std::string::npos,
          "history");
    r = get(port, "/api/v1/sources/99/history?since_ms=0");
    check(r.status == 404, "history of an unknown source: 404");
    r = get(port, "/api/v1/sources/x/history");
    check(r.status == 400, "bad source id: 400");
    r = get(port, "/nowhere");
    check(r.status == 404, "unknown path: 404");
    r = request(port, "POST /metrics HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
    check(r.status == 405 && r.head.find("Allow: GET") != std::string::npos, "POST: 405");
    r = request(port, "garbage\r\n\r\n");
    check(r.status == 400, "bad request line: 400");
    r = request(port, "GET / HTTP/1.1\r\nX-Big: " + std::string(20'000, 'a') + "\r\n\r\n");
    check(r.status == 413, "oversized request: 413");

    robotweax_srt_telemetry_http_server_stop(server);
}

void test_live_feed()
{
    Setup s;
    robotweax_srt_telemetry_http_server_config_v1 config;
    robotweax_srt_telemetry_http_server_config_default(s.sampler, &config);
    config.live_interval_ms = 200;
    robotweax_srt_telemetry_http_server *server = nullptr;
    robotweax_srt_telemetry_http_server_start(&config, &server);
    const auto port = robotweax_srt_telemetry_http_server_port(server);

    /* Cross-origin pages may not read the feed. */
    const int evil = connect_to(port);
    send_all(evil, upgrade_request(port, "/api/v1/live", "Origin: http://evil.example\r\n"));
    check(read_all(evil).rfind("HTTP/1.1 403", 0) == 0, "cross-origin feed: 403");
    ::close(evil);
    const int missing = connect_to(port);
    send_all(missing, "GET /api/v1/live HTTP/1.1\r\nHost: x\r\n\r\n");
    check(read_all(missing).rfind("HTTP/1.1 400", 0) == 0, "feed without upgrade: 400");
    ::close(missing);

    const int fd = connect_to(port);
    send_all(fd, upgrade_request(port, "/api/v1/live", "Origin: http://127.0.0.1:" + std::to_string(port) + "\r\n"));
    const auto head = read_head(fd);
    check(head.rfind("HTTP/1.1 101", 0) == 0 &&
              head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos,
          "handshake (RFC 6455 example key)");
    auto f = read_frame(fd);
    check(f.opcode == 1 && f.payload.rfind("{\"type\":\"snapshot\",\"data\":{\"schema\":", 0) == 0 &&
              f.payload.back() == '}',
          "first message is a snapshot");
    const auto first = std::chrono::steady_clock::now();
    f = read_frame(fd);
    const auto gap = std::chrono::steady_clock::now() - first;
    check(f.opcode == 1 && f.payload.find("\"type\":\"snapshot\"") != std::string::npos &&
              gap > std::chrono::milliseconds(120) && gap < std::chrono::milliseconds(600),
          "snapshots every interval");

    /* New events arrive as they happen. */
    s.add();
    bool got_event = false;
    for (int i = 0; i < 10 && !got_event; ++i)
    {
        f = read_frame(fd);
        got_event = f.payload.rfind("{\"type\":\"events\"", 0) == 0 &&
                    f.payload.find("\"kind\":\"source_added\"") != std::string::npos &&
                    f.payload.find("\"source_id\":2") != std::string::npos;
    }
    check(got_event, "events pushed live");

    send_frame(fd, 0x9, "hi");
    bool pong = false;
    for (int i = 0; i < 10 && !pong; ++i)
    {
        f = read_frame(fd);
        pong = f.opcode == 0xA && f.payload == "hi";
    }
    check(pong, "ping answered with pong");
    send_frame(fd, 0x8, std::string("\x03\xe8", 2));
    bool closed = false;
    for (int i = 0; i < 10 && !closed; ++i)
    {
        f = read_frame(fd);
        closed = f.opcode == 0x8;
    }
    check(closed, "close handshake");
    ::close(fd);

    /* A client chooses the feed's window; an unconfigured one is refused. */
    const int one = connect_to(port);
    send_all(one, upgrade_request(port, "/api/v1/live?window=1"));
    check(read_head(one).rfind("HTTP/1.1 101", 0) == 0, "feed with a 1 s window");
    f = read_frame(one);
    check(f.payload.find("\"window_seconds\":1,") != std::string::npos, "feed renders the requested window");
    ::close(one);
    const int seven = connect_to(port);
    send_all(seven, upgrade_request(port, "/api/v1/live?window=7"));
    check(read_all(seven).rfind("HTTP/1.1 400", 0) == 0, "feed with an unconfigured window: 400");
    ::close(seven);

    /* Stopping the server closes feed clients cleanly. */
    const int fd2 = connect_to(port);
    send_all(fd2, upgrade_request(port, "/api/v1/live"));
    read_head(fd2);
    read_frame(fd2);
    robotweax_srt_telemetry_http_server_stop(server);
    bool going_away = false;
    for (int i = 0; i < 20 && !going_away; ++i)
    {
        f = read_frame(fd2);
        if (f.opcode < 0)
        {
            break;
        }
        going_away = f.opcode == 0x8 && f.payload.size() == 2 && static_cast<unsigned char>(f.payload[0]) == 0x03 &&
                     static_cast<unsigned char>(f.payload[1]) == 0xe9;
    }
    check(going_away, "stop sends 1001 going away");
    ::close(fd2);
}

void test_token_and_limits()
{
    Setup s;
    robotweax_srt_telemetry_http_server_config_v1 config;
    robotweax_srt_telemetry_http_server_config_default(s.sampler, &config);
    std::strcpy(config.bearer_token, "s3cret-token");
    config.max_connections = 2;
    robotweax_srt_telemetry_http_server *server = nullptr;
    robotweax_srt_telemetry_http_server_start(&config, &server);
    const auto port = robotweax_srt_telemetry_http_server_port(server);

    check(get(port, "/metrics").status == 401, "token required");
    check(get(port, "/metrics", "Authorization: Bearer wrong\r\n").status == 401, "wrong token");
    check(get(port, "/metrics", "Authorization: Bearer s3cret-token\r\n").status == 200, "right token");
    check(get(port, "/metrics?token=s3cret-token").status == 401, "query token only for the feed");
    check(get(port, "/healthz").status == 200, "healthz needs no token");
    const int fd = connect_to(port);
    send_all(fd, upgrade_request(port, "/api/v1/live?token=s3cret-token"));
    check(read_head(fd).rfind("HTTP/1.1 101", 0) == 0, "feed with a query token");

    /* Two connections are the limit: one feed plus one idle; a third gets 503. */
    const int idle = connect_to(port);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto r = get(port, "/healthz");
    check(r.status == 503, "connection limit: 503");
    ::close(idle);
    ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(get(port, "/healthz").status == 200, "served again below the limit");
    robotweax_srt_telemetry_http_server_stop(server);
}

} // namespace

int main()
{
    test_config();
    test_endpoints();
    test_live_feed();
    test_token_and_limits();
    if (failures != 0)
    {
        std::cerr << failures << " HTTP server checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all HTTP server checks passed\n";
    return EXIT_SUCCESS;
}
