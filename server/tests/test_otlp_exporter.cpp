/* OTLP/HTTP exporter against a fake collector on loopback. */
#include <robotweax/srt_telemetry_otlp.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
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

using Clock = std::chrono::steady_clock;
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
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS;
    raw.unique_received_packet_total = f.packets;
    raw.unique_received_wire_byte_total = f.packets * 1316;
    raw.rtt_seconds = 0.02;
    return robotweax_srt_telemetry_delta_engine_update(f.engine, &raw, &output->delta);
}

struct Setup
{
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    Fake fake;

    Setup()
    {
        robotweax_srt_telemetry_sampler_config_v1 config;
        robotweax_srt_telemetry_sampler_config_default(&config);
        robotweax_srt_telemetry_sampler_create(&config, &sampler);
        robotweax_srt_telemetry_sampler_enable_health(sampler, 1);
        robotweax_srt_telemetry_delta_engine_create(&fake.engine);
        const robotweax_srt_telemetry_label_v1 labels[] = {{"site", "studio-2"}};
        robotweax_srt_telemetry_source_config_v1 cfg{};
        cfg.struct_size = sizeof(cfg);
        cfg.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        cfg.sample = &fake_sample;
        cfg.source_data = &fake;
        cfg.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;
        cfg.labels = labels;
        cfg.label_count = 1;
        std::uint64_t id = 0;
        robotweax_srt_telemetry_sampler_add_source(sampler, &cfg, &id);
        for (std::uint64_t i = 0; i < 125; ++i)
        {
            robotweax_srt_telemetry_sampler_tick(sampler, base_unix + i * second / 10, second + i * second / 10);
        }
    }
    ~Setup()
    {
        robotweax_srt_telemetry_sampler_destroy(sampler);
        robotweax_srt_telemetry_delta_engine_destroy(fake.engine);
    }
};

/* Accepts connections, reads one request each and answers with the next
 * scripted status (200 when the script is empty). Status 0: never answer;
 * the connection is held until the collector stops. */
class Collector
{
  public:
    Collector()
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        const int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd_, reinterpret_cast<sockaddr *>(&a), sizeof(a));
        socklen_t size = sizeof(a);
        ::getsockname(fd_, reinterpret_cast<sockaddr *>(&a), &size);
        port_ = ntohs(a.sin_port);
        ::listen(fd_, 16);
        thread_ = std::thread([this] { run(); });
    }
    ~Collector()
    {
        running_ = false;
        thread_.join();
        for (const int fd : held_)
        {
            ::close(fd);
        }
        ::close(fd_);
    }
    std::uint16_t port() const
    {
        return port_;
    }
    void script(std::initializer_list<int> statuses)
    {
        std::lock_guard lock(mutex_);
        script_.insert(script_.end(), statuses);
    }
    std::vector<std::string> requests()
    {
        std::lock_guard lock(mutex_);
        return requests_;
    }
    std::size_t count()
    {
        std::lock_guard lock(mutex_);
        return requests_.size();
    }

  private:
    void run()
    {
        while (running_)
        {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0)
            {
                continue;
            }
            const int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0)
            {
                continue;
            }
            std::string text;
            char buffer[65536];
            std::size_t need = std::string::npos;
            const auto deadline = Clock::now() + std::chrono::seconds(5);
            while (Clock::now() < deadline)
            {
                pollfd q{c, POLLIN, 0};
                if (::poll(&q, 1, 50) <= 0)
                {
                    continue;
                }
                const auto n = ::recv(c, buffer, sizeof(buffer), 0);
                if (n <= 0)
                {
                    break;
                }
                text.append(buffer, static_cast<std::size_t>(n));
                const auto end = text.find("\r\n\r\n");
                if (end != std::string::npos && need == std::string::npos)
                {
                    const auto at = text.find("Content-Length: ");
                    need = end + 4 + (at == std::string::npos ? 0 : std::stoul(text.substr(at + 16)));
                }
                if (need != std::string::npos && text.size() >= need)
                {
                    break;
                }
            }
            int status = 200;
            {
                std::lock_guard lock(mutex_);
                requests_.push_back(text);
                if (!script_.empty())
                {
                    status = script_.front();
                    script_.pop_front();
                }
            }
            if (status == 0)
            {
                held_.push_back(c);
                continue;
            }
            const std::string response = "HTTP/1.1 " + std::to_string(status) +
                                         " X\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}";
            ::send(c, response.data(), response.size(), MSG_NOSIGNAL);
            ::close(c);
        }
    }

    int fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{true};
    std::thread thread_;
    std::mutex mutex_;
    std::deque<int> script_;
    std::vector<std::string> requests_;
    std::vector<int> held_;
};

