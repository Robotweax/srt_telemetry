/* Embedding layer against real SRT sockets on loopback: spec parsing,
 * attach/detach, health, events, labels, linger and the HTTP server. */

#if defined(ROBOTWEAX_SRT_TELEMETRY_USE_FLAT_SRT_HEADER)
#include <srt.h>
#else
#include <srt/srt.h>
#endif
#include <robotweax/srt_telemetry_public_srt_host.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace
{

int failures = 0;

void check(const bool condition, const std::string &what)
{
    if (!condition)
    {
        ++failures;
        std::cerr << "FAIL: " << what << '\n';
    }
}

using Clock = std::chrono::steady_clock;

bool wait_until(const std::function<bool()> &condition, const int timeout_ms)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline)
    {
        if (condition())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return condition();
}

robotweax_srt_telemetry_embed_config_v1 base_config()
{
    robotweax_srt_telemetry_embed_config_v1 config;
    robotweax_srt_telemetry_embed_config_default(&config);
    robotweax_srt_telemetry_host_embed_functions(&config);
    return config;
}

void test_parse()
{
    auto parse = [](const char *spec, robotweax_srt_telemetry_embed_config_v1 &config) {
        config = base_config();
        return robotweax_srt_telemetry_embed_config_parse(&config, spec);
    };
    robotweax_srt_telemetry_embed_config_v1 c;
    check(parse("9810", c) == ROBOTWEAX_SRT_TELEMETRY_OK && std::string(c.bind_address) == "127.0.0.1" &&
              c.port == 9810,
          "bare port binds loopback");
    check(parse("0.0.0.0:9811", c) == ROBOTWEAX_SRT_TELEMETRY_OK && std::string(c.bind_address) == "0.0.0.0" &&
              c.port == 9811,
          "address and port");
    check(parse("[::]:9812,name=cam-7,token=abc,host=enc-7.lan,host=enc-7,otlp=http://col:4318,otlp_interval_ms=5000,"
                "history_s=600,linger_s=5,stream_id,no_ui",
                c) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              std::string(c.bind_address) == "::" && c.port == 9812 && std::string(c.name) == "cam-7" &&
              std::string(c.bearer_token) == "abc" &&
              std::string(c.allowed_hosts) == "enc-7.lan,enc-7" &&
              std::string(c.otlp_endpoint) == "http://col:4318" && c.otlp_interval_ms == 5000 &&
              c.history_seconds == 600 && c.linger_seconds == 5 &&
              (c.flags & ROBOTWEAX_SRT_TELEMETRY_EMBED_STREAM_ID_LABEL) != 0 &&
              (c.flags & ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_UI) != 0,
          "every option");
    check(parse(":9813", c) == ROBOTWEAX_SRT_TELEMETRY_OK && std::string(c.bind_address) == "0.0.0.0" &&
              c.port == 9813,
          "\":port\" binds every interface");
    check(parse("off,otlp=http://col:4318", c) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              (c.flags & ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP) != 0,
          "off: no HTTP server");
    const std::string token_path = "embed_test_token.txt";
    {
        std::ofstream(token_path) << "\xEF\xBB\xBF  s3cret-token \r\n";
    }
    check(parse(("9810,token_file=" + token_path).c_str(), c) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              std::string(c.bearer_token) == "s3cret-token",
          "token from a file, line end stripped");
    std::remove(token_path.c_str());
    for (const char *bad : {"", "abc", "::1:9810", "[::1]", "[::1]9810", "9810,bogus=1", "9810,token=",
                            "9810,token_file=/nonexistent/x", "65536", "9810,stream_id=1", "host:9810,linger_s=x", "off",
                            "9810,", ",9810", "9810,linger_s=99999999999"})
    {
        auto config = base_config();
        const auto before = config;
        check(robotweax_srt_telemetry_embed_config_parse(&config, bad) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT &&
                  std::memcmp(&config, &before, sizeof(config)) == 0,
              std::string("bad spec refused, config unchanged: ") + bad);
    }
}

std::string http_get(const std::uint16_t port, const std::string &path)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string response;
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
    {
        const std::string request = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        if (::send(fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()))
        {
            char buffer[65536];
            ssize_t n;
            while ((n = ::recv(fd, buffer, sizeof(buffer), 0)) > 0)
            {
                response.append(buffer, static_cast<std::size_t>(n));
            }
        }
    }
    ::close(fd);
    return response;
}

