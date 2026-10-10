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
 * With --group backup (or broadcast), also a connection group on both sides:
 *   bonded-1     paths lte and sat through their own relays; lte cycles:
 *                clean 40 s, 14 s outage (its member breaks), reconnect
 * The embedding layer monitors the group (one row per group and per path);
 * the other streams are plain sampler sources on the embed's sampler.
 *
 * Usage: dashboard_demo [--port N] [--bind ADDRESS] [--token T]
 *                       [--duration SECONDS] [--streams N] [--group backup|broadcast]
 *                       [--frame-ancestors "https://console.example.com"]
 *                       [--otlp http://127.0.0.1:4318/v1/metrics] [--otlp-interval MS] */

#include <robotweax/srt_telemetry_embed.h>
#include <robotweax/srt_telemetry_http.h>
#include <robotweax/srt_telemetry_otlp.h>
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

/* A connection group on loopback: the caller group sends through one relay
 * per path to a listener group (SRTO_GROUPCONNECT). Path lte (weight 20) has
 * an outage schedule; the demo reconnects it when the outage ends. */
struct GroupStream
{
    SRT_GROUP_TYPE type = SRT_GTYPE_BACKUP;
    SRTSOCKET listener = SRT_INVALID_SOCK;
    SRTSOCKET group = SRT_INVALID_SOCK;
    std::atomic<SRTSOCKET> accepted{SRT_INVALID_SOCK};
    bool attached_receiver = false;
    Relay relays[2]; /* lte, sat */
    std::vector<SRT_SOCKGROUPCONFIG> endpoints;
    std::vector<Phase> lte_phases;
    std::size_t phase = 0;
    Clock::time_point phase_start;
    Clock::time_point last_reconnect;
    std::atomic<bool> running{true};
    std::thread accept_thread;
    std::thread send_thread;
};

constexpr const char *path_names[2] = {"lte", "sat"};
constexpr std::uint16_t path_weights[2] = {20, 10};

std::uint16_t port_of(const sockaddr_in &address)
{
    return ntohs(address.sin_port);
}

/* Caller side: the weight names the path. */
int name_caller_path(void *, const robotweax_srt_telemetry_group_member_identity_v1 *member, char *name,
                     const std::size_t capacity)
{
    for (std::size_t i = 0; i < 2; ++i)
    {
        if (member->weight == path_weights[i])
        {
            std::snprintf(name, capacity, "%s", path_names[i]);
            return 1;
        }
    }
    return 0;
}

/* Listener side: the peer is the relay's back socket. */
int name_listener_path(void *user, const robotweax_srt_telemetry_group_member_identity_v1 *member, char *name,
                       const std::size_t capacity)
{
    const auto &stream = *static_cast<const GroupStream *>(user);
    const char *colon = std::strrchr(member->peer_address, ':');
    const int port = colon != nullptr ? std::atoi(colon + 1) : -1;
    for (std::size_t i = 0; i < 2; ++i)
    {
        if (port == port_of(stream.relays[i].back_address()))
        {
            std::snprintf(name, capacity, "%s", path_names[i]);
            return 1;
        }
    }
    return 0;
}

bool open_group(robotweax_srt_telemetry_embed *embed, GroupStream &stream)
{
    Impairment clean;
    Impairment outage;
    outage.blackhole = true;
    stream.lte_phases = {{clean, 40}, {outage, 14}};
    stream.listener = srt_create_socket();
    const int yes = 1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    if (srt_setsockflag(stream.listener, SRTO_GROUPCONNECT, &yes, sizeof(yes)) == SRT_ERROR ||
        !set_int(stream.listener, SRTO_LATENCY, 250) ||
        srt_bind(stream.listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SRT_ERROR ||
        srt_getsockname(stream.listener, reinterpret_cast<sockaddr *>(&address), reinterpret_cast<int *>(&size)) ==
            SRT_ERROR ||
        srt_listen(stream.listener, 4) == SRT_ERROR || !stream.relays[0].start(address) ||
        !stream.relays[1].start(address))
    {
        return false;
    }
    Impairment satellite;
    satellite.delay_ms = 40;
    stream.relays[1].set(satellite);
    stream.accept_thread = std::thread(
        [&stream]
        {
            sockaddr_storage peer{};
            int length = sizeof(peer);
            const SRTSOCKET socket = srt_accept(stream.listener, reinterpret_cast<sockaddr *>(&peer), &length);
            if (socket == SRT_INVALID_SOCK)
            {
                return;
            }
            static_cast<void>(set_int(socket, SRTO_RCVTIMEO, 200));
            stream.accepted = socket;
            std::array<char, 1500> buffer{};
            while (stream.running)
            {
                SRT_MSGCTRL control = srt_msgctrl_default;
                if (srt_recvmsg2(socket, buffer.data(), static_cast<int>(buffer.size()), &control) <= 0 &&
                    srt_getsockstate(socket) != SRTS_CONNECTED)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        });
    stream.group = srt_create_group(stream.type);
    if (stream.group == SRT_INVALID_SOCK || !set_int(stream.group, SRTO_LATENCY, 250) ||
        !set_int(stream.group, SRTO_SNDTIMEO, 200))
    {
        return false;
    }
    for (std::size_t i = 0; i < 2; ++i)
    {
        const auto &relay = stream.relays[i].address();
        stream.endpoints.push_back(
            srt_prepare_endpoint(nullptr, reinterpret_cast<const sockaddr *>(&relay), sizeof(relay)));
        stream.endpoints.back().weight = path_weights[i];
    }
    if (srt_connect_group(stream.group, stream.endpoints.data(), 2) == SRT_ERROR)
    {
        return false;
    }
    for (int i = 0; i < 250 && stream.accepted == SRT_INVALID_SOCK; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (stream.accepted == SRT_INVALID_SOCK)
    {
        return false;
    }
    const robotweax_srt_telemetry_label_v1 labels[] = {{"stream", "bonded-1"}, {"site", "dresden"}};
    robotweax_srt_telemetry_embed_group_v1 g;
    robotweax_srt_telemetry_embed_group_default(&g);
    g.group = stream.group;
    g.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    g.group_type = stream.type == SRT_GTYPE_BACKUP ? ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP
                                                   : ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BROADCAST;
    g.expected_paths = 2;
    g.labels = labels;
    g.label_count = 2;
    g.path_name = &name_caller_path;
    if (robotweax_srt_telemetry_embed_attach_group(embed, &g) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return false;
    }
    g.group = stream.accepted;
    g.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;
    g.path_name = &name_listener_path;
    g.path_name_user_data = &stream;
    if (robotweax_srt_telemetry_embed_attach_group(embed, &g) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return false;
    }
    stream.attached_receiver = true;
    stream.phase = 0;
    stream.phase_start = Clock::now();
    stream.last_reconnect = Clock::now();
    stream.relays[0].set(stream.lte_phases.front().impairment);
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
                SRT_MSGCTRL control = srt_msgctrl_default;
                static_cast<void>(srt_sendmsg2(stream.group, payload.data(), payload_bytes, &control));
            }
        });
    return true;
}

/* Closes broken members (Robotweax SRT keeps them listed until closed) and,
 * with the lte path clean again, reconnects it when it has no member. */
void maintain_group(GroupStream &stream)
{
    const auto &phase = stream.lte_phases[stream.phase];
    if (phase.seconds > 0 &&
        std::chrono::duration<double>(Clock::now() - stream.phase_start).count() >= phase.seconds)
    {
        stream.phase = (stream.phase + 1) % stream.lte_phases.size();
        stream.phase_start = Clock::now();
        stream.relays[0].set(stream.lte_phases[stream.phase].impairment);
    }
    for (const SRTSOCKET group : {stream.group, stream.accepted.load()})
    {
        std::array<SRT_SOCKGROUPDATA, 8> members{};
        std::size_t count = members.size();
        if (srt_group_data(group, members.data(), &count) == SRT_ERROR)
        {
            continue;
        }
        bool lte_listed = false;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (members[i].memberstate == SRT_GST_BROKEN || members[i].sockstate > SRTS_CONNECTED)
            {
                srt_close(members[i].id);
            }
            else if (group == stream.group && members[i].weight == path_weights[0])
            {
                lte_listed = true;
            }
        }
        if (group == stream.group && !lte_listed && !stream.lte_phases[stream.phase].impairment.blackhole &&
            Clock::now() - stream.last_reconnect > std::chrono::seconds(2))
        {
            stream.last_reconnect = Clock::now();
            auto endpoint = stream.endpoints[0];
            endpoint.id = SRT_INVALID_SOCK;
            static_cast<void>(srt_connect_group(stream.group, &endpoint, 1));
        }
    }
}