robotweax_srt_telemetry_otlp_exporter_stats_v1 stats_of(robotweax_srt_telemetry_otlp_exporter *x)
{
    robotweax_srt_telemetry_otlp_exporter_stats_v1 s{};
    s.struct_size = sizeof(s);
    s.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    robotweax_srt_telemetry_otlp_exporter_stats(x, &s);
    return s;
}

template <typename Predicate> bool wait_until(Predicate p, const int ms)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < deadline)
    {
        if (p())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return p();
}

void test_config()
{
    Setup s;
    robotweax_srt_telemetry_otlp_exporter_config_v1 config;
    robotweax_srt_telemetry_otlp_exporter *x = nullptr;
    const auto start_with = [&](auto &&edit)
    {
        robotweax_srt_telemetry_otlp_exporter_config_default(s.sampler, &config);
        edit(config);
        const auto error = robotweax_srt_telemetry_otlp_exporter_start(&config, &x);
        if (error == ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            robotweax_srt_telemetry_otlp_exporter_stop(x);
        }
        return error;
    };
    robotweax_srt_telemetry_otlp_exporter_config_default(s.sampler, &config);
    check(std::string_view(config.endpoint) == "http://127.0.0.1:4318/v1/metrics" && config.interval_ms == 10'000 &&
              config.timeout_ms == 5'000,
          "defaults");
    check(start_with([](auto &) {}) == ROBOTWEAX_SRT_TELEMETRY_OK, "default config starts");
    const auto invalid = ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    check(start_with([](auto &c) { std::strcpy(c.endpoint, "https://collector:4318/v1/metrics"); }) == invalid,
          "https refused (no TLS)");
    for (const char *bad : {"collector:4318", "http://", "http://:4318", "http://host:0", "http://host:99999",
                            "http://user@host:4318", "http://ho st:4318", "http://[::1/v1/metrics",
                            "http://host:43a8/v1", "http://::1:4318/v1/metrics", "http://host%eth0:4318"})
    {
        check(start_with([&](auto &c) { std::strcpy(c.endpoint, bad); }) == invalid,
              std::string("bad endpoint refused: ") + bad);
    }
    for (const char *good : {"http://collector", "http://[::1]:4318", "http://[fe80::1%25eth0]:4318", "http://[fe80::1%eth0]:4318", "http://otel.example:4318/custom/path"})
    {
        check(start_with([&](auto &c) { std::strcpy(c.endpoint, good); }) == ROBOTWEAX_SRT_TELEMETRY_OK,
              std::string("endpoint accepted: ") + good);
    }
    for (const char *bad : {"novalue", "=x", "bad key=x", "content-length=5", "host=evil", "a=b\r\nc: d",
                            "a=b%0D%0Ac: d", "a=%7F"})
    {
        check(start_with([&](auto &c) { std::strcpy(c.headers, bad); }) == invalid,
              std::string("bad headers refused: ") + bad);
    }
    check(start_with([](auto &c) { c.interval_ms = 50; }) == invalid, "interval too short");
    check(start_with([](auto &c) { c.timeout_ms = 0; }) == invalid, "timeout too short");
    check(start_with([](auto &c) { c.render_flags = 8; }) == invalid, "unknown render flags");
    check(start_with([](auto &c) { c.window_nano = 3 * second; }) == invalid, "unconfigured window");
    check(start_with([](auto &c) { c.resource_attribute_count = 1; }) == invalid, "missing resource attributes");
    check(start_with([](auto &c) { c.sampler = nullptr; }) == invalid, "sampler required");
}

