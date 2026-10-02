/* Dashboard demo: a small fleet of real SRT streams on loopback, each sent
 * through an impairing relay, with the sampler, health engine and embedded
 * HTTP server. Open the printed URL to see the built-in UI.
 *
 * Streams (one sender and one receiver source each):
 *   studio-a     clean path, 200 ms latency                    -> OK
 *   field-cam-2  2 % random loss, repaired                     -> OK / Watch
 *   remote-4g    RTT 140 ms against 200 ms latency             -> latency advice
 *   uplink-3     path capped at 3 Mbit/s for a 5.3 Mbit/s feed -> bitrate advice
 *   venue-b      cycles: clean, loss bursts, 6 s outage, clean
 *
 * Usage: dashboard_demo [--port N] [--bind ADDRESS] [--token T]
 *                       [--duration SECONDS] [--streams N]
 *                       [--frame-ancestors "https://console.example.com"] */

#include <robotweax/srt_telemetry_http.h>
#include <robotweax/srt_telemetry_public_srt.h>
#include <robotweax/srt_telemetry_sampler.h>

#if defined(ROBOTWEAX_SRT_TELEMETRY_RUNTIME_USE_FLAT_SRT_HEADER)
#include <srt.h>
#define ROBOTWEAX_SRT_TELEMETRY_USE_FLAT_SRT_HEADER
#else
#include <srt/srt.h>
#endif
#include <robotweax/srt_telemetry_public_srt_host.h>

#include "impairing_relay.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{

using robotweax_srt_telemetry_lab::Clock;
using robotweax_srt_telemetry_lab::Impairment;
using robotweax_srt_telemetry_lab::Relay;

constexpr auto provider =
#if defined(ROBOTWEAX_SRT_TELEMETRY_RUNTIME_PROVIDER_ROBOTWEAX)
    ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
constexpr const char *provider_label = "robotweax-srt";
constexpr std::uint64_t ext_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_ROBOTWEAX_EXT_CANDIDATE_FIELDS;
#else
    ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
constexpr const char *provider_label = "haivision-srt";
constexpr std::uint64_t ext_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_HAIVISION_V1_5_5_EXT_CANDIDATE_FIELDS;
#endif

constexpr int payload_bytes = 1316;
constexpr int packets_per_second = 500;

std::atomic<bool> stop_requested{false};

void on_signal(int)
{
    stop_requested = true;
}

/* One phase of a stream's impairment schedule. */
struct Phase
{
    Impairment impairment;
    double seconds = 0; /* 0: for good */
};

struct StreamSpec
{
    const char *name;
    const char *site;
    int latency_ms;
    std::vector<Phase> phases; /* repeated */
};

std::vector<StreamSpec> specs()
{
    std::vector<StreamSpec> list;
    Impairment clean;
    list.push_back({"studio-a", "berlin", 200, {{clean, 0}}});
    Impairment lossy;
    lossy.forward_loss = 0.02;
    list.push_back({"field-cam-2", "hamburg", 200, {{lossy, 0}}});
    Impairment far;
    far.delay_ms = 70;
    list.push_back({"remote-4g", "munich", 200, {{clean, 8}, {far, 0}}});
    Impairment capped;
    capped.forward_rate_bps = 3e6;
    capped.queue_limit_bytes = 100'000;
    list.push_back({"uplink-3", "cologne", 200, {{clean, 8}, {capped, 0}}});
    Impairment bursts;
    bursts.burst_every_ms = 2000;
    bursts.burst_packets = 30;
    Impairment outage;
    outage.blackhole = true;
    list.push_back({"venue-b", "leipzig", 300, {{clean, 40}, {bursts, 30}, {outage, 6}, {clean, 20}}});
    return list;
}

bool set_int(const SRTSOCKET socket, const SRT_SOCKOPT option, const int value)
{
    return srt_setsockflag(socket, option, &value, sizeof(value)) != SRT_ERROR;
}