void close_group(robotweax_srt_telemetry_embed *embed, GroupStream &stream)
{
    stream.running = false;
    if (stream.send_thread.joinable())
    {
        stream.send_thread.join();
    }
    if (stream.group != SRT_INVALID_SOCK)
    {
        static_cast<void>(robotweax_srt_telemetry_embed_detach(embed, stream.group));
    }
    if (stream.attached_receiver)
    {
        static_cast<void>(robotweax_srt_telemetry_embed_detach(embed, stream.accepted));
    }
    for (const SRTSOCKET socket : {stream.group, stream.listener})
    {
        if (socket != SRT_INVALID_SOCK)
        {
            srt_close(socket); /* closing the listener ends a pending accept */
        }
    }
    if (stream.accept_thread.joinable())
    {
        stream.accept_thread.join();
    }
    if (stream.accepted != SRT_INVALID_SOCK)
    {
        srt_close(stream.accepted);
    }
}

} // namespace

int main(const int argc, char **argv)
{
    int port = 9810;
    const char *bind = "127.0.0.1";
    const char *token = "";
    const char *frame_ancestors = "";
    const char *otlp_endpoint = "";
    int otlp_interval_ms = 10'000;
    double duration = 0;
    std::size_t stream_count = 5;
    std::string group_type;
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
        else if (option == "--otlp")
        {
            otlp_endpoint = argv[i + 1];
        }
        else if (option == "--otlp-interval")
        {
            otlp_interval_ms = std::atoi(argv[i + 1]);
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
        else if (option == "--group")
        {
            group_type = argv[i + 1];
            if (group_type != "backup" && group_type != "broadcast")
            {
                std::fprintf(stderr, "--group takes backup or broadcast\n");
                return 2;
            }
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

    /* The embed ticks the sampler (health, 1 h history, 4096 events) and
     * monitors the group; the demo serves the sampler itself. */
    robotweax_srt_telemetry_embed_config_v1 embed_config;
    robotweax_srt_telemetry_embed_config_default(&embed_config);
    robotweax_srt_telemetry_host_embed_functions(&embed_config);
    embed_config.flags = ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP;
    embed_config.history_seconds = 3600;
    embed_config.journal_capacity = 4096;
    robotweax_srt_telemetry_embed *embed = nullptr;
    if (robotweax_srt_telemetry_embed_start(&embed_config, &embed) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        std::fprintf(stderr, "embed setup failed\n");
        return 1;
    }
#if defined(ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION) && ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION == 1
    robotweax_srt_telemetry_embed_set_group_send_state_v1(embed, robotweax_srt_group_send_state_v1);
#endif
#if defined(ROBOTWEAX_SRT_RTTDATA_VERSION) && ROBOTWEAX_SRT_RTTDATA_VERSION == 1
    /* Demo's explicit candidate-build opt-in; old providers retain absence. */
    static_cast<void>(robotweax_srt_telemetry_host_embed_rtt_data(embed));
#endif
    auto *sampler = robotweax_srt_telemetry_embed_sampler(embed);

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
    std::unique_ptr<GroupStream> group;
    if (ok && !group_type.empty())
    {
        group = std::make_unique<GroupStream>();
        group->type = group_type == "backup" ? SRT_GTYPE_BACKUP : SRT_GTYPE_BROADCAST;
        if (!open_group(embed, *group))
        {
            /* Haivision SRT built without ENABLE_BONDING has no groups. */
            std::fprintf(stderr, "group: unavailable: %s\n", srt_getlasterror_str());
            close_group(embed, *group);
            group.reset();
        }
    }
    robotweax_srt_telemetry_http_server *server = nullptr;
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
            if (!group_type.empty())
            {
                std::printf("group: %s\n", group ? group_type.c_str() : "unavailable");
            }
            std::fflush(stdout);
        }
    }
    robotweax_srt_telemetry_otlp_exporter *exporter = nullptr;
    if (ok && otlp_endpoint[0] != '\0')
    {
        robotweax_srt_telemetry_otlp_exporter_config_v1 otlp{};
        robotweax_srt_telemetry_otlp_exporter_config_default(sampler, &otlp);
        std::snprintf(otlp.endpoint, sizeof(otlp.endpoint), "%s", otlp_endpoint);
        otlp.interval_ms = static_cast<std::uint32_t>(otlp_interval_ms);
        const robotweax_srt_telemetry_label_v1 resource[] = {{"service.name", "srt-dashboard-demo"},
                                                             {"service.instance.id", "demo-1"}};
        otlp.resource_attributes = resource;
        otlp.resource_attribute_count = 2;
        const auto error = robotweax_srt_telemetry_otlp_exporter_start(&otlp, &exporter);
        if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            std::fprintf(stderr, "OTLP exporter failed: %s\n", robotweax_srt_telemetry_error_string(error));
            ok = false;
        }
        else
        {
            std::printf("otlp: %s every %d ms\n", otlp_endpoint, otlp_interval_ms);
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
        if (group)
        {
            maintain_group(*group);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    robotweax_srt_telemetry_otlp_exporter_stop(exporter);
    robotweax_srt_telemetry_http_server_stop(server);
    if (group)
    {
        close_group(embed, *group);
    }
    for (auto &stream : streams)
    {
        close_stream(sampler, *stream);
    }
    robotweax_srt_telemetry_embed_stop(embed);
    srt_cleanup();
    return ok ? 0 : 1;
}