void test_export()
{
    Setup s;
    Collector collector;
    robotweax_srt_telemetry_otlp_exporter_config_v1 config;
    robotweax_srt_telemetry_otlp_exporter_config_default(s.sampler, &config);
    const std::string endpoint = "http://127.0.0.1:" + std::to_string(collector.port()) + "/v1/metrics";
    std::strcpy(config.endpoint, endpoint.c_str());
    std::strcpy(config.headers, "authorization=Bearer%20abc, x-tenant = studio, x-literal=50%");
    config.interval_ms = 200;
    robotweax_srt_telemetry_otlp_exporter *x = nullptr;
    {
        /* The caller's attribute strings go away after start. */
        std::string key = "service.name";
        std::string value = "encoder-7";
        const robotweax_srt_telemetry_label_v1 resource[] = {{key.c_str(), value.c_str()}};
        config.resource_attributes = resource;
        config.resource_attribute_count = 1;
        check(robotweax_srt_telemetry_otlp_exporter_start(&config, &x) == ROBOTWEAX_SRT_TELEMETRY_OK, "start");
        key.assign(key.size(), 'x');
        value.assign(value.size(), 'y');
    }
    check(wait_until([&] { return stats_of(x).exports_succeeded >= 2; }, 3000), "exports arrive every interval");
    const auto requests = collector.requests();
    check(!requests.empty(), "collector received requests");
    if (!requests.empty())
    {
        const auto &r = requests.front();
        check(r.rfind("POST /v1/metrics HTTP/1.1\r\n", 0) == 0, "POST to the path");
        check(r.find("\r\nHost: 127.0.0.1:" + std::to_string(collector.port()) + "\r\n") != std::string::npos,
              "Host header");
        check(r.find("\r\nContent-Type: application/json\r\n") != std::string::npos, "JSON content type");
        check(r.find("\r\nauthorization: Bearer abc\r\n") != std::string::npos &&
                  r.find("\r\nx-tenant: studio\r\n") != std::string::npos &&
                  r.find("\r\nx-literal: 50%\r\n") != std::string::npos,
              "configured headers sent");
        const auto body = r.substr(r.find("\r\n\r\n") + 4);
        check(body.rfind("{\"resourceMetrics\":", 0) == 0 &&
                  body.find("\"stringValue\":\"encoder-7\"") != std::string::npos &&
                  body.find("srt.receiver.unique_received.packets") != std::string::npos,
              "body is the OTLP request with the copied resource");
    }
    const auto st = stats_of(x);
    check(st.last_http_status == 200 && st.last_error == ROBOTWEAX_SRT_TELEMETRY_OK && st.bytes_sent > 0 &&
              st.last_success_unix_nano > 0,
          "stats after success");
    robotweax_srt_telemetry_otlp_exporter_stop(x);
}

void test_retry_and_failure()
{
    Setup s;
    Collector collector;
    robotweax_srt_telemetry_otlp_exporter_config_v1 config;
    robotweax_srt_telemetry_otlp_exporter_config_default(s.sampler, &config);
    std::strcpy(config.endpoint, ("http://127.0.0.1:" + std::to_string(collector.port())).c_str());
    config.interval_ms = 400;
    /* 503 then 200: retried within the interval. Then 400: permanent. */
    collector.script({503, 200, 400});
    robotweax_srt_telemetry_otlp_exporter *x = nullptr;
    robotweax_srt_telemetry_otlp_exporter_start(&config, &x);
    check(wait_until([&] { return stats_of(x).exports_succeeded >= 1; }, 3000), "503 is retried");
    auto st = stats_of(x);
    check(st.retries == 1 && st.exports_failed == 0, "one retry, no failure");
    check(wait_until([&] { return stats_of(x).exports_failed >= 1; }, 3000), "400 fails the export");
    st = stats_of(x);
    check(st.last_http_status == 400 && st.last_error == ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE && st.retries == 1,
          "400 is not retried");
    robotweax_srt_telemetry_otlp_exporter_stop(x);

    /* Nobody listening: connection errors, retried, then counted. */
    int closed_port = 0;
    {
        Collector gone;
        closed_port = gone.port();
    }
    std::strcpy(config.endpoint, ("http://127.0.0.1:" + std::to_string(closed_port)).c_str());
    config.interval_ms = 400; /* backoff 100 ms twice: both retries fit */
    robotweax_srt_telemetry_otlp_exporter_start(&config, &x);
    check(wait_until([&] { return stats_of(x).exports_failed >= 1; }, 3000), "connection refused: export fails");
    st = stats_of(x);
    check(st.last_error == ROBOTWEAX_SRT_TELEMETRY_IO_FAILURE && st.last_http_status == 0 && st.retries >= 2,
          "connection errors are retried and reported as I/O failures");
    robotweax_srt_telemetry_otlp_exporter_stop(x);
}

