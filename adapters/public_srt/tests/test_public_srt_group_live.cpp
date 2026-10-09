// Live group test: a Broadcast and a Main/Backup group with two members over
// loopback, sampled on both sides with group sessions while the application
// sends. Checks member listing, path naming, carrying and the active member,
// the failover after closing the primary member, and that the group counters
// equal what the application sent and received.
//
// Exit code 77 (skipped) when the SRT library has no group support (Haivision
// SRT without ENABLE_BONDING), unless --require is given.
#include <robotweax/srt_telemetry_public_srt_host.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
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

using Result = robotweax_srt_telemetry_public_srt_group_result_v1;
using Clock = std::chrono::steady_clock;

std::uint64_t now_ns()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

std::uint64_t wall_ns()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

struct Session
{
    robotweax_srt_telemetry_public_srt_group_session *session = nullptr;
    std::unique_ptr<Result> result = std::make_unique<Result>();
    std::vector<robotweax_srt_telemetry_group_member_v1> members =
        std::vector<robotweax_srt_telemetry_group_member_v1>(8);
    ~Session()
    {
        robotweax_srt_telemetry_public_srt_group_session_destroy(session);
    }
    bool create(const std::int32_t group, const std::uint32_t type, const bool labeled = false)
    {
        robotweax_srt_telemetry_public_srt_group_config_v1 config{};
        config.struct_size = sizeof(config);
        config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        config.group = group;
        config.group_type = type;
        config.capabilities = ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1 |
                              ROBOTWEAX_SRT_TELEMETRY_CAP_GROUP_MEMBERS_V1;
        config.group_qualified_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_CANDIDATE_FIELDS;
        config.group_qualified_ext_fields =
            ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_EXT_CANDIDATE_FIELDS;
        config.member_qualified_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_V1_5_5_CANDIDATE_FIELDS;
        robotweax_srt_telemetry_host_group_functions(&config);
        (void)labeled;
#if defined(ROBOTWEAX_SRT_PATHID_NEGOTIATED)
        if (labeled)
        {
            config.capabilities |= ROBOTWEAX_SRT_TELEMETRY_CAP_GROUP_PATH_IDENTIFIERS_V1;
        }
#endif
        if (robotweax_srt_telemetry_public_srt_group_session_create(&config, &session) !=
            ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            return false;
        }
#if defined(ROBOTWEAX_SRT_PATHID_NEGOTIATED)
        if (labeled)
        {
            return robotweax_srt_telemetry_public_srt_group_session_set_path_data_v1(
                       session, robotweax_srt_group_path_data_v1) == ROBOTWEAX_SRT_TELEMETRY_OK;
        }
#endif
        return true;
    }
    robotweax_srt_telemetry_error sample()
    {
        std::memset(result.get(), 0, sizeof(Result));
        result->struct_size = sizeof(Result);
        result->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        result->members = members.data();
        result->member_capacity = static_cast<std::uint32_t>(members.size());
        result->member_entry_size = sizeof(robotweax_srt_telemetry_group_member_v1);
        return robotweax_srt_telemetry_public_srt_group_session_sample(session, wall_ns(), now_ns(),
                                                                       result.get());
    }
};

const robotweax_srt_telemetry_group_member_v1 *member(const Result &result, const std::int32_t socket)
{
    for (std::uint32_t i = 0; i < result.member_count; ++i)
    {
        if (result.members[i].socket == socket)
        {
            return &result.members[i];
        }
    }
    return nullptr;
}

bool has(const std::uint32_t flags, const std::uint32_t flag)
{
    return (flags & flag) == flag;
}

struct Observed
{
    bool caller_active_switch = false;
    bool listener_active_switch = false;
    bool listener_primary_gone = false; /* BROKEN or removed on the accepting side */
    bool caller_primary_removed = false;
    std::int32_t listener_primary = -1;
};

