/* Embedding layer against real SRT sockets on loopback: spec parsing,
 * attach/detach, health, events, labels, linger, the HTTP server and the
 * JSONL recordings. */

#if defined(ROBOTWEAX_SRT_TELEMETRY_USE_FLAT_SRT_HEADER)
#include <srt.h>
#else
#include <srt/srt.h>
#endif
#include <robotweax/srt_telemetry_public_srt_host.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
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
    check(parse("off,jsonl=/var/lib/srt,jsonl_s=3600,provider_revision=899348d8", c) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              (c.flags & ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP) != 0 &&
              std::string(c.recording_directory) == "/var/lib/srt" && c.recording_seconds == 3600 &&
              std::string(c.provider_revision) == "899348d8",
          "off with recordings only");
    check(parse("9810", c) == ROBOTWEAX_SRT_TELEMETRY_OK && c.recording_directory[0] == '\0' &&
              c.recording_seconds == 600 && c.provider_version[0] != '\0',
          "recordings off by default; 600 s ring; provider version from srt.h");
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
                            "9810,", ",9810", "9810,linger_s=99999999999", "9810,jsonl=",
                            "9810,jsonl_s=0", "9810,jsonl_s=36001", "9810,provider_revision="})
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

/* Value of a top-level numeric or string field in one JSON line (enough for
 * the recorder's flat header). */
std::string json_field(const std::string &line, const std::string &key)
{
    const auto at = line.find("\"" + key + "\":");
    if (at == std::string::npos)
    {
        return {};
    }
    auto start = at + key.size() + 3;
    if (line[start] == '"')
    {
        ++start;
        return line.substr(start, line.find('"', start) - start);
    }
    return line.substr(start, line.find_first_of(",}", start) - start);
}