std::vector<std::uint64_t> sources(robotweax_srt_telemetry_embed *embed)
{
    std::uint64_t ids[16];
    std::size_t count = 0;
    static_cast<void>(
        robotweax_srt_telemetry_sampler_source_ids(robotweax_srt_telemetry_embed_sampler(embed), ids, 16, &count));
    return {ids, ids + count};
}

robotweax_srt_telemetry_health_status_v1 health(robotweax_srt_telemetry_embed *embed, const std::uint64_t id)
{
    robotweax_srt_telemetry_health_status_v1 status{};
    status.struct_size = sizeof(status);
    status.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    static_cast<void>(robotweax_srt_telemetry_sampler_health_read(robotweax_srt_telemetry_embed_sampler(embed), id, &status));
    return status;
}

/* A live stream: listener (receiver) and caller (sender) on loopback. */
struct Stream
{
    SRTSOCKET listener = SRT_INVALID_SOCK;
    SRTSOCKET sender = SRT_INVALID_SOCK;
    SRTSOCKET receiver = SRT_INVALID_SOCK;
    std::atomic<bool> running{true};
    std::thread send_thread;
    std::thread read_thread;

    bool open(const char *stream_id)
    {
        listener = srt_create_socket();
        sender = srt_create_socket();
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int size = sizeof(address);
        if (srt_bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SRT_ERROR ||
            srt_listen(listener, 1) == SRT_ERROR ||
            srt_getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size) == SRT_ERROR)
        {
            return false;
        }
        if (stream_id != nullptr)
        {
            srt_setsockflag(sender, SRTO_STREAMID, stream_id, static_cast<int>(std::strlen(stream_id)));
        }
        if (srt_connect(sender, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SRT_ERROR)
        {
            return false;
        }
        receiver = srt_accept(listener, nullptr, nullptr);
        return receiver != SRT_INVALID_SOCK;
    }