struct Endpoint
{
    SRTSOCKET socket = SRT_INVALID_SOCK;
    robotweax_srt_telemetry_public_srt_session *session = nullptr;
    robotweax_srt_telemetry_public_srt_connection_monitor *monitor = nullptr;
    robotweax_srt_telemetry_health_context_v1 context{};
    std::uint64_t source_id = 0;
};

/* A caller (sender) and listener (receiver) joined through a relay. */
struct Stream
{
    StreamSpec spec;
    SRTSOCKET listener = SRT_INVALID_SOCK;
    Endpoint sender;
    Endpoint receiver;
    Relay relay;
    std::atomic<bool> running{true};
    std::thread send_thread;
    std::thread read_thread;
    std::size_t phase = 0;
    Clock::time_point phase_start;
};

bool attach(robotweax_srt_telemetry_sampler *sampler, Endpoint &endpoint, const StreamSpec &spec,
            const robotweax_srt_telemetry_endpoint_role role)
{
    robotweax_srt_telemetry_public_srt_config_v2 config{};
    config.struct_size = sizeof(config);
    config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config.socket = endpoint.socket;
    config.provider = provider;
    config.capabilities = ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1 | ROBOTWEAX_SRT_TELEMETRY_CAP_FEC_STATS_V1;
    config.qualified_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_V1_5_5_CANDIDATE_FIELDS;
    config.qualified_ext_fields = ext_fields;
    config.bistats = &srt_bistats;
    if (robotweax_srt_telemetry_public_srt_session_create_v2(&config, &endpoint.session) !=
        ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return false;
    }
    robotweax_srt_telemetry_public_srt_connection_config_v1 monitor{};
    monitor.struct_size = sizeof(monitor);
    monitor.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    monitor.socket = endpoint.socket;
    monitor.provider = provider;
    robotweax_srt_telemetry_host_connection_functions(&monitor);
    robotweax_srt_telemetry_health_context_init(&endpoint.context);
    if (robotweax_srt_telemetry_public_srt_connection_monitor_create(&monitor, &endpoint.monitor) !=
        ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return false;
    }
    const robotweax_srt_telemetry_label_v1 labels[] = {
        {"stream", spec.name},
        {"site", spec.site},
        {"provider", provider_label},
    };
    robotweax_srt_telemetry_source_config_v1 source{};
    robotweax_srt_telemetry_public_srt_source_config(endpoint.session, role, labels, 3, &source);
    return robotweax_srt_telemetry_sampler_add_source(sampler, &source, &endpoint.source_id) ==
           ROBOTWEAX_SRT_TELEMETRY_OK;
}

void poll_endpoint(robotweax_srt_telemetry_sampler *sampler, Endpoint &endpoint)
{
    robotweax_srt_telemetry_public_srt_connection_result_v1 result{};
    result.struct_size = sizeof(result);
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
    if (robotweax_srt_telemetry_public_srt_connection_monitor_poll(endpoint.monitor, now, now, &result) ==
        ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        static_cast<void>(robotweax_srt_telemetry_public_srt_health_context(&result, &endpoint.context));
    }
    static_cast<void>(robotweax_srt_telemetry_sampler_set_health_context(sampler, endpoint.source_id,
                                                                         &endpoint.context));
}