std::vector<std::string> read_lines(const std::filesystem::path &path)
{
    std::vector<std::string> lines;
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);)
    {
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::filesystem::path> recordings_in(const std::filesystem::path &directory)
{
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
    {
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

/* A recording is a complete recorder export: a 1alpha2 header (the embed's
 * default), then exactly recordCount samples of the same stream, oldest
 * first, and healthCount health lines, each after the sample it was graded
 * on. */
bool complete_recording(const std::vector<std::string> &lines, const std::string &role, std::size_t &samples)
{
    if (lines.empty() || json_field(lines[0], "kind") != "robotweax-srt-telemetry-recording" ||
        json_field(lines[0], "schemaVersion") != "1alpha2" || json_field(lines[0], "role") != role ||
        json_field(lines[0], "streamId") != "cam-9" || json_field(lines[0], "flowId") != "studio-link" ||
        json_field(lines[0], "groupRow") != "null" || json_field(lines[0], "healthLines") != "true")
    {
        return false;
    }
    samples = 0;
    std::size_t health = 0;
    std::uint64_t previous = 0;
    for (std::size_t i = 1; i < lines.size(); ++i)
    {
        if (json_field(lines[i], "kind") == "robotweax-srt-telemetry-health")
        {
            /* After its sample: sampleIndex names a sample already written. */
            if (samples == 0 || std::stoull(json_field(lines[i], "sampleIndex")) >= samples ||
                json_field(lines[i], "streamId") != "cam-9")
            {
                return false;
            }
            ++health;
            continue;
        }
        const auto observed = std::stoull(json_field(lines[i], "observed_at_unix_nano"));
        if (json_field(lines[i], "kind") != "robotweax-srt-telemetry-sample" ||
            json_field(lines[i], "streamId") != "cam-9" || observed < previous ||
            json_field(lines[i], "member") != "null")
        {
            return false;
        }
        previous = observed;
        ++samples;
    }
    return json_field(lines[0], "recordCount") == std::to_string(samples) &&
           json_field(lines[0], "healthCount") == std::to_string(health);
}

void test_recordings()
{
    const auto directory = std::filesystem::temp_directory_path() /
                           ("srt-telemetry-embed-recordings-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    auto missing = base_config();
    check(robotweax_srt_telemetry_embed_config_parse(&missing, "off,jsonl=/nonexistent/srt-telemetry") ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "spec with a recording directory parses");
    robotweax_srt_telemetry_embed *refused = nullptr;
    check(robotweax_srt_telemetry_embed_start(&missing, &refused) == ROBOTWEAX_SRT_TELEMETRY_IO_FAILURE &&
              refused == nullptr,
          "a missing recording directory fails start");

    auto config = base_config();
    const auto spec = "off,name=cam-9,jsonl=" + directory.string() + ",jsonl_s=30,linger_s=30,provider_revision=test-rev";
    check(robotweax_srt_telemetry_embed_config_parse(&config, spec.c_str()) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "recording spec");
    robotweax_srt_telemetry_embed *embed = nullptr;
    check(robotweax_srt_telemetry_embed_start(&config, &embed) == ROBOTWEAX_SRT_TELEMETRY_OK && embed != nullptr,
          "start with recordings and no HTTP server");
    if (embed == nullptr)
    {
        return;
    }
    Stream stream;
    check(stream.open(nullptr), "recording stream opened");
    const robotweax_srt_telemetry_label_v1 flow[] = {{"flow", "studio-link"}};
    check(robotweax_srt_telemetry_embed_attach(embed, stream.sender, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER, flow,
                                               1) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_embed_attach(embed, stream.receiver, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER,
                                                   flow, 1) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "attach both ends");
    stream.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    check(recordings_in(directory).empty(), "nothing written while the sockets are attached");

    /* Detach writes at once: a host may exit right after it. */
    check(robotweax_srt_telemetry_embed_detach(embed, stream.sender) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_embed_detach(embed, stream.receiver) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "detach both ends");
    std::uint64_t written = 0;
    std::uint64_t failed = 0;
    check(wait_until(
              [&] {
                  robotweax_srt_telemetry_embed_recordings(embed, &written, &failed);
                  return written == 2;
              },
              5'000) &&
              failed == 0,
          "both recordings written soon after detach, by the writer thread");
    const auto files = recordings_in(directory);
    check(files.size() == 2, "one file per source, no temporary files left");
    std::map<std::string, std::size_t> detached_samples;
    for (const auto &file : files)
    {
        const auto name = file.filename().string();
        const auto role = name.find("-sender-") != std::string::npos ? "sender" : "receiver";
        check(name.rfind("srt-telemetry-20", 0) == 0 && name.find("Z-cam-9-") != std::string::npos &&
                  file.extension() == ".jsonl",
              "file name: " + name);
        const auto lines = read_lines(file);
        std::size_t samples = 0;
        check(complete_recording(lines, role, samples), std::string("complete recording: ") + role);
        check(samples >= 10 && samples <= 40, std::string("about 1.5 s of 100 ms samples: ") + role + " " +
                                                  std::to_string(samples));
        check(!lines.empty() && json_field(lines[0], "providerRevision") == "test-rev" &&
                  json_field(lines[0], "providerVersion") == ROBOTWEAX_SRT_TELEMETRY_HOST_PROVIDER_VERSION &&
                  json_field(lines[0], "buildProfile") == "embed" && json_field(lines[0], "capacity") == "300" &&
                  json_field(lines[0], "overwrittenCount") == "0",
              std::string("header metadata: ") + role);
        const auto counter = std::string(role) == "sender" ? "unique_sent_packet_total" : "unique_received_packet_total";
        /* The last sample: health lines (1alpha2) may follow it. */
        std::string last_sample;
        for (const auto &line : lines)
        {
            if (json_field(line, "kind") == "robotweax-srt-telemetry-sample")
                last_sample = line;
        }
        const auto total = json_field(last_sample, counter);
        check(!total.empty() && total != "null" && std::stoull(total) > 100,
              std::string("traffic recorded: ") + counter);
        /* The detach write already holds the source's loss, its last line. */
        check(json_field(lines.back(), "kind") == "robotweax-srt-telemetry-health" &&
                  json_field(lines.back(), "reason") == "connection_lost",
              std::string("the detach write ends with the loss: ") + role);
        detached_samples[role] = samples;
    }
    stream.stop();

    /* Stop ends the lingering sources: their files already hold every
     * sample, so they are left as written. */
    robotweax_srt_telemetry_embed_stop(embed);
    const auto final_files = recordings_in(directory);
    check(final_files == files, "stop leaves the same files, nothing else");
    for (const auto &file : final_files)
    {
        std::size_t samples = 0;
        const auto role = file.filename().string().find("-sender-") != std::string::npos ? "sender" : "receiver";
        check(complete_recording(read_lines(file), role, samples) && samples >= detached_samples[role],
              std::string("final recording complete: ") + role);
    }

    /* A ring shorter than the stream overwrites the oldest samples and says
     * so in the header. */
    auto small = base_config();
    const auto small_spec = "off,name=cam-9,jsonl=" + directory.string() + ",jsonl_s=1,linger_s=0";
    check(robotweax_srt_telemetry_embed_config_parse(&small, small_spec.c_str()) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "short ring spec");
    robotweax_srt_telemetry_embed *ring = nullptr;
    check(robotweax_srt_telemetry_embed_start(&small, &ring) == ROBOTWEAX_SRT_TELEMETRY_OK, "start short ring");
    Stream again;
    check(again.open(nullptr), "second stream opened");
    check(robotweax_srt_telemetry_embed_attach(ring, again.sender, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER, flow,
                                               1) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "attach sender to the short ring");
    again.start();
    /* 3 s of samples into a 1 s ring: overwrites for certain, also on a
     * slow machine. */
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    check(robotweax_srt_telemetry_embed_detach(ring, again.sender) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "detach with linger 0 removes and writes");
    again.stop();
    robotweax_srt_telemetry_embed_stop(ring);
    const auto all = recordings_in(directory);
    check(all.size() == 3, "a third recording");
    for (const auto &file : all)
    {
        if (std::find(files.begin(), files.end(), file) != files.end())
        {
            continue;
        }
        const auto lines = read_lines(file);
        std::size_t samples = 0;
        check(complete_recording(lines, "sender", samples) && samples == 10 &&
                  json_field(lines[0], "capacity") == "10" && std::stoull(json_field(lines[0], "overwrittenCount")) > 0,
              "a full ring keeps the newest 10 samples and counts the overwritten");
    }

    /* The default is 1alpha2; jsonl_schema=1alpha1 keeps the old schema for
     * a reader that accepts only that: no group fields, no health lines. */
    auto old = base_config();
    check(old.recording_schema == 0, "the default config leaves the schema to the library (1alpha2)");
    const auto old_spec = "off,name=cam-9,jsonl=" + directory.string() + ",linger_s=0,jsonl_schema=1alpha1";
    check(robotweax_srt_telemetry_embed_config_parse(&old, old_spec.c_str()) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              old.recording_schema == ROBOTWEAX_SRT_TELEMETRY_EMBED_RECORDING_1ALPHA1,
          "jsonl_schema=1alpha1 parses");
    robotweax_srt_telemetry_embed *legacy = nullptr;
    check(robotweax_srt_telemetry_embed_start(&old, &legacy) == ROBOTWEAX_SRT_TELEMETRY_OK, "start with 1alpha1");
    Stream third;
    check(third.open(nullptr) && robotweax_srt_telemetry_embed_attach(legacy, third.sender,
                                                                      ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER,
                                                                      flow, 1) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "attach to the 1alpha1 embed");
    third.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    check(robotweax_srt_telemetry_embed_detach(legacy, third.sender) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "detach from the 1alpha1 embed");
    third.stop();
    robotweax_srt_telemetry_embed_stop(legacy);
    bool legacy_found = false;
    for (const auto &file : recordings_in(directory))
    {
        if (std::find(all.begin(), all.end(), file) != all.end())
        {
            continue;
        }
        legacy_found = true;
        const auto lines = read_lines(file);
        bool plain = !lines.empty() && json_field(lines[0], "schemaVersion") == "1alpha1" &&
                     lines[0].find("\"groupRow\"") == std::string::npos && lines.size() > 1;
        for (std::size_t i = 1; i < lines.size(); ++i)
        {
            plain = plain && json_field(lines[i], "kind") == "robotweax-srt-telemetry-sample" &&
                    json_field(lines[i], "schemaVersion") == "1alpha1" &&
                    lines[i].find("\"member\"") == std::string::npos;
        }
        check(plain && json_field(lines[0], "recordCount") == std::to_string(lines.size() - 1),
              "jsonl_schema=1alpha1 writes 1alpha1: samples only");
    }
    check(legacy_found, "the 1alpha1 recording was written");
    std::filesystem::remove_all(directory);
}

std::uint64_t recordings_written(robotweax_srt_telemetry_embed *embed, std::uint64_t *failed = nullptr)
{
    std::uint64_t written = 0;
    robotweax_srt_telemetry_embed_recordings(embed, &written, failed);
    return written;
}

/* Linger expiry inside the embed thread, a reconnection continuing one
 * recording, no file for a socket that never sampled, hostile stream labels,
 * a failing directory and a zeroed config. */
void test_recording_lifecycle()
{
    const auto directory = std::filesystem::temp_directory_path() /
                           ("srt-telemetry-embed-lifecycle-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    /* A zeroed config (a host that builds it by hand) with recordings off. */
    robotweax_srt_telemetry_embed_config_v1 zeroed;
    std::memset(&zeroed, 0, sizeof(zeroed));
    zeroed.struct_size = sizeof(zeroed);
    zeroed.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    robotweax_srt_telemetry_host_embed_functions(&zeroed);
    zeroed.flags = ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP;
    std::snprintf(zeroed.otlp_endpoint, sizeof(zeroed.otlp_endpoint), "http://127.0.0.1:9");
    zeroed.otlp_interval_ms = 10'000; /* the exporter's own minimum is 100 ms */
    robotweax_srt_telemetry_embed *plain = nullptr;
    check(robotweax_srt_telemetry_embed_start(&zeroed, &plain) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "a zeroed config without recordings starts (recording_seconds 0)");
    robotweax_srt_telemetry_embed_stop(plain);
    std::snprintf(zeroed.recording_directory, sizeof(zeroed.recording_directory), "%s", directory.c_str());
    check(robotweax_srt_telemetry_embed_start(&zeroed, &plain) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "recording_seconds 0 with recordings means the default");
    robotweax_srt_telemetry_embed_stop(plain);

    auto config = base_config();
    const auto spec = "off,name=cam-9,jsonl=" + directory.string() + ",linger_s=2";
    check(robotweax_srt_telemetry_embed_config_parse(&config, spec.c_str()) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "lifecycle spec");
    robotweax_srt_telemetry_embed *embed = nullptr;
    check(robotweax_srt_telemetry_embed_start(&config, &embed) == ROBOTWEAX_SRT_TELEMETRY_OK, "lifecycle start");
    if (embed == nullptr)
    {
        return;
    }
    const robotweax_srt_telemetry_label_v1 flow[] = {{"flow", "studio-link"}};

    /* A reconnection during the linger continues the same recording. */
    Stream first;
    check(first.open(nullptr) && robotweax_srt_telemetry_embed_attach(embed, first.sender,
                                                                      ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER,
                                                                      flow, 1) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "first connection attached");
    first.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    check(robotweax_srt_telemetry_embed_detach(embed, first.sender) == ROBOTWEAX_SRT_TELEMETRY_OK, "first detached");
    first.stop();
    Stream second;
    check(second.open(nullptr) && robotweax_srt_telemetry_embed_attach(embed, second.sender,
                                                                       ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER,
                                                                       flow, 1) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "reconnection attached during the linger");
    second.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    check(robotweax_srt_telemetry_embed_detach(embed, second.sender) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "reconnection detached");
    second.stop();
    /* Two detach writes; the removal at the end of the linger (2 s) finds
     * no new samples and leaves the file alone. */
    check(wait_until([&] { return recordings_written(embed) >= 2; }, 8'000), "two detach writes");
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    check(recordings_written(embed) == 2, "the removal does not rewrite an unchanged recording");
    auto files = recordings_in(directory);
    check(files.size() == 1, "a continued source is one recording");
    std::size_t samples = 0;
    check(files.size() == 1 && complete_recording(read_lines(files[0]), "sender", samples) && samples >= 15,
          "the recording spans both connections: " + std::to_string(samples));

    /* A socket that never sampled leaves no file. */
    const auto idle = srt_create_socket();
    check(robotweax_srt_telemetry_embed_attach(embed, idle, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER, flow, 1) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_embed_detach(embed, idle) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "an unconnected socket attached and detached");
    srt_close(idle);

    /* A hostile stream label stays a plain file name in the directory. */
    Stream hostile;
    const robotweax_srt_telemetry_label_v1 path[] = {{"stream", "../../etc/x\"y\\z"}};
    check(hostile.open(nullptr) && robotweax_srt_telemetry_embed_attach(embed, hostile.receiver,
                                                                        ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER,
                                                                        path, 1) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "attach with a path-like stream label");
    hostile.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    check(robotweax_srt_telemetry_embed_detach(embed, hostile.receiver) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "hostile detached");
    hostile.stop();
    check(wait_until([&] { return recordings_written(embed) >= 3; }, 5'000), "hostile recording written");
    std::uint64_t failed = 0;
    static_cast<void>(recordings_written(embed, &failed));
    check(failed == 0, "no failed writes so far");
    files = recordings_in(directory);
    check(files.size() == 2 && std::all_of(files.begin(), files.end(),
                                           [&](const auto &file) { return file.parent_path() == directory; }),
          "no file for the idle socket; the hostile one stays inside the directory");

    robotweax_srt_telemetry_embed_stop(embed);
    std::filesystem::remove_all(directory);
}

/* Recording events as the writer reports them. */
struct RecordingEvents
{
    std::mutex mutex;
    std::vector<robotweax_srt_telemetry_embed_recording_event_v1> events;
    std::vector<std::string> paths;

    static void callback(void *user, const robotweax_srt_telemetry_embed_recording_event_v1 *event)
    {
        auto &self = *static_cast<RecordingEvents *>(user);
        std::lock_guard lock(self.mutex);
        self.events.push_back(*event);
        self.paths.emplace_back(event->path != nullptr ? event->path : "");
        self.events.back().path = nullptr;
    }
    std::size_t size()
    {
        std::lock_guard lock(mutex);
        return events.size();
    }
    robotweax_srt_telemetry_embed_recording_event_v1 at(const std::size_t i)
    {
        std::lock_guard lock(mutex);
        return events.at(i);
    }
    std::string path(const std::size_t i)
    {
        std::lock_guard lock(mutex);
        return paths.at(i);
    }
};

/* A live source written on request, the writer's events, and the final
 * write: a pruned file is not brought back by an unchanged removal. */
void test_live_recording()
{
    const auto directory = std::filesystem::temp_directory_path() /
                           ("srt-telemetry-embed-live-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    auto plain = base_config();
    plain.flags = ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP;
    robotweax_srt_telemetry_embed *without = nullptr;
    check(robotweax_srt_telemetry_embed_start(&plain, &without) == ROBOTWEAX_SRT_TELEMETRY_OK, "start without recordings");
    check(robotweax_srt_telemetry_embed_write_recording(without, 7) == ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE,
          "no live write without recordings");
    robotweax_srt_telemetry_embed_stop(without);
    check(robotweax_srt_telemetry_embed_write_recording(nullptr, 7) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT &&
              robotweax_srt_telemetry_embed_set_recording_callback(nullptr, nullptr, nullptr) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "NULL embed refused");

    auto config = base_config();
    const auto spec = "off,name=cam-9,jsonl=" + directory.string() + ",linger_s=2";
    check(robotweax_srt_telemetry_embed_config_parse(&config, spec.c_str()) == ROBOTWEAX_SRT_TELEMETRY_OK, "live spec");
    robotweax_srt_telemetry_embed *embed = nullptr;
    check(robotweax_srt_telemetry_embed_start(&config, &embed) == ROBOTWEAX_SRT_TELEMETRY_OK, "live start");
    if (embed == nullptr)
    {
        return;
    }
    const robotweax_srt_telemetry_label_v1 flow[] = {{"flow", "studio-link"}};
    RecordingEvents events;
    check(robotweax_srt_telemetry_embed_set_recording_callback(embed, RecordingEvents::callback, &events) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "recording callback set");
    Stream stream;
    check(stream.open(nullptr) && robotweax_srt_telemetry_embed_attach(embed, stream.sender,
                                                                       ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER,
                                                                       flow, 1) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "live attach");
    check(robotweax_srt_telemetry_embed_write_recording(embed, stream.receiver) == ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND,
          "a socket that is not attached");
    stream.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    /* Live write: one file, the source goes on. */
    check(robotweax_srt_telemetry_embed_write_recording(embed, stream.sender) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "live write requested");
    check(wait_until([&] { return events.size() == 1; }, 5'000), "live write reported");
    const auto first = events.at(0);
    check(first.flags == ROBOTWEAX_SRT_TELEMETRY_EMBED_RECORDING_WRITTEN && first.record_count >= 5 &&
              first.source_id != 0 && first.struct_size == sizeof(first),
          "live write: written, not final, " + std::to_string(first.record_count) + " samples");
    const auto file = std::filesystem::path(events.path(0));
    auto files = recordings_in(directory);
    check(files.size() == 1 && files[0] == file, "the event names the recording's one file");
    std::size_t samples = 0;
    check(complete_recording(read_lines(file), "sender", samples) && samples == first.record_count,
          "the live file is a complete recording of the samples so far");

    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    check(robotweax_srt_telemetry_embed_write_recording(embed, stream.sender) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              wait_until([&] { return events.size() == 2; }, 5'000),
          "second live write reported");
    const auto second = events.at(1);
    check(second.flags == ROBOTWEAX_SRT_TELEMETRY_EMBED_RECORDING_WRITTEN &&
              second.record_count > first.record_count && events.path(1) == file.string() &&
              recordings_in(directory).size() == 1,
          "a later live write replaces the same file with a longer version");

    /* Detach writes the rest; the removal after the linger is final and
     * leaves a pruned file pruned. */
    check(robotweax_srt_telemetry_embed_detach(embed, stream.sender) == ROBOTWEAX_SRT_TELEMETRY_OK, "live detach");
    stream.stop();
    check(wait_until([&] { return events.size() == 3; }, 5'000) &&
              events.at(2).flags == ROBOTWEAX_SRT_TELEMETRY_EMBED_RECORDING_WRITTEN &&
              events.at(2).record_count >= second.record_count,
          "detach writes, not final (the source lingers)");
    std::filesystem::remove(file);
    check(wait_until([&] { return events.size() == 4; }, 8'000), "removal after the linger reported");
    const auto last = events.at(3);
    check(last.flags == (ROBOTWEAX_SRT_TELEMETRY_EMBED_RECORDING_UNCHANGED | ROBOTWEAX_SRT_TELEMETRY_EMBED_RECORDING_FINAL) &&
              last.source_id == first.source_id && last.record_count == events.at(2).record_count,
          "the final event: unchanged, final, same source: flags " + std::to_string(last.flags) + ", " +
              std::to_string(last.record_count) + " vs " + std::to_string(events.at(2).record_count) + " samples");
    check(!std::filesystem::exists(file), "the pruned file does not come back");

    /* A source that never sampled: one final event, no file. */
    const auto idle = srt_create_socket();
    check(robotweax_srt_telemetry_embed_attach(embed, idle, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER, nullptr, 0) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_embed_detach(embed, idle) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "idle socket attached and detached");
    srt_close(idle);
    check(wait_until([&] { return events.size() == 6; }, 8'000) &&
              events.at(4).flags == ROBOTWEAX_SRT_TELEMETRY_EMBED_RECORDING_EMPTY &&
              events.at(5).flags ==
                  (ROBOTWEAX_SRT_TELEMETRY_EMBED_RECORDING_EMPTY | ROBOTWEAX_SRT_TELEMETRY_EMBED_RECORDING_FINAL),
          "a never-sampled source: empty at detach, empty and final at removal");

    /* A source still attached at stop gets its final write then. */
    Stream open;
    check(open.open(nullptr) && robotweax_srt_telemetry_embed_attach(embed, open.receiver,
                                                                     ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER,
                                                                     nullptr, 0) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "attached until stop");
    open.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    robotweax_srt_telemetry_embed_stop(embed);
    open.stop();
    check(events.size() == 7 &&
              events.at(6).flags ==
                  (ROBOTWEAX_SRT_TELEMETRY_EMBED_RECORDING_WRITTEN | ROBOTWEAX_SRT_TELEMETRY_EMBED_RECORDING_FINAL),
          "stop: written and final, reported before stop returns");
    std::filesystem::remove_all(directory);
}

/* The recording callback may call into the embed, also while stop runs;
 * clearing the callback waits for a running call. */
struct ReentrantCallback
{
    robotweax_srt_telemetry_embed *embed = nullptr;
    std::vector<std::int32_t> sockets;
    std::atomic<int> finals{0};
    std::atomic<int> refused{0};
    std::atomic<bool> slow{false};
    std::atomic<bool> running{false};

    static void callback(void *user, const robotweax_srt_telemetry_embed_recording_event_v1 *event)
    {
        auto &self = *static_cast<ReentrantCallback *>(user);
        self.running = true;
        if (self.slow)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        if ((event->flags & ROBOTWEAX_SRT_TELEMETRY_EMBED_RECORDING_FINAL) != 0)
        {
            ++self.finals;
            for (const auto socket : self.sockets)
            {
                const auto write = robotweax_srt_telemetry_embed_write_recording(self.embed, socket);
                const auto detach = robotweax_srt_telemetry_embed_detach(self.embed, socket);
                const auto attach = robotweax_srt_telemetry_embed_attach(
                    self.embed, socket, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER, nullptr, 0);
                if (write == ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE && detach == ROBOTWEAX_SRT_TELEMETRY_OK &&
                    attach == ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE)
                {
                    ++self.refused;
                }
            }
        }
        self.running = false;
    }
};

void test_recording_callback_reentry()
{
    const auto directory = std::filesystem::temp_directory_path() /
                           ("srt-telemetry-embed-reentry-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    auto config = base_config();
    const auto spec = "off,name=cam-9,jsonl=" + directory.string() + ",linger_s=30";
    check(robotweax_srt_telemetry_embed_config_parse(&config, spec.c_str()) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "reentry spec");
    robotweax_srt_telemetry_embed *embed = nullptr;
    check(robotweax_srt_telemetry_embed_start(&config, &embed) == ROBOTWEAX_SRT_TELEMETRY_OK, "reentry start");
    if (embed == nullptr)
    {
        return;
    }
    ReentrantCallback state;
    state.embed = embed;
    Stream stream;
    check(stream.open(nullptr), "reentry stream");
    for (const auto socket : {stream.sender, stream.receiver})
    {
        check(robotweax_srt_telemetry_embed_attach(embed, socket,
                                                   socket == stream.sender ? ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER
                                                                           : ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER,
                                                   nullptr, 0) == ROBOTWEAX_SRT_TELEMETRY_OK,
              "reentry attach");
        state.sockets.push_back(socket);
    }
    stream.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    /* Clearing waits for a running call: after it returns, the callback is
     * neither running nor called again. */
    state.slow = true;
    check(robotweax_srt_telemetry_embed_set_recording_callback(embed, ReentrantCallback::callback, &state) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_embed_write_recording(embed, stream.sender) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "slow callback and a live write");
    check(wait_until([&] { return state.running.load(); }, 5'000), "the slow callback runs");
    check(robotweax_srt_telemetry_embed_set_recording_callback(embed, nullptr, nullptr) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              !state.running,
          "clearing the callback waits until it has returned");
    state.slow = false;

    /* During stop the final events call write, detach and attach for every
     * socket: refused or ignored, never a crash. */
    check(robotweax_srt_telemetry_embed_set_recording_callback(embed, ReentrantCallback::callback, &state) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "reentrant callback set");
    robotweax_srt_telemetry_embed_stop(embed);
    stream.stop();
    check(state.finals == 2 && state.refused == 4,
          "stop: two final events, embed calls from them refused (" + std::to_string(state.finals) + ", " +
              std::to_string(state.refused) + ")");
    std::filesystem::remove_all(directory);
}

/* Thread names of this process (Linux), from /proc. */
std::vector<std::string> thread_names()
{
    std::vector<std::string> names;
#if defined(__linux__)
    for (const auto &task : std::filesystem::directory_iterator("/proc/self/task"))
    {
        std::ifstream comm(task.path() / "comm");
        std::string name;
        std::getline(comm, name);
        names.push_back(name);
    }
#endif
    return names;
}

void test_thread_names()
{
#if defined(__linux__)
    /* A host thread with its own name starts the embed: the embed's threads
     * do not inherit it. */
    pthread_setname_np(pthread_self(), "rwx-main");
    const auto directory = std::filesystem::temp_directory_path() /
                           ("srt-telemetry-embed-names-" + std::to_string(::getpid()));
    std::filesystem::create_directories(directory);
    auto config = base_config();
    const auto spec = "127.0.0.1:0,name=cam-9,jsonl=" + directory.string();
    check(robotweax_srt_telemetry_embed_config_parse(&config, spec.c_str()) == ROBOTWEAX_SRT_TELEMETRY_OK, "names spec");
    robotweax_srt_telemetry_embed *embed = nullptr;
    check(robotweax_srt_telemetry_embed_start(&config, &embed) == ROBOTWEAX_SRT_TELEMETRY_OK, "names start");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto names = thread_names();
    const auto has = [&](const char *name) { return std::find(names.begin(), names.end(), name) != names.end(); };
    check(has("srt-tel-embed") && has("srt-tel-writer") && has("srt-tel-http"),
          "embed, writer and HTTP threads carry their own names");
    check(std::count(names.begin(), names.end(), "rwx-main") == 1, "only the host thread is named rwx-main");
    robotweax_srt_telemetry_embed_stop(embed);
    std::filesystem::remove_all(directory);
#endif
}

void test_recording_failure_counter()
{
    const auto directory = std::filesystem::temp_directory_path() /
                           ("srt-telemetry-embed-failure-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    auto config = base_config();
    const auto spec = "off,name=cam-9,jsonl=" + directory.string() + ",linger_s=0";
    check(robotweax_srt_telemetry_embed_config_parse(&config, spec.c_str()) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "failure spec");
    robotweax_srt_telemetry_embed *embed = nullptr;
    check(robotweax_srt_telemetry_embed_start(&config, &embed) == ROBOTWEAX_SRT_TELEMETRY_OK, "failure start");
    Stream stream;
    check(stream.open(nullptr) && robotweax_srt_telemetry_embed_attach(embed, stream.sender,
                                                                       ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER,
                                                                       nullptr, 0) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "failure attach");
    stream.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::filesystem::remove_all(directory);
    check(robotweax_srt_telemetry_embed_detach(embed, stream.sender) == ROBOTWEAX_SRT_TELEMETRY_OK, "failure detach");
    stream.stop();
    std::uint64_t failed = 0;
    check(wait_until(
              [&] {
                  static_cast<void>(recordings_written(embed, &failed));
                  return failed == 1;
              },
              5'000) &&
              recordings_written(embed) == 0,
          "a write into a removed directory is counted as failed");
    robotweax_srt_telemetry_embed_stop(embed);
}

#if defined(ROBOTWEAX_SRT_PEER_IDENTITY_VERSION) && defined(ROBOTWEAX_SRT_PEER_HANDSHAKE_VERSION)
/* The "peer" object of the source with this stream label and role. */
std::string peer_json(robotweax_srt_telemetry_embed *embed, const std::string &stream, const std::string &role)
{
    robotweax_srt_telemetry_render_options_v1 options{};
    robotweax_srt_telemetry_render_options_default(ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON, &options);
    std::vector<char> buffer(1 << 20);
    std::size_t length = 0;
    if (robotweax_srt_telemetry_sampler_render(robotweax_srt_telemetry_embed_sampler(embed), &options, buffer.data(),
                                               buffer.size(), &length) != ROBOTWEAX_SRT_TELEMETRY_OK)
        return {};
    const std::string json(buffer.data(), length);
    for (std::size_t at = json.find("{\"id\":"); at != std::string::npos; at = json.find("{\"id\":", at + 1))
    {
        const auto next = json.find("{\"id\":", at + 1);
        const auto source = json.substr(at, next == std::string::npos ? std::string::npos : next - at);
        if (source.find("\"role\":\"" + role + "\"") == std::string::npos ||
            source.find("\"stream\":\"" + stream + "\"") == std::string::npos)
            continue;
        const auto start = source.find("\"peer\":");
        const auto end = source.find("}}", start);
        return start == std::string::npos || end == std::string::npos ? std::string{}
                                                                       : source.substr(start, end + 2 - start);
    }
    return {};
}

int note_handshake(void *embed, SRTSOCKET socket, int, const sockaddr *, const char *)
{
    /* The host's own admission decision stays its own: noting never rejects. */
    static_cast<void>(robotweax_srt_telemetry_host_embed_note_peer_handshake(
        static_cast<robotweax_srt_telemetry_embed *>(embed), socket));
    return 0;
}

/* Robotweax SRT 0.3.0 peer metadata, live on loopback: each side's identity
 * as the other advertises it (the caller its name only, the listener name
 * and release), the caller's handshake on the accepted socket (its own
 * configured latencies), and unknown identities without advertisement. */
void test_peer_metadata()
{
    auto config = base_config();
    config.flags = ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP;
    config.linger_seconds = 0;
    robotweax_srt_telemetry_embed *embed = nullptr;
    check(robotweax_srt_telemetry_embed_start(&config, &embed) == ROBOTWEAX_SRT_TELEMETRY_OK, "peer: start");
    if (embed == nullptr)
        return;
    check(robotweax_srt_telemetry_host_embed_peer_identity(embed) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "peer: identity enabled");

    const auto open = [&](const char *stream, const bool advertise, SRTSOCKET &listener, SRTSOCKET &caller,
                          SRTSOCKET &accepted)
    {
        listener = srt_create_socket();
        caller = srt_create_socket();
        if (advertise)
        {
            std::int32_t full = ROBOTWEAX_SRT_IDENTITY_NAME_AND_RELEASE;
            std::int32_t name = ROBOTWEAX_SRT_IDENTITY_NAME;
            check(srt_setsockflag(listener, SRTO_ROBOTWEAX_IDENTITY_ADVERTISEMENT, &full, sizeof(full)) == 0 &&
                      srt_setsockflag(caller, SRTO_ROBOTWEAX_IDENTITY_ADVERTISEMENT, &name, sizeof(name)) == 0,
                  "peer: advertisement set");
        }
        int receive = 250;
        int peer = 180;
        srt_setsockflag(caller, SRTO_RCVLATENCY, &receive, sizeof(receive));
        srt_setsockflag(caller, SRTO_PEERLATENCY, &peer, sizeof(peer));
        srt_listen_callback(listener, note_handshake, embed);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int size = sizeof(address);
        const bool ok = srt_bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
                        srt_listen(listener, 1) == 0 &&
                        srt_getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size) == 0 &&
                        srt_connect(caller, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != SRT_ERROR;
        accepted = ok ? srt_accept(listener, nullptr, nullptr) : SRT_INVALID_SOCK;
        check(accepted != SRT_INVALID_SOCK, std::string("peer: connected ") + stream);
        const robotweax_srt_telemetry_label_v1 labels[] = {{"stream", stream}};
        check(robotweax_srt_telemetry_embed_attach(embed, caller, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER, labels,
                                                   1) == ROBOTWEAX_SRT_TELEMETRY_OK &&
                  robotweax_srt_telemetry_embed_attach(embed, accepted, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER,
                                                       labels, 1) == ROBOTWEAX_SRT_TELEMETRY_OK,
              std::string("peer: attached ") + stream);
    };
    SRTSOCKET listener = SRT_INVALID_SOCK, caller = SRT_INVALID_SOCK, accepted = SRT_INVALID_SOCK;
    SRTSOCKET quiet_listener = SRT_INVALID_SOCK, quiet_caller = SRT_INVALID_SOCK, quiet_accepted = SRT_INVALID_SOCK;
    open("advertised", true, listener, caller, accepted);
    open("quiet", false, quiet_listener, quiet_caller, quiet_accepted);

    const std::string release = std::string("\"release\":\"") + ROBOTWEAX_SRT_VERSION_STRING + "\"";
    check(wait_until(
              [&] {
                  return peer_json(embed, "advertised", "sender").find(release) != std::string::npos &&
                         peer_json(embed, "advertised", "receiver").find("\"release\":null") != std::string::npos;
              },
              5'000),
          "peer: both identities read");
    const auto sender = peer_json(embed, "advertised", "sender");
    const auto receiver = peer_json(embed, "advertised", "receiver");
    check(sender.find("\"identity\":{\"availability\":\"available\",\"name\":\"Robotweax SRT\"," + release + "}") !=
                  std::string::npos &&
              sender.find("\"handshake\":{\"availability\":\"unavailable\"}") != std::string::npos,
          "peer: the caller sees the listener's name and release, no handshake: " + sender);
    check(receiver.find("\"identity\":{\"availability\":\"available\",\"name\":\"Robotweax SRT\","
                        "\"release\":null}") != std::string::npos,
          "peer: the listener sees the caller's name, release withheld: " + receiver);
    check(receiver.find("\"handshake\":{\"availability\":\"available\",\"srt_version\":\"1.5.7\"") !=
                  std::string::npos &&
              receiver.find("\"tsbpd_send\":true") != std::string::npos &&
              receiver.find("\"receiver_delay_seconds\":0.25,\"sender_delay_seconds\":0.18") != std::string::npos,
          "peer: the accepted socket shows the caller's handshake and its own latencies: " + receiver);

    check(wait_until(
              [&] {
                  return peer_json(embed, "quiet", "sender").find("\"identity\":{\"availability\":\"unknown\"}") !=
                             std::string::npos &&
                         peer_json(embed, "quiet", "receiver").find("\"identity\":{\"availability\":\"unknown\"}") !=
                             std::string::npos;
              },
              5'000),
          "peer: without advertisement both sides read unknown: " + peer_json(embed, "quiet", "receiver"));
    check(peer_json(embed, "quiet", "receiver").find("\"handshake\":{\"availability\":\"available\"") !=
              std::string::npos,
          "peer: the handshake does not need identity advertisement");

    check(robotweax_srt_telemetry_embed_set_peer_identity_v1(embed, nullptr) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              peer_json(embed, "advertised", "sender").find("\"identity\":{\"availability\":\"unavailable\"}") !=
                  std::string::npos,
          "peer: disabling clears identities");
    check(peer_json(embed, "advertised", "receiver").find("\"handshake\":{\"availability\":\"available\"") !=
              std::string::npos,
          "peer: disabling identity keeps the handshake");

    check(robotweax_srt_telemetry_embed_note_peer_handshake_v1(embed, 7, nullptr, 24) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "peer: a missing handshake is refused");
    /* The listeners' callback holds the embed: close them before stop. */
    for (const auto socket : {listener, quiet_listener})
        if (socket != SRT_INVALID_SOCK)
            srt_close(socket);
    robotweax_srt_telemetry_embed_stop(embed);
    for (const auto socket : {caller, accepted, quiet_caller, quiet_accepted})
        if (socket != SRT_INVALID_SOCK)
            srt_close(socket);
}
#endif

} // namespace

int main()
{
    srt_startup();
    srt_setloglevel(2);
    test_parse();
    test_lifecycle();
    test_recordings();
    test_recording_lifecycle();
    test_recording_failure_counter();
    test_live_recording();
    test_recording_callback_reentry();
    test_thread_names();
#if defined(ROBOTWEAX_SRT_PEER_IDENTITY_VERSION) && defined(ROBOTWEAX_SRT_PEER_HANDSHAKE_VERSION)
    test_peer_metadata();
#endif
    srt_cleanup();
    if (failures != 0)
    {
        std::cerr << failures << " embed checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all embed checks passed\n";
    return EXIT_SUCCESS;
}