    void start()
    {
        send_thread = std::thread([this, socket = sender] {
            char payload[1316] = {};
            while (running && srt_sendmsg(socket, payload, sizeof(payload), -1, 1) != SRT_ERROR)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        read_thread = std::thread([this, socket = receiver] {
            char buffer[1500];
            while (running && srt_recvmsg(socket, buffer, sizeof(buffer)) != SRT_ERROR)
            {
            }
        });
    }

    void stop()
    {
        running = false;
        for (auto s : {sender, receiver, listener})
        {
            if (s != SRT_INVALID_SOCK)
            {
                srt_close(s);
            }
        }
        if (send_thread.joinable())
        {
            send_thread.join();
        }
        if (read_thread.joinable())
        {
            read_thread.join();
        }
    }
};

void test_lifecycle()
{
    check(robotweax_srt_telemetry_embed_attach(nullptr, 5, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER, nullptr, 0) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_embed_detach(nullptr, 5) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_embed_sampler(nullptr) == nullptr &&
              robotweax_srt_telemetry_embed_http_port(nullptr) == 0,
          "a NULL embed does nothing");
    robotweax_srt_telemetry_embed_stop(nullptr);

    auto config = base_config();
    check(robotweax_srt_telemetry_embed_config_parse(&config, "127.0.0.1:0,linger_s=5,stream_id") ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "spec");
    const robotweax_srt_telemetry_label_v1 site[] = {{"site", "studio-2"}};
    config.labels = site;
    config.label_count = 1;
    robotweax_srt_telemetry_embed *embed = nullptr;
    auto error = robotweax_srt_telemetry_embed_start(&config, &embed);
    check(error == ROBOTWEAX_SRT_TELEMETRY_OK && embed != nullptr, "start");
    if (embed == nullptr)
    {
        return;
    }
    const auto port = robotweax_srt_telemetry_embed_http_port(embed);
    check(port != 0, "HTTP server bound to a free port");

    /* A second embed on the same port: the bind fails cleanly. */
    {
        auto clash = base_config();
        clash.port = port;
        robotweax_srt_telemetry_embed *second = nullptr;
        check(robotweax_srt_telemetry_embed_start(&clash, &second) == ROBOTWEAX_SRT_TELEMETRY_IO_FAILURE &&
                  second == nullptr,
              "port in use: IO_FAILURE");
        auto labelled = base_config();
        labelled.port = 0;
        const robotweax_srt_telemetry_label_v1 bad[] = {{"bad key", "x"}};
        labelled.labels = bad;
        labelled.label_count = 1;
        check(robotweax_srt_telemetry_embed_start(&labelled, &second) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
              "start refuses a bad label");
        auto invalid = base_config();
        invalid.bistats = nullptr;
        check(robotweax_srt_telemetry_embed_start(&invalid, &second) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
              "missing SRT function refused");
    }

    Stream stream;
    check(stream.open("#!::r=live/cam-2,m=publish"), "stream opened");
    const robotweax_srt_telemetry_label_v1 cam[] = {{"stream", "cam-2"}};
    check(robotweax_srt_telemetry_embed_attach(embed, stream.sender, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER, cam,
                                               1) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_embed_attach(embed, stream.receiver,
                                                   ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER, cam,
                                                   1) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "attach sender and receiver");
    check(robotweax_srt_telemetry_embed_attach(embed, stream.sender, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER,
                                               nullptr, 0) == ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE,
          "attaching twice refused");
    check(robotweax_srt_telemetry_embed_attach(embed, stream.listener, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_UNKNOWN,
                                               nullptr, 0) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "unknown role refused");
    const robotweax_srt_telemetry_label_v1 many[] = {{"a", "1"}, {"b", "2"}, {"c", "3"}, {"d", "4"}, {"e", "5"}};
    check(robotweax_srt_telemetry_embed_attach(embed, stream.listener, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER,
                                               many, 5) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "too many labels refused");
    const robotweax_srt_telemetry_label_v1 bad_key[] = {{"9lives", "x"}};
    const robotweax_srt_telemetry_label_v1 bad_value[] = {{"note", "a\nb"}};
    const robotweax_srt_telemetry_label_v1 taken[] = {{"site", "other"}};
    const robotweax_srt_telemetry_label_v1 twice[] = {{"k", "1"}, {"k", "2"}};
    for (const auto &[bad, n, what] :
         {std::tuple{bad_key, 1, "a bad key"}, std::tuple{bad_value, 1, "a non-printable value"},
          std::tuple{taken, 1, "a key the embed already uses"}, std::tuple{twice, 2, "a repeated key"}})
    {
        check(robotweax_srt_telemetry_embed_attach(embed, stream.listener, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER,
                                                   bad, static_cast<std::size_t>(n)) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
              std::string("attach refuses ") + what);
    }
    check(robotweax_srt_telemetry_embed_detach(embed, stream.listener) == ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND,
          "detaching an unknown socket: NOT_FOUND");
    stream.start();

    const auto ids = sources(embed);
    check(ids.size() == 2, "two sources");
    check(wait_until(
              [&] {
                  for (const auto id : ids)
                  {
                      if (health(embed, id).verdict != ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK)
                      {
                          return false;
                      }
                  }
                  return true;
              },
              10'000),
          "both endpoints graded OK on a clean stream");

    const auto metrics = http_get(port, "/metrics");
    check(metrics.find("HTTP/1.1 200") == 0, "/metrics answers");
    check(metrics.find("srt_health_verdict{source_id=\"") != std::string::npos &&
              metrics.find("role=\"sender\"") != std::string::npos &&
              metrics.find("role=\"receiver\"") != std::string::npos,
          "/metrics has both endpoints");
    check(metrics.find("site=\"studio-2\"") != std::string::npos &&
              metrics.find("stream=\"cam-2\"") != std::string::npos &&
              metrics.find(std::string("provider=\"") +
                           (ROBOTWEAX_SRT_TELEMETRY_HOST_PROVIDER == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT
                                ? "robotweax-srt"
                                : "haivision-srt") +
                           "\"") != std::string::npos,
          "embed, host and provider labels");
    check(metrics.find("stream_id=\"#!::r=live/cam-2,m=publish\"") != std::string::npos,
          "stream ID label when enabled");
    check(http_get(port, "/").find("HTTP/1.1 200") == 0, "dashboard served");

    robotweax_srt_telemetry_event_v1 events[32];
    std::size_t count = 0;
    std::uint64_t overwritten = 0;
    static_cast<void>(robotweax_srt_telemetry_sampler_journal_read(robotweax_srt_telemetry_embed_sampler(embed), 0,
                                                                   events, 32, &count, &overwritten));
    std::size_t connected = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        connected += events[i].kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_CONNECTED ? 1 : 0;
    }
    check(connected == 2, "a CONNECTED event per endpoint");

    /* The sender goes away: detached, then closed. It stays as lost for the
     * linger (5 s), then disappears. */
    check(robotweax_srt_telemetry_embed_detach(embed, stream.sender) == ROBOTWEAX_SRT_TELEMETRY_OK, "detach sender");
    srt_close(stream.sender);
    stream.sender = SRT_INVALID_SOCK;
    check(wait_until(
              [&] {
                  const auto status = health(embed, ids[0]);
                  return status.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                         status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_CONNECTION_LOST;
              },
              2'000),
          "a detached sender shows as connection lost");
    check(sources(embed).size() == 2, "kept during the linger");
    check(wait_until([&] { return health(embed, ids[1]).reason == ROBOTWEAX_SRT_TELEMETRY_REASON_CONNECTION_LOST; },
                     8'000),
          "the receiver sees the connection lost");
    check(robotweax_srt_telemetry_embed_detach(embed, stream.receiver) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "detach receiver");
    stream.stop();

    /* The stream comes back (same role and labels) within the linger: it
     * continues the same sources, which recover. */
    Stream again;
    check(again.open("#!::r=live/cam-2,m=publish"), "stream reopened");
    check(robotweax_srt_telemetry_embed_attach(embed, again.sender, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER, cam,
                                               1) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_embed_attach(embed, again.receiver,
                                                   ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER, cam,
                                                   1) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "attach the reconnected sockets");
    again.start();
    check(sources(embed) == ids, "a reconnection continues the lingering sources");
    check(wait_until(
              [&] {
                  return health(embed, ids[0]).verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK &&
                         health(embed, ids[1]).verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK;
              },
              20'000),
          "the continued sources recover to OK");
    const auto sender_label = "{source_id=\"" + std::to_string(ids[0]) + "\"";
    check(wait_until(
              [&] {
                  const auto after = http_get(port, "/metrics");
                  const auto up = after.find("srt_source_up" + sender_label);
                  const auto line = up == std::string::npos ? std::string() : after.substr(up, after.find('\n', up) - up);
                  return line.size() > 2 && line.substr(line.size() - 2) == " 1" &&
                         after.find("srt_sender_unique_sent_bits_per_second" + sender_label) != std::string::npos;
              },
              15'000), /* rates come from the 10 s window, so up to one window after the reconnection */
          "the continued sender reads statistics and reports rates again");
    check(robotweax_srt_telemetry_embed_detach(embed, again.sender) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_embed_detach(embed, again.receiver) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "detach both");
    again.stop();
    check(sources(embed).size() == 2, "kept during the linger");
    check(wait_until([&] { return sources(embed).empty(); }, 8'000), "removed after the linger");

    /* A peer that is down: each connection attempt continues the same
     * source, which reads as lost (Failing), not as connecting. */
    auto attempt_once = [&]() {
        SRTSOCKET attempt = srt_create_socket();
        const bool no = false;
        srt_setsockflag(attempt, SRTO_RCVSYN, &no, sizeof(no));
        sockaddr_in nowhere{};
        nowhere.sin_family = AF_INET;
        nowhere.sin_port = htons(9);
        nowhere.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        srt_connect(attempt, reinterpret_cast<sockaddr *>(&nowhere), sizeof(nowhere));
        const auto attached = robotweax_srt_telemetry_embed_attach(embed, attempt,
                                                                   ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER,
                                                                   nullptr, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto detached = robotweax_srt_telemetry_embed_detach(embed, attempt);
        srt_close(attempt);
        return attached == ROBOTWEAX_SRT_TELEMETRY_OK && detached == ROBOTWEAX_SRT_TELEMETRY_OK;
    };
    check(attempt_once(), "first connection attempt attached and detached");
    const auto attempts = sources(embed);
    check(attempts.size() == 1, "a failed attempt lingers");
    check(attempt_once() && sources(embed) == attempts, "the next attempt continues the same source");
    check(!attempts.empty() && health(embed, attempts[0]).reason == ROBOTWEAX_SRT_TELEMETRY_REASON_CONNECTION_LOST,
          "a peer that stays down reads as connection lost");
    check(wait_until([&] { return sources(embed).empty(); }, 8'000), "the attempts are removed after the linger");

    /* Stop with a socket still attached; the host closes it afterwards. */
    Stream last;
    check(last.open(nullptr) &&
              robotweax_srt_telemetry_embed_attach(embed, last.sender, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER,
                                                   nullptr, 0) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "attach before stop");
    robotweax_srt_telemetry_embed_stop(embed);
    last.stop();
}

} // namespace

int main()
{
    srt_startup();
    srt_setloglevel(2);
    test_parse();
    test_lifecycle();
    srt_cleanup();
    if (failures != 0)
    {
        std::cerr << failures << " embed checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all embed checks passed\n";
    return EXIT_SUCCESS;
}