int run(const bool backup, const bool require, const bool labeled = false)
{
    const std::string name = std::string(backup ? "backup" : "broadcast") + (labeled ? "-pathid" : "");
    const auto type = backup ? ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP
                             : ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BROADCAST;
    const SRTSOCKET group = srt_create_group(backup ? SRT_GTYPE_BACKUP : SRT_GTYPE_BROADCAST);
    if (group == SRT_INVALID_SOCK)
    {
        std::cout << "SKIP: this SRT library has no connection groups ("
                  << srt_getlasterror_str() << ")\n";
        return require ? 1 : 77;
    }

    const SRTSOCKET listener = srt_create_socket();
    int yes = 1;
    srt_setsockflag(listener, SRTO_GROUPCONNECT, &yes, sizeof(yes));
#if defined(ROBOTWEAX_SRT_PATHID_NEGOTIATED)
    if (labeled)
    {
        check(srt_setsockflag(group, SRTO_ROBOTWEAX_PATHID_REQUIRED, &yes, sizeof(yes)) == 0 &&
                  srt_setsockflag(listener, SRTO_ROBOTWEAX_PATHID_REQUIRED, &yes, sizeof(yes)) == 0,
              name + ": bilateral path policy");
    }
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0; /* any free port */
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    int address_length = sizeof(address);
    if (srt_bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        srt_listen(listener, 4) != 0 ||
        srt_getsockname(listener, reinterpret_cast<sockaddr *>(&address), &address_length) != 0)
    {
        std::cerr << "FAIL: listen: " << srt_getlasterror_str() << '\n';
        return 1;
    }
    const int port = ntohs(address.sin_port);

    std::atomic<SRTSOCKET> accepted{SRT_INVALID_SOCK};
    std::atomic<bool> stop_receiving{false};
    std::atomic<std::int64_t> received{0};
    std::thread receiver([&] {
        sockaddr_storage peer{};
        int length = sizeof(peer);
        const SRTSOCKET socket = srt_accept(listener, reinterpret_cast<sockaddr *>(&peer), &length);
        if (socket == SRT_INVALID_SOCK)
        {
            return;
        }
        accepted = socket;
        int timeout_ms = 100;
        srt_setsockflag(socket, SRTO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
        char buffer[1500];
        while (!stop_receiving)
        {
            SRT_MSGCTRL control = srt_msgctrl_default;
            if (srt_recvmsg2(socket, buffer, sizeof(buffer), &control) > 0)
            {
                ++received;
            }
        }
    });

    std::vector<SRT_SOCKGROUPCONFIG> endpoints;
    endpoints.push_back(srt_prepare_endpoint(nullptr, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
    endpoints.push_back(srt_prepare_endpoint(nullptr, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
    endpoints[0].weight = 20;
    endpoints[1].weight = 10;
#if defined(ROBOTWEAX_SRT_PATHID_NEGOTIATED)
    if (labeled)
    {
        for (std::size_t i = 0; i < endpoints.size(); ++i)
        {
            const unsigned char label[] = {'p', 0, static_cast<unsigned char>(i + 1)};
            endpoints[i].config = srt_create_config();
            check(endpoints[i].config != nullptr &&
                      srt_config_add(endpoints[i].config, SRTO_ROBOTWEAX_PATHID,
                                     label, sizeof(label)) == 0,
                  name + ": endpoint binary label");
        }
    }
#endif
    const auto connect_result = srt_connect_group(group, endpoints.data(), static_cast<int>(endpoints.size()));
    for (auto &endpoint : endpoints)
    {
        srt_delete_config(endpoint.config);
        endpoint.config = nullptr;
    }
    if (connect_result == SRT_ERROR)
    {
        std::cerr << "FAIL: connect_group: " << srt_getlasterror_str() << '\n';
        srt_close(listener);
        srt_close(group);
        receiver.join();
        return 1;
    }
    const auto primary = endpoints[0].id;
    const auto secondary = endpoints[1].id;

    std::atomic<bool> stop_sending{false};
    std::atomic<std::int64_t> sent{0};
    std::thread sender([&] {
        char payload[1316];
        std::memset(payload, 0x47, sizeof(payload));
        auto next = Clock::now();
        while (!stop_sending)
        {
            SRT_MSGCTRL control = srt_msgctrl_default;
            if (srt_sendmsg2(group, payload, sizeof(payload), &control) > 0)
            {
                ++sent;
            }
            next += std::chrono::milliseconds(2);
            std::this_thread::sleep_until(next);
        }
    });

    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (accepted == SRT_INVALID_SOCK && Clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(accepted != SRT_INVALID_SOCK, name + ": the listener accepted the group");

    Session caller;
    Session callee;
    check(caller.create(group, type, labeled), name + ": caller group session");
    check(accepted == SRT_INVALID_SOCK || callee.create(accepted, type, labeled),
          name + ": listener group session");

    /* srt_connect_group returns after the first link: wait for both. */
    const auto both_up = Clock::now() + std::chrono::seconds(5);
    while (callee.session != nullptr && Clock::now() < both_up)
    {
        caller.sample();
        callee.sample();
        if (caller.result->usable_member_count == 2 && callee.result->usable_member_count == 2)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(caller.result->usable_member_count == 2 && callee.result->usable_member_count == 2,
          name + ": both members connected");

    Observed observed;
    /* Phase 1: both members up. Phase 2: primary closed on the caller. */
    for (int tick = 0; tick < 30 && callee.session != nullptr; ++tick)
    {
        if (tick == 15)
        {
            srt_close(primary);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        check(caller.sample() == ROBOTWEAX_SRT_TELEMETRY_OK, name + ": caller sample");
        check(callee.sample() == ROBOTWEAX_SRT_TELEMETRY_OK, name + ": listener sample");
        const auto &c = *caller.result;
        const auto &l = *callee.result;

        if (tick == 0)
        {
            check(c.listed_member_count == 2 && l.listed_member_count == 2,
                  name + ": both sides list two members");
            for (std::uint32_t i = 0; i < c.member_count; ++i)
            {
                const auto &m = c.members[i];
                check(m.token >= 0 &&
                          (labeled ? (m.path_source == ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_IDENTIFIER &&
                                      std::string(m.path) == (m.weight == 20 ? "pathid-700001" : "pathid-700002"))
                                   : (m.path_source == ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_TOKEN &&
                                      std::string(m.path) == "token-" + std::to_string(m.token))),
                      name + ": caller label or ordinary token path; local token preserved");
                check(m.peer_address == std::string("127.0.0.1:") + std::to_string(port),
                      name + ": caller peer address");
                check(m.local_address[0] != '\0', name + ": caller local address known");
            }
            for (std::uint32_t i = 0; i < l.member_count; ++i)
            {
                const auto &m = l.members[i];
                check(m.token == -1 &&
                          (labeled ? (m.path_source == ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_IDENTIFIER &&
                                      std::string(m.path) == (m.weight == 20 ? "pathid-700001" : "pathid-700002"))
                                   : m.path_source == ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_ADDRESSES),
                      name + ": receiver label or ordinary addresses; token remains -1");
                check(m.local_address == std::string("127.0.0.1:") + std::to_string(port),
                      name + ": listener local address");
                check(m.weight == 20 || m.weight == 10, name + ": weight reaches the listener");
            }
        }
        if (tick == 10)
        {
            const auto *p = member(c, primary);
            const auto *s = member(c, secondary);
            check(p != nullptr && s != nullptr, name + ": caller members found");
            check(c.usable_member_count == 2 && l.usable_member_count == 2,
                  name + ": both members usable");
            check(has(c.flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_STATS) &&
                      has(l.flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_STATS),
                  name + ": group statistics on both sides");
            if (backup)
            {
                check(c.active_member == primary, name + ": caller primary active");
                /* The standby may carry a little while it is probed, so only
                 * the primary's activity is required. */
                check(p != nullptr && has(p->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_CARRYING),
                      name + ": the primary carries");
                observed.listener_primary = l.active_member;
                check(l.active_member != -1, name + ": listener sees an active member");
            }
            else
            {
                check(p != nullptr && has(p->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_CARRYING) &&
                          s != nullptr && has(s->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_CARRYING),
                      name + ": both members carry");
                for (std::uint32_t i = 0; i < l.member_count; ++i)
                {
                    check(has(l.members[i].flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_CARRYING),
                          name + ": both listener members carry, whatever their member state");
                }
            }
        }
        if (tick >= 15)
        {
            const auto *p = member(c, primary);
            if (p != nullptr && has(p->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_REMOVED))
            {
                observed.caller_primary_removed = true;
            }
            if (backup && has(c.flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED) &&
                c.active_member == secondary && c.previous_active_member == primary)
            {
                observed.caller_active_switch = true;
            }
            if (backup && has(l.flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED) &&
                l.previous_active_member == observed.listener_primary)
            {
                observed.listener_active_switch = true;
            }
            const auto *lp = member(l, observed.listener_primary);
            if (observed.listener_primary != -1 &&
                (lp == nullptr || has(lp->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_REMOVED) ||
                 lp->member_state == ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATE_BROKEN))
            {
                observed.listener_primary_gone = true;
            }
        }
    }

    check(observed.caller_primary_removed, name + ": the closed member is reported removed");
    if (backup)
    {
        check(observed.caller_active_switch, name + ": caller reports the switch to the backup");
        check(observed.listener_active_switch, name + ": listener reports the switch");
        check(observed.listener_primary_gone, name + ": listener sees the primary broken or gone");
    }

    /* Counter parity: stop, drain, then compare with what the application did. */
    stop_sending = true;
    sender.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    caller.sample();
    callee.sample();
    stop_receiving = true;
    receiver.join();
    const auto &c = *caller.result;
    const auto &l = *callee.result;
    check(c.raw.unique_sent_packet_total == static_cast<std::uint64_t>(sent.load()),
          name + ": caller group unique sent equals the messages the application sent (" +
              std::to_string(c.raw.unique_sent_packet_total) + " vs " + std::to_string(sent.load()) +
              ")");
    check(l.raw.unique_received_packet_total == static_cast<std::uint64_t>(received.load()),
          name + ": listener group unique received equals the messages the application read (" +
              std::to_string(l.raw.unique_received_packet_total) + " vs " +
              std::to_string(received.load()) + ")");
    std::cout << name << ": sent " << sent.load() << ", received " << received.load()
              << ", receiver drops " << l.raw.receiver_drop_packet_total << '\n';

    srt_close(group);
    srt_close(accepted);
    srt_close(listener);
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    const bool require = argc > 1 && std::string_view(argv[1]) == "--require";
    srt_startup();
    auto status = run(true, require);
    if (status == 0)
    {
        status = run(false, require);
    }
#if defined(ROBOTWEAX_SRT_PATHID_NEGOTIATED)
    if (status == 0)
    {
        status = run(true, require, true);
    }
    if (status == 0)
    {
        status = run(false, require, true);
    }
#endif
    srt_cleanup();
    if (status != 0)
    {
        return status;
    }
    if (failures != 0)
    {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "public SRT live group tests passed\n";
    return 0;
}