/* A host name goes through the resolver (on its own thread); localhost may
 * resolve to ::1 first, where nothing listens: the next address is tried. */
void test_host_name()
{
    Setup s;
    Collector collector;
    robotweax_srt_telemetry_otlp_exporter_config_v1 config;
    robotweax_srt_telemetry_otlp_exporter_config_default(s.sampler, &config);
    std::strcpy(config.endpoint, ("http://localhost:" + std::to_string(collector.port())).c_str());
    config.interval_ms = 200;
    robotweax_srt_telemetry_otlp_exporter *x = nullptr;
    check(robotweax_srt_telemetry_otlp_exporter_start(&config, &x) == ROBOTWEAX_SRT_TELEMETRY_OK, "host name start");
    check(wait_until([&] { return stats_of(x).exports_succeeded >= 1; }, 3000), "export to a host name");
    const auto requests = collector.requests();
    check(!requests.empty() && requests.front().find("\r\nHost: localhost:" + std::to_string(collector.port()) +
                                                     "\r\n") != std::string::npos,
          "Host header with the name");
    robotweax_srt_telemetry_otlp_exporter_stop(x);
}

void test_stop_is_prompt()
{
    Setup s;
    Collector collector;
    collector.script({0, 0, 0}); /* never answers */
    robotweax_srt_telemetry_otlp_exporter_config_v1 config;
    robotweax_srt_telemetry_otlp_exporter_config_default(s.sampler, &config);
    std::strcpy(config.endpoint, ("http://127.0.0.1:" + std::to_string(collector.port())).c_str());
    config.interval_ms = 100;
    config.timeout_ms = 30'000;
    robotweax_srt_telemetry_otlp_exporter *x = nullptr;
    robotweax_srt_telemetry_otlp_exporter_start(&config, &x);
    check(wait_until([&] { return collector.count() >= 1; }, 3000), "request sent to a silent collector");
    const auto started = Clock::now();
    robotweax_srt_telemetry_otlp_exporter_stop(x);
    const auto took = Clock::now() - started;
    check(took < std::chrono::milliseconds(500), "stop abandons a hanging attempt promptly");

    /* A silent collector with a short timeout: the attempt times out. */
    Collector silent;
    silent.script({0, 0, 0, 0});
    std::strcpy(config.endpoint, ("http://127.0.0.1:" + std::to_string(silent.port())).c_str());
    config.timeout_ms = 200;
    config.interval_ms = 1000;
    robotweax_srt_telemetry_otlp_exporter_start(&config, &x);
    check(wait_until([&] { return stats_of(x).retries >= 1; }, 3000), "timeouts are retried");
    check(stats_of(x).last_error == ROBOTWEAX_SRT_TELEMETRY_IO_FAILURE, "timeout reported as I/O failure");
    robotweax_srt_telemetry_otlp_exporter_stop(x);
}

} // namespace

int main()
{
    test_config();
    test_export();
    test_retry_and_failure();
    test_host_name();
    test_stop_is_prompt();
    if (failures != 0)
    {
        std::cerr << failures << " OTLP exporter checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all OTLP exporter checks passed\n";
    return EXIT_SUCCESS;
}