bool open_stream(robotweax_srt_telemetry_sampler *sampler, Stream &stream)
{
    stream.listener = srt_create_socket();
    stream.sender.socket = srt_create_socket();
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    const int latency = stream.spec.latency_ms;
    if (!set_int(stream.listener, SRTO_LATENCY, latency) || !set_int(stream.sender.socket, SRTO_LATENCY, latency) ||
        !set_int(stream.listener, SRTO_PEERIDLETIMEO, 10'000) ||
        !set_int(stream.sender.socket, SRTO_PEERIDLETIMEO, 10'000) ||
        !set_int(stream.sender.socket, SRTO_SNDTIMEO, 200) ||
        srt_bind(stream.listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SRT_ERROR ||
        srt_getsockname(stream.listener, reinterpret_cast<sockaddr *>(&address), reinterpret_cast<int *>(&size)) ==
            SRT_ERROR ||
        srt_listen(stream.listener, 1) == SRT_ERROR || !stream.relay.start(address))
    {
        return false;
    }
    stream.relay.set(Impairment{}); /* connect on a clean path */
    const auto &relay_address = stream.relay.address();
    if (srt_connect(stream.sender.socket, reinterpret_cast<const sockaddr *>(&relay_address),
                    sizeof(relay_address)) == SRT_ERROR)
    {
        return false;
    }
    stream.receiver.socket = srt_accept(stream.listener, nullptr, nullptr);
    if (stream.receiver.socket == SRT_INVALID_SOCK || !set_int(stream.receiver.socket, SRTO_RCVTIMEO, 200))
    {
        return false;
    }
    if (!attach(sampler, stream.sender, stream.spec, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER) ||
        !attach(sampler, stream.receiver, stream.spec, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER))
    {
        return false;
    }
    stream.phase = 0;
    stream.phase_start = Clock::now();
    stream.relay.set(stream.spec.phases.front().impairment);
    stream.send_thread = std::thread(
        [&stream]
        {
            std::array<char, payload_bytes> payload{};
            payload.fill('x');
            const auto start = Clock::now();
            const auto period = std::chrono::nanoseconds(1'000'000'000 / packets_per_second);
            for (std::int64_t i = 0; stream.running; ++i)
            {
                std::this_thread::sleep_until(start + period * i);
                if (srt_sendmsg(stream.sender.socket, payload.data(), payload_bytes, -1, 1) == SRT_ERROR &&
                    srt_getsockstate(stream.sender.socket) != SRTS_CONNECTED)
                {
                    return; /* broken: the dashboard shows the lost connection */
                }
            }
        });
    stream.read_thread = std::thread(
        [&stream]
        {
            std::array<char, 1500> buffer{};
            while (stream.running)
            {
                if (srt_recvmsg(stream.receiver.socket, buffer.data(), static_cast<int>(buffer.size())) <= 0 &&
                    srt_getsockstate(stream.receiver.socket) != SRTS_CONNECTED)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        });
    return true;
}

void advance(Stream &stream)
{
    const auto &phase = stream.spec.phases[stream.phase];
    if (phase.seconds <= 0 ||
        std::chrono::duration<double>(Clock::now() - stream.phase_start).count() < phase.seconds)
    {
        return;
    }
    stream.phase = (stream.phase + 1) % stream.spec.phases.size();
    stream.phase_start = Clock::now();
    stream.relay.set(stream.spec.phases[stream.phase].impairment);
}

void close_stream(robotweax_srt_telemetry_sampler *sampler, Stream &stream)
{
    stream.running = false;
    for (auto *endpoint : {&stream.sender, &stream.receiver})
    {
        if (endpoint->source_id != 0)
        {
            static_cast<void>(robotweax_srt_telemetry_sampler_remove_source(sampler, endpoint->source_id));
        }
    }
    for (const auto socket : {stream.sender.socket, stream.receiver.socket, stream.listener})
    {
        if (socket != SRT_INVALID_SOCK)
        {
            srt_close(socket);
        }
    }
    if (stream.send_thread.joinable())
    {
        stream.send_thread.join();
    }
    if (stream.read_thread.joinable())
    {
        stream.read_thread.join();
    }
    for (auto *endpoint : {&stream.sender, &stream.receiver})
    {
        robotweax_srt_telemetry_public_srt_connection_monitor_destroy(endpoint->monitor);
        robotweax_srt_telemetry_public_srt_session_destroy(endpoint->session);
    }
}

} // namespace

int main(const int argc, char **argv)
{
    int port = 9810;
    const char *bind = "127.0.0.1";
    const char *token = "";
    const char *frame_ancestors = "";
    double duration = 0;
    std::size_t stream_count = 5;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string option = argv[i];
        if (option == "--port")
        {
            port = std::atoi(argv[i + 1]);
        }
        else if (option == "--bind")
        {
            bind = argv[i + 1];
        }
        else if (option == "--token")
        {
            token = argv[i + 1];
        }
        else if (option == "--frame-ancestors")
        {
            frame_ancestors = argv[i + 1];
        }
        else if (option == "--duration")
        {
            duration = std::atof(argv[i + 1]);
        }
        else if (option == "--streams")
        {
            stream_count = static_cast<std::size_t>(std::atoi(argv[i + 1]));
        }
        else
        {
            std::fprintf(stderr, "unknown option %s\n", option.c_str());
            return 2;
        }
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    srt_startup();
    srt_setloglevel(3);

    robotweax_srt_telemetry_sampler_config_v1 config{};
    robotweax_srt_telemetry_sampler_config_default(&config);
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    if (robotweax_srt_telemetry_sampler_create(&config, &sampler) != ROBOTWEAX_SRT_TELEMETRY_OK ||
        robotweax_srt_telemetry_sampler_enable_health(sampler, 1) != ROBOTWEAX_SRT_TELEMETRY_OK ||
        robotweax_srt_telemetry_sampler_enable_history(sampler, 3600) != ROBOTWEAX_SRT_TELEMETRY_OK ||
        robotweax_srt_telemetry_sampler_enable_journal(sampler, 4096) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        std::fprintf(stderr, "sampler setup failed\n");
        return 1;
    }

    auto all = specs();
    std::vector<std::unique_ptr<Stream>> streams;
    for (std::size_t i = 0; i < std::min(stream_count, all.size()); ++i)
    {
        auto stream = std::make_unique<Stream>();
        stream->spec = all[i];
        streams.push_back(std::move(stream));
    }
    bool ok = true;
    for (auto &stream : streams)
    {
        if (!open_stream(sampler, *stream))
        {
            std::fprintf(stderr, "%s: setup failed: %s\n", stream->spec.name, srt_getlasterror_str());
            ok = false;
            break;
        }
    }
    robotweax_srt_telemetry_http_server *server = nullptr;
    if (ok)
    {
        ok = robotweax_srt_telemetry_sampler_start(sampler) == ROBOTWEAX_SRT_TELEMETRY_OK;
    }
    if (ok)
    {
        robotweax_srt_telemetry_http_server_config_v1 http{};
        robotweax_srt_telemetry_http_server_config_default(sampler, &http);
        std::snprintf(http.bind_address, sizeof(http.bind_address), "%s", bind);
        std::snprintf(http.bearer_token, sizeof(http.bearer_token), "%s", token);
        std::snprintf(http.frame_ancestors, sizeof(http.frame_ancestors), "%s", frame_ancestors);
        http.port = static_cast<std::uint16_t>(port);
        const auto error = robotweax_srt_telemetry_http_server_start(&http, &server);
        if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            std::fprintf(stderr, "HTTP server failed: %s\n", robotweax_srt_telemetry_error_string(error));
            ok = false;
        }
        else
        {
            const char *host = std::strcmp(bind, "0.0.0.0") == 0 ? "127.0.0.1"
                               : std::strchr(bind, ':') != nullptr ? "[::1]"
                                                                     : bind;
            std::printf("dashboard: http://%s:%u/%s%s\n", host,
                        robotweax_srt_telemetry_http_server_port(server), token[0] ? "#token=" : "", token);
            std::fflush(stdout);
        }
    }
    const auto start = Clock::now();
    while (ok && !stop_requested &&
           (duration <= 0 || std::chrono::duration<double>(Clock::now() - start).count() < duration))
    {
        for (auto &stream : streams)
        {
            advance(*stream);
            poll_endpoint(sampler, stream->sender);
            poll_endpoint(sampler, stream->receiver);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    robotweax_srt_telemetry_http_server_stop(server);
    for (auto &stream : streams)
    {
        close_stream(sampler, *stream);
    }
    robotweax_srt_telemetry_sampler_destroy(sampler);
    srt_cleanup();
    return ok ? 0 : 1;
}
