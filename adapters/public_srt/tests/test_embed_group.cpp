// Embedded connection groups, live: a Main/Backup group with two members over
// loopback, attached on both sides with embed_attach_group. Checks the
// group and member sources (labels group and path), the group events in the
// journal, the member verdict cap after a path is cut, the continuation of
// the path's row when a member reconnects on it, and detach.
//
// Exit code 77 (skipped) without connection groups, unless --require.
#include <robotweax/srt_telemetry_public_srt_host.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <set>
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

void sleep_ms(const int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/* The JSON snapshot, one string per source object. */
std::vector<std::string> sources(robotweax_srt_telemetry_embed *embed)
{
    robotweax_srt_telemetry_render_options_v1 options;
    robotweax_srt_telemetry_render_options_default(ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON, &options);
    std::vector<char> buffer(1 << 20);
    std::size_t length = 0;
    if (robotweax_srt_telemetry_sampler_render(robotweax_srt_telemetry_embed_sampler(embed), &options,
                                               buffer.data(), buffer.size(), &length) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return {};
    }
    const std::string json(buffer.data(), length);
    std::vector<std::string> out;
    const std::string marker = "{\"id\":";
    for (auto at = json.find(marker); at != std::string::npos;)
    {
        const auto next = json.find(marker, at + 1);
        out.push_back(json.substr(at, next == std::string::npos ? std::string::npos : next - at));
        at = next;
    }
    return out;
}

bool has(const std::string &text, const std::string &part)
{
    return text.find(part) != std::string::npos;
}

int count_with(const std::vector<std::string> &all, const std::vector<std::string> &parts)
{
    int count = 0;
    for (const auto &source : all)
    {
        bool every = true;
        for (const auto &part : parts)
        {
            /* A path label belongs to labels, not the nested authority snapshot. */
            const auto label_prefix = source.substr(0, source.find("},\"up\":"));
            every = every && has(part.starts_with("\"path\":") ? label_prefix : source, part);
        }
        count += every ? 1 : 0;
    }
    return count;
}

std::string find_source(const std::vector<std::string> &all, const std::vector<std::string> &parts)
{
    for (const auto &source : all)
    {
        bool every = true;
        for (const auto &part : parts)
        {
            /* A path label belongs to labels, not the nested authority snapshot. */
            const auto label_prefix = source.substr(0, source.find("},\"up\":"));
            every = every && has(part.starts_with("\"path\":") ? label_prefix : source, part);
        }
        if (every)
        {
            return source;
        }
    }
    return {};
}

/* The group's own row: the group label without a path. */
std::string group_row(const std::vector<std::string> &all, const std::string &stream)
{
    for (const auto &source : all)
    {
        if (has(source, "\"stream\":\"" + stream + "\"") && has(source, "\"group\":") &&
            !has(source.substr(0, source.find("},\"up\":")), "\"path\":"))
        {
            return source;
        }
    }
    return {};
}

/* Polls until done() or the deadline; true when done. */
template <typename F> bool wait_for(F done, const int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 50)
    {
        if (done())
        {
            return true;
        }
        sleep_ms(50);
    }
    return done();
}

struct Journal
{
    std::vector<robotweax_srt_telemetry_event_v1> events;
    void read(robotweax_srt_telemetry_embed *embed)
    {
        events.resize(4096);
        std::size_t count = 0;
        static_cast<void>(robotweax_srt_telemetry_sampler_journal_read(robotweax_srt_telemetry_embed_sampler(embed),
                                                                      0, events.data(), events.size(), &count,
                                                                      nullptr));
        events.resize(count);
    }
    int count(const robotweax_srt_telemetry_event_kind kind, const std::string &detail = "") const
    {
        int n = 0;
        for (const auto &e : events)
        {
            n += e.kind == kind && (detail.empty() || has(e.detail, detail)) ? 1 : 0;
        }
        return n;
    }
};

/* Main/Backup on both sides: rows, events, cut, reconnect, detach. */
void backup_scenario()
{
    const SRTSOCKET caller_group = srt_create_group(SRT_GTYPE_BACKUP);

    robotweax_srt_telemetry_embed_config_v1 config;
    robotweax_srt_telemetry_embed_config_default(&config);
    robotweax_srt_telemetry_host_embed_functions(&config);
    config.flags = ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP;
    config.linger_seconds = 30;
    robotweax_srt_telemetry_embed *embed = nullptr;
    check(robotweax_srt_telemetry_embed_start(&config, &embed) == ROBOTWEAX_SRT_TELEMETRY_OK, "embed start");
#if defined(ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION) && ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION == 1
    check(robotweax_srt_telemetry_host_embed_group_send_state(embed) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "explicitly enable qualified group sender observation");
#endif
#if defined(ROBOTWEAX_SRT_RTTDATA_VERSION) && ROBOTWEAX_SRT_RTTDATA_VERSION == 1
    check(robotweax_srt_telemetry_host_embed_rtt_data(embed) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "explicitly enable qualified RTT getter");
#endif

    if (embed == nullptr)
    {
        return;
    }

    const SRTSOCKET listener = srt_create_socket();
    int yes = 1;
    srt_setsockflag(listener, SRTO_GROUPCONNECT, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    int address_length = sizeof(address);
    check(srt_bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
              srt_listen(listener, 4) == 0 &&
              srt_getsockname(listener, reinterpret_cast<sockaddr *>(&address), &address_length) == 0,
          "listen");

    std::atomic<SRTSOCKET> accepted{SRT_INVALID_SOCK};
    std::atomic<bool> stop{false};
    std::thread receiver([&] {
        sockaddr_storage peer{};
        int length = sizeof(peer);
        const SRTSOCKET socket = srt_accept(listener, reinterpret_cast<sockaddr *>(&peer), &length);
        if (socket == SRT_INVALID_SOCK)
        {
            return;
        }
        accepted = socket;
        int timeout = 100;
        srt_setsockflag(socket, SRTO_RCVTIMEO, &timeout, sizeof(timeout));
        char buffer[1500];
        while (!stop)
        {
            SRT_MSGCTRL control = srt_msgctrl_default;
            static_cast<void>(srt_recvmsg2(socket, buffer, sizeof(buffer), &control));
        }
    });

    std::vector<SRT_SOCKGROUPCONFIG> endpoints;
    for (const std::uint16_t weight : {20, 10})
    {
        endpoints.push_back(
            srt_prepare_endpoint(nullptr, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
        endpoints.back().weight = weight;
    }
    check(srt_connect_group(caller_group, endpoints.data(), 2) != SRT_ERROR, "connect group");
    const auto primary = endpoints[0].id;
    const auto primary_token = endpoints[0].token;
    const std::string primary_path = "token-" + std::to_string(primary_token);

    std::thread sender([&] {
        char payload[1316];
        std::memset(payload, 0x47, sizeof(payload));
        auto next = std::chrono::steady_clock::now();
        while (!stop)
        {
            SRT_MSGCTRL control = srt_msgctrl_default;
            static_cast<void>(srt_sendmsg2(caller_group, payload, sizeof(payload), &control));
            next += std::chrono::milliseconds(2);
            std::this_thread::sleep_until(next);
        }
    });
    for (int i = 0; i < 100 && accepted == SRT_INVALID_SOCK; ++i)
    {
        sleep_ms(20);
    }

    /* Attach both sides. */
    robotweax_srt_telemetry_label_v1 tx_labels[] = {{"stream", "cam-tx"}};
    robotweax_srt_telemetry_label_v1 rx_labels[] = {{"stream", "cam-rx"}};
    robotweax_srt_telemetry_embed_group_v1 tx;
    robotweax_srt_telemetry_embed_group_default(&tx);
    tx.group = caller_group;
    tx.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    tx.group_type = ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP;
    tx.labels = tx_labels;
    tx.label_count = 1;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &tx) == ROBOTWEAX_SRT_TELEMETRY_OK, "attach caller group");
    check(robotweax_srt_telemetry_embed_attach_group(embed, &tx) == ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE,
          "a group is attached once");
    auto rx = tx;
    rx.group = accepted;
    rx.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;
    rx.labels = rx_labels;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &rx) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "attach accepted group");

    auto bad = tx;
    bad.group_type = 7;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &bad) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "unknown group type rejected");
    robotweax_srt_telemetry_label_v1 reserved_key[] = {{"path", "x"}};
    bad = tx;
    bad.labels = reserved_key;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &bad) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "the path label belongs to the embed");

    /* Both members of both sides get rows once connected. */
    std::vector<std::string> all;
    for (int i = 0; i < 100; ++i)
    {
        sleep_ms(50);
        all = sources(embed);
        if (count_with(all, {"\"path\":"}) >= 4)
        {
            break;
        }
    }
    sleep_ms(2500); /* a few windows of data */
    all = sources(embed);
    check(all.size() == 6, "two groups and four members: " + std::to_string(all.size()) + " sources");
    check(count_with(all, {"\"stream\":\"cam-tx\"", "\"group\":\"backup\""}) == 3, "caller group and its members");
    check(count_with(all, {"\"stream\":\"cam-rx\"", "\"group\":\"backup\""}) == 3, "listener group and its members");
    check(count_with(all, {"\"stream\":\"cam-tx\"", "\"path\":\"token-"}) == 2, "caller paths from tokens");
    check(count_with(all, {"\"stream\":\"cam-rx\"", "\"path\":\"127.0.0.1:"}) == 2,
          "listener paths from addresses (no token on the accepting side)");
    check(count_with(all, {"\"provider\":"}) == 6, "every source has the provider label");
    check(count_with(all, {"\"path\":", "\"verdict\":\"ok\"", "\"reason\":\"standby\""}) == 2,
          "one idle standby per side is OK / standby");
    check(count_with(all, {"\"path\":", "\"reason\":\"standby\"", "\"standby\":true"}) == 2,
          "the snapshot marks each waiting path as standby");
    check(count_with(all, {"\"path\":", "\"verdict\":\"unknown\""}) == 0, "no member is ungraded");
    check(!group_row(all, "cam-tx").empty() && !group_row(all, "cam-rx").empty(), "group rows found");
    check(wait_for(
              [&] {
                  const auto row = group_row(sources(embed), "cam-tx");
                  return has(row, "\"verdict\":\"ok\"") &&
                         has(row, "\"group\":{\"usable_paths\":2,\"expected_paths\":2}");
              },
              6000),
          "the sending group is graded through its paths: OK, 2 of 2 paths");
    check(has(group_row(all, "cam-rx"), "\"verdict\":\"ok\""), "the receiving group is graded OK");

#if defined(ROBOTWEAX_SRT_RTTDATA_VERSION) && ROBOTWEAX_SRT_RTTDATA_VERSION == 1
    check(has(find_source(all, {"\"stream\":\"cam-tx\"", "\"path\":\"" + primary_path + "\""}),
              "\"rtt_metadata\":{\"version\":1,\"availability\":\"available\""),
          "connected group member has RTT metadata");
#endif
#if defined(ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION) && ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION == 1
    check(has(group_row(all, "cam-tx"),
              "\"send_authority\":{\"version\":1,\"availability\":\"available\""),
          "sending group exposes public sender observation");
    check(has(group_row(all, "cam-tx"), "\"state\":\"selected\""),
          "sending group has selected authority");
    check(has(group_row(all, "cam-rx"), "\"state\":\"unselected\""),
          "receiver does not inherit remote sender authority");
#endif
    Journal journal;
    journal.read(embed);
    check(journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_ADDED) == 4, "member_added for every member");
    check(journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_ACTIVE_MEMBER_CHANGED, "path=none -> " + primary_path) >= 1,
          "caller: the primary carries the most observed packets");
    for (const auto &e : journal.events)
    {
        if (e.kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_ACTIVE_MEMBER_CHANGED)
        {
            check(std::string_view(e.detail).starts_with("inferred; authority=unavailable; "),
                  "data-path changes disclose inference before potentially truncated names");
        }
    }
#if defined(ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION) && ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION == 1
    {
        std::set<std::uint64_t> authority_sources;
        for (const auto &e : journal.events)
        {
            if (e.kind != ROBOTWEAX_SRT_TELEMETRY_EVENT_SEND_AUTHORITY_CHANGED)
                continue;
            authority_sources.insert(e.source_id);
            check(has(e.detail, "; member_generation=") && !has(e.detail, "revision=0;"),
                  "authority events name the member generation and never report an empty baseline");
        }
        check(authority_sources.size() == 1,
              "only the sending group journals authority; the receiving group never selected a member");
    }
#endif
    for (const auto &e : journal.events)
    {
        if (e.kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_ADDED)
        {
            check(e.observed_at_unix_nano != 0 && e.sample_monotonic_nano != 0 && e.source_id != 0,
                  "group events carry the tick's timestamps and the group's source");
        }
    }

    check(journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_FAILED) == 0, "no failed statistics reads");

    /* Cut the primary path on the caller. */
    srt_close(primary);
    check(wait_for(
              [&] {
                  journal.read(embed);
                  return journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_REMOVED, "path=" + primary_path) == 1 &&
                         journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_ACTIVE_MEMBER_CHANGED,
                                       "path=" + primary_path + " -> ") >= 1;
              },
              5000),
          "the cut is journaled");
    sleep_ms(1500); /* the cut path's verdict settles */
    journal.read(embed);
    check(journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_REMOVED, "path=" + primary_path) == 1,
          "member_removed for the cut path");
    check(journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_ACTIVE_MEMBER_CHANGED, "path=" + primary_path + " -> ") >= 1,
          "caller: the active member switches away from the cut path");
    all = sources(embed);
    const auto cut = find_source(all, {"\"stream\":\"cam-tx\"", "\"path\":\"" + primary_path + "\""});
    check(has(cut, "\"verdict\":\"watch\"") && has(cut, "\"capped\":true") &&
              has(cut, "\"uncapped_verdict\":\"failing\"") && has(cut, "\"reason\":\"connection_lost\""),
          "the cut path is Watch, capped from Failing, with the reason: " + cut.substr(0, 0));
    check(!has(group_row(all, "cam-tx"), "\"verdict\":\"failing\""),
          "the group is not Failing while the backup carries");
    check(wait_for(
              [&] {
                  const auto row = group_row(sources(embed), "cam-tx");
                  return has(row, "\"verdict\":\"at_risk\"") && has(row, "\"reason\":\"redundancy_lost\"") &&
                         has(row, "\"usable_paths\":1,\"expected_paths\":2");
              },
              6000),
          "the sending group runs on its last path: At risk / redundancy_lost");
    /* The backup took over: it is the last usable path, graded on its own;
     * its standby period left no latched grade, so it settles to OK. */
    check(wait_for(
              [&] {
                  const auto backup_row =
                      find_source(sources(embed), {"\"stream\":\"cam-tx\"", "\"path\":", "\"verdict\":\"ok\"",
                                                   "\"capped\":false", "\"group_member\":true"});
                  return !backup_row.empty();
              },
              6000),
          "the last usable path is graded on its own (not capped), still a group path, and OK");
    check(count_with(sources(embed), {"\"stream\":\"cam-tx\"", "\"path\":", "\"verdict\":\"failing\""}) == 0,
          "no path of the working group is Failing");

    /* Reconnect the cut path with its token: the path's row continues. */
    std::vector<SRT_SOCKGROUPCONFIG> again;
    again.push_back(srt_prepare_endpoint(nullptr, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
    again.back().weight = 20;
    again.back().token = primary_token;
    const auto before = all.size();
    check(srt_connect_group(caller_group, again.data(), 1) != SRT_ERROR, "reconnect the path");
    check(wait_for(
              [&] {
                  journal.read(embed);
                  return journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_ADDED, "path=" + primary_path) == 2;
              },
              5000),
          "the reconnect is journaled");
    check(wait_for(
              [&] {
                  const auto row = group_row(sources(embed), "cam-tx");
                  return has(row, "\"verdict\":\"ok\"") && has(row, "\"usable_paths\":2,\"expected_paths\":2");
              },
              15000),
          "redundancy back: the group returns to OK after the leave hold");
    sleep_ms(1500);
    all = sources(embed);
    journal.read(embed);
    check(count_with(all, {"\"stream\":\"cam-tx\""}) == 3,
          "caller: the reconnected path continues its row (same token, no new source)");
    /* The accepting side has no path identity (SRT reports token -1) and the
     * new member comes from a new port: a new address-named row, as
     * documented; host path names fix this where the addresses differ. */
    check(count_with(all, {"\"stream\":\"cam-rx\"", "\"path\":"}) == 3,
          "listener: the reconnected member is a new address-named path");
    check(journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_ADDED, "path=" + primary_path) == 2,
          "member_added again for the reconnected path");
    const auto back = find_source(all, {"\"stream\":\"cam-tx\"", "\"path\":\"" + primary_path + "\""});
    check(!has(back, "\"reason\":\"connection_lost\""), "the reconnected path is no longer lost");

#if defined(ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION) && ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION == 1
    check(robotweax_srt_telemetry_embed_set_group_send_state_v1(embed, nullptr) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "disable public sender observations");
    check(has(group_row(sources(embed), "cam-tx"),
              "\"send_authority\":{\"version\":1,\"availability\":\"unavailable\"}"),
          "disable immediately clears stale sender authority");
    check(robotweax_srt_telemetry_host_embed_group_send_state(embed) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "re-enable starts a fresh comparison baseline");
    check(wait_for(
              [&]
              {
                  return has(group_row(sources(embed), "cam-tx"),
                             "\"send_authority\":{\"version\":1,\"availability\":\"available\"");
              },
              1000),
          "re-enabled getter resumes observations without resetting SRT");
#endif
    /* Detach: every row stays as lost for the linger. */
    check(robotweax_srt_telemetry_embed_detach(embed, caller_group) == ROBOTWEAX_SRT_TELEMETRY_OK, "detach caller");
    check(robotweax_srt_telemetry_embed_detach(embed, accepted) == ROBOTWEAX_SRT_TELEMETRY_OK, "detach listener");
    sleep_ms(400);
    all = sources(embed);
    check(all.size() == before + 1, "detached groups and members linger");
    const auto lost_member = find_source(all, {"\"stream\":\"cam-rx\"", "\"path\":"});
    check(has(lost_member, "\"verdict\":\"watch\""), "a detached member is Watch, not Failing");

    stop = true;
    sender.join();
    srt_close(caller_group);
    receiver.join();
    srt_close(accepted);
    srt_close(listener);
    robotweax_srt_telemetry_embed_stop(embed);
}

/* Host path names by weight: the caller's paths are "lte" and "sat". */
int name_by_weight(void *user_data, const robotweax_srt_telemetry_group_member_identity_v1 *member, char *name,
                   const size_t capacity)
{
    const bool duplicate = user_data != nullptr;
    if (member->token < 0)
    {
        return 0; /* the accepting side: automatic names */
    }
    std::snprintf(name, capacity, "%s", duplicate ? "same" : member->weight >= 20 ? "lte" : "sat");
    return 1;
}

/* One caller group of the given type with two members (weights 20 and 10)
 * to an in-process listener, application sending. */
struct GroupRun
{
    SRTSOCKET listener = SRT_INVALID_SOCK;
    SRTSOCKET group = SRT_INVALID_SOCK;
    std::atomic<SRTSOCKET> accepted{SRT_INVALID_SOCK};
    std::atomic<bool> stop{false};
    std::thread receiver;
    std::thread sender;
    std::vector<SRT_SOCKGROUPCONFIG> endpoints;

    bool start(const SRT_GROUP_TYPE type)
    {
        listener = srt_create_socket();
        int yes = 1;
        srt_setsockflag(listener, SRTO_GROUPCONNECT, &yes, sizeof(yes));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        int length = sizeof(address);
        if (srt_bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            srt_listen(listener, 4) != 0 ||
            srt_getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) != 0)
        {
            return false;
        }
        receiver = std::thread([this] {
            sockaddr_storage peer{};
            int size = sizeof(peer);
            const SRTSOCKET socket = srt_accept(listener, reinterpret_cast<sockaddr *>(&peer), &size);
            if (socket == SRT_INVALID_SOCK)
            {
                return;
            }
            accepted = socket;
            int timeout = 100;
            srt_setsockflag(socket, SRTO_RCVTIMEO, &timeout, sizeof(timeout));
            char buffer[1500];
            while (!stop)
            {
                SRT_MSGCTRL control = srt_msgctrl_default;
                static_cast<void>(srt_recvmsg2(socket, buffer, sizeof(buffer), &control));
            }
        });
        group = srt_create_group(type);
        for (const std::uint16_t weight : {20, 10})
        {
            endpoints.push_back(srt_prepare_endpoint(nullptr, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
            endpoints.back().weight = weight;
        }
        if (srt_connect_group(group, endpoints.data(), 2) == SRT_ERROR)
        {
            return false;
        }
        sender = std::thread([this] {
            char payload[1316];
            std::memset(payload, 0x47, sizeof(payload));
            auto next = std::chrono::steady_clock::now();
            while (!stop)
            {
                SRT_MSGCTRL control = srt_msgctrl_default;
                static_cast<void>(srt_sendmsg2(group, payload, sizeof(payload), &control));
                next += std::chrono::milliseconds(2);
                std::this_thread::sleep_until(next);
            }
        });
        return wait_for([this] { return accepted != SRT_INVALID_SOCK; }, 5000);
    }

    /* After embed_stop or detach: the host closes its sockets. */
    void finish()
    {
        stop = true;
        if (sender.joinable())
        {
            sender.join();
        }
        if (group != SRT_INVALID_SOCK)
        {
            srt_close(group);
        }
        if (listener != SRT_INVALID_SOCK)
        {
            srt_close(listener); /* ends a pending accept */
        }
        if (receiver.joinable())
        {
            receiver.join();
        }
        if (accepted != SRT_INVALID_SOCK)
        {
            srt_close(accepted);
        }
    }
};

robotweax_srt_telemetry_embed *start_embed(const std::uint32_t linger_seconds)
{
    robotweax_srt_telemetry_embed_config_v1 config;
    robotweax_srt_telemetry_embed_config_default(&config);
    robotweax_srt_telemetry_host_embed_functions(&config);
    config.flags = ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP;
    config.linger_seconds = linger_seconds;
    robotweax_srt_telemetry_embed *embed = nullptr;
    check(robotweax_srt_telemetry_embed_start(&config, &embed) == ROBOTWEAX_SRT_TELEMETRY_OK, "embed start");
    return embed;
}

/* Broadcast with host path names and no linger; stop while attached. */
void broadcast_scenario()
{
    GroupRun run;
    check(run.start(SRT_GTYPE_BROADCAST), "broadcast group connected");
    auto *embed = start_embed(0);
    robotweax_srt_telemetry_label_v1 labels[] = {{"stream", "bc"}};
    robotweax_srt_telemetry_embed_group_v1 g;
    robotweax_srt_telemetry_embed_group_default(&g);
    g.group = run.group;
    g.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    g.group_type = ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BROADCAST;
    g.labels = labels;
    g.label_count = 1;
    g.path_name = &name_by_weight;
    g.expected_paths = 3; /* one path never comes up */
    check(robotweax_srt_telemetry_embed_attach_group(embed, &g) == ROBOTWEAX_SRT_TELEMETRY_OK, "attach broadcast");

    std::vector<std::string> all;
    check(wait_for(
              [&] {
                  all = sources(embed);
                  return count_with(all, {"\"path\":\"lte\"", "\"group\":\"broadcast\""}) == 1 &&
                         count_with(all, {"\"path\":\"sat\"", "\"group\":\"broadcast\""}) == 1;
              },
              5000),
          "broadcast paths named by the host");
    Journal journal;
    check(wait_for(
              [&] {
                  journal.read(embed);
                  for (const auto &e : journal.events)
                  {
                      if (e.kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_ACTIVE_MEMBER_CHANGED && e.current_value == 2 &&
                          has(e.detail, "carrying=") && has(e.detail, "lte") && has(e.detail, "sat"))
                      {
                          return true;
                      }
                  }
                  return false;
              },
              5000),
          "broadcast: both paths carry (current_value 2, carrying=lte,sat)");
    const bool both_ok = wait_for(
        [&] {
            all = sources(embed);
            return count_with(all, {"\"stream\":\"bc\"", "\"path\":", "\"capped\":false", "\"verdict\":\"ok\""}) == 2;
        },
        8000);
    if (!both_ok)
    {
        for (const auto &source : all)
        {
            const auto h = source.find("\"health\":");
            std::cerr << "  " << source.substr(source.find("\"labels\":"), 90) << " "
                      << (h == std::string::npos ? "" : source.substr(h, 120)) << '\n';
        }
    }
    check(both_ok, "both broadcast paths OK while both are usable");
    check(wait_for(
              [&] {
                  const auto row = group_row(sources(embed), "bc");
                  return has(row, "\"verdict\":\"watch\"") && has(row, "\"reason\":\"path_lost\"") &&
                         has(row, "\"usable_paths\":2,\"expected_paths\":3");
              },
              6000),
          "broadcast on 2 of 3 expected paths: Watch / path_lost");

    /* Cut "lte": without a linger its row goes at once; "sat" is the last
     * usable path and is graded on its own. */
    srt_close(run.endpoints[0].id);
    check(wait_for(
              [&] {
                  all = sources(embed);
                  return count_with(all, {"\"path\":\"lte\""}) == 0;
              },
              5000),
          "no linger: the cut path's row is removed");
    journal.read(embed);
    check(journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_REMOVED, "path=lte") == 1, "member_removed for lte");
    check(has(find_source(all, {"\"path\":\"sat\""}), "\"capped\":false") &&
              has(find_source(all, {"\"path\":\"sat\""}), "\"group_member\":true"),
          "the last path is not capped and stays a group path");
    check(wait_for(
              [&] {
                  const auto row = group_row(sources(embed), "bc");
                  return has(row, "\"reason\":\"redundancy_lost\"") && has(row, "\"usable_paths\":1,\"expected_paths\":3");
              },
              6000),
          "broadcast on 1 of 3 expected paths: redundancy_lost");

    /* Stop while the group is still attached; then the host closes. */
    robotweax_srt_telemetry_embed_stop(embed);
    run.finish();
}

/* Host names on the accepting side, by the caller's weight (it reaches the
 * listener): "w20" and "w10". */
int name_by_weight_rx(void *, const robotweax_srt_telemetry_group_member_identity_v1 *member, char *name,
                      const size_t capacity)
{
    std::snprintf(name, capacity, "w%u", member->weight);
    return 1;
}

/* The accepting side with host names: a cut path that reconnects gets its row
 * back, also on a provider that keeps broken members listed. */
void listener_names_reconnect_scenario()
{
    GroupRun run;
    check(run.start(SRT_GTYPE_BACKUP), "backup group connected");
    auto *embed = start_embed(30);
    robotweax_srt_telemetry_label_v1 labels[] = {{"stream", "named-rx"}};
    robotweax_srt_telemetry_embed_group_v1 g;
    robotweax_srt_telemetry_embed_group_default(&g);
    g.group = run.accepted;
    g.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;
    g.group_type = ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP;
    g.labels = labels;
    g.label_count = 1;
    g.path_name = &name_by_weight_rx;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &g) == ROBOTWEAX_SRT_TELEMETRY_OK, "attach named listener");
    check(wait_for(
              [&] {
                  const auto all = sources(embed);
                  return count_with(all, {"\"path\":\"w20\""}) == 1 && count_with(all, {"\"path\":\"w10\""}) == 1;
              },
              5000),
          "listener paths named by the host");

    /* Cut the primary path on the caller and reconnect it (same weight). */
    srt_close(run.endpoints[0].id);
    sleep_ms(1500);
    sockaddr_in address{};
    int length = sizeof(address);
    srt_getsockname(run.listener, reinterpret_cast<sockaddr *>(&address), &length);
    std::vector<SRT_SOCKGROUPCONFIG> again;
    again.push_back(srt_prepare_endpoint(nullptr, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
    again.back().weight = 20;
    check(srt_connect_group(run.group, again.data(), 1) != SRT_ERROR, "reconnect the path");
    Journal journal;
    check(wait_for(
              [&] {
                  journal.read(embed);
                  const auto w20 = find_source(sources(embed), {"\"path\":\"w20\""});
                  return journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_ADDED, "path=w20") == 2 &&
                         !has(w20, "\"reason\":\"connection_lost\"");
              },
              8000),
          "the reconnected path gets its row back (member_added, no longer lost)");
    check(count_with(sources(embed), {"\"stream\":\"named-rx\"", "\"path\":"}) == 2,
          "still two path rows: the reconnect continued w20");
    robotweax_srt_telemetry_embed_stop(embed);
    run.finish();
}

/* Duplicate host names: one row, and no flood of member_added. */
void duplicate_names_scenario()
{
    GroupRun run;
    check(run.start(SRT_GTYPE_BROADCAST), "duplicate-name group connected");
    auto *embed = start_embed(30);
    int marker = 1;
    robotweax_srt_telemetry_embed_group_v1 g;
    robotweax_srt_telemetry_embed_group_default(&g);
    g.group = run.group;
    g.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    g.group_type = ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BROADCAST;
    g.path_name = &name_by_weight;
    g.path_name_user_data = &marker;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &g) == ROBOTWEAX_SRT_TELEMETRY_OK, "attach duplicate");
    sleep_ms(3000);
    const auto all = sources(embed);
    Journal journal;
    journal.read(embed);
    check(count_with(all, {"\"path\":\"same\""}) == 1, "duplicate names share one row");
    check(journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_ADDED) == 1,
          "duplicate names: one member_added, no flood (" +
              std::to_string(journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_ADDED)) + ")");
    check(robotweax_srt_telemetry_embed_detach(embed, run.group) == ROBOTWEAX_SRT_TELEMETRY_OK, "detach");
    robotweax_srt_telemetry_embed_stop(embed);
    run.finish();
}

/* Review C1 #3: a replacement on the same path while the old member still
 * works is one path: no inflated expected count, no lasting Watch. */
void overlap_scenario()
{
    GroupRun run;
    check(run.start(SRT_GTYPE_BACKUP), "overlap group connected");
    auto *embed = start_embed(30);
    robotweax_srt_telemetry_label_v1 labels[] = {{"stream", "ov"}};
    robotweax_srt_telemetry_embed_group_v1 g;
    robotweax_srt_telemetry_embed_group_default(&g);
    g.group = run.group;
    g.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    g.group_type = ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP;
    g.labels = labels;
    g.label_count = 1;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &g) == ROBOTWEAX_SRT_TELEMETRY_OK, "attach overlap group");
    check(wait_for([&] { return has(group_row(sources(embed), "ov"), "\"usable_paths\":2,\"expected_paths\":2"); },
                   5000),
          "two paths");

    /* A third member with the primary's token: same path, overlapping. */
    sockaddr_in address{};
    int length = sizeof(address);
    srt_getsockname(run.listener, reinterpret_cast<sockaddr *>(&address), &length);
    std::vector<SRT_SOCKGROUPCONFIG> again;
    again.push_back(srt_prepare_endpoint(nullptr, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
    again.back().weight = 20;
    again.back().token = run.endpoints[0].token;
    check(srt_connect_group(run.group, again.data(), 1) != SRT_ERROR, "overlapping replacement connects");
    sleep_ms(2500);
    const auto row = group_row(sources(embed), "ov");
    check(has(row, "\"usable_paths\":2,\"expected_paths\":2"), "the overlap counts as two paths, not three");

    /* The old member leaves: still two paths of two, no lasting Watch. */
    srt_close(run.endpoints[0].id);
    check(wait_for(
              [&] {
                  const auto r = group_row(sources(embed), "ov");
                  return has(r, "\"usable_paths\":2,\"expected_paths\":2") && !has(r, "path_lost");
              },
              8000),
          "after the old member leaves: 2 of 2 paths, no path_lost");
    robotweax_srt_telemetry_embed_stop(embed);
    run.finish();
}

/* group and path belong to the embed; a plain socket never continues a
 * group's row. */
void reserved_labels_scenario()
{
    robotweax_srt_telemetry_embed_config_v1 config;
    robotweax_srt_telemetry_embed_config_default(&config);
    robotweax_srt_telemetry_host_embed_functions(&config);
    config.flags = ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP;
    robotweax_srt_telemetry_label_v1 group_key[] = {{"group", "x"}};
    config.labels = group_key;
    config.label_count = 1;
    robotweax_srt_telemetry_embed *embed = nullptr;
    check(robotweax_srt_telemetry_embed_start(&config, &embed) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT &&
              embed == nullptr,
          "start rejects the group label");
    embed = start_embed(30);
    const SRTSOCKET socket = srt_create_socket();
    robotweax_srt_telemetry_label_v1 path_key[] = {{"path", "x"}};
    check(robotweax_srt_telemetry_embed_attach(embed, socket, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER, path_key,
                                               1) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "attach rejects the path label");
    robotweax_srt_telemetry_embed_group_v1 big;
    robotweax_srt_telemetry_embed_group_default(&big);
    big.group = socket;
    big.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    big.group_type = ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP;
    big.expected_paths = 70000;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &big) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "expected_paths above 65535 rejected");
    srt_close(socket);
    robotweax_srt_telemetry_embed_stop(embed);
}

int no_path_data(std::int32_t, struct ROBOTWEAX_SRT_GroupPathDataV1_ *, std::size_t *)
{
    return -1;
}

/* Negotiated path labels are a Robotweax SRT extension: the getter is refused
 * with another provider. */
void path_data_provider_scenario()
{
    robotweax_srt_telemetry_embed_config_v1 config;
    robotweax_srt_telemetry_embed_config_default(&config);
    robotweax_srt_telemetry_host_embed_functions(&config);
    config.flags = ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP;
    config.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
    config.group_path_data = &no_path_data;
    robotweax_srt_telemetry_embed *embed = nullptr;
    check(robotweax_srt_telemetry_embed_start(&config, &embed) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT &&
              embed == nullptr,
          "start rejects a path-label getter for Haivision SRT");
}

#if defined(ROBOTWEAX_SRT_PATHID_NEGOTIATED)
/* Robotweax SRT with negotiated labels: both sides name the paths from the
 * caller's labels, so the receiver's rows match the sender's without host
 * code. */
void negotiated_labels_scenario()
{
    const SRTSOCKET listener = srt_create_socket();
    int yes = 1;
    srt_setsockflag(listener, SRTO_GROUPCONNECT, &yes, sizeof(yes));
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    check(srt_setsockflag(group, SRTO_ROBOTWEAX_PATHID_REQUIRED, &yes, sizeof(yes)) == 0 &&
              srt_setsockflag(listener, SRTO_ROBOTWEAX_PATHID_REQUIRED, &yes, sizeof(yes)) == 0,
          "labels: both peers require path labels");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    int length = sizeof(address);
    check(srt_bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
              srt_listen(listener, 4) == 0 &&
              srt_getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0,
          "labels: listen");
    std::atomic<SRTSOCKET> accepted{SRT_INVALID_SOCK};
    std::atomic<bool> stop{false};
    std::thread receiver([&] {
        sockaddr_storage peer{};
        int size = sizeof(peer);
        const SRTSOCKET socket = srt_accept(listener, reinterpret_cast<sockaddr *>(&peer), &size);
        if (socket == SRT_INVALID_SOCK)
        {
            return;
        }
        accepted = socket;
        int timeout = 100;
        srt_setsockflag(socket, SRTO_RCVTIMEO, &timeout, sizeof(timeout));
        char buffer[1500];
        while (!stop)
        {
            SRT_MSGCTRL control = srt_msgctrl_default;
            static_cast<void>(srt_recvmsg2(socket, buffer, sizeof(buffer), &control));
        }
    });
    std::vector<SRT_SOCKGROUPCONFIG> endpoints;
    for (const char *label : {"lte", "sat"})
    {
        endpoints.push_back(srt_prepare_endpoint(nullptr, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
        endpoints.back().config = srt_create_config();
        check(srt_config_add(endpoints.back().config, SRTO_ROBOTWEAX_PATHID, label,
                             static_cast<int>(std::strlen(label))) == 0,
              std::string("labels: endpoint label ") + label);
    }
    const bool connected = srt_connect_group(group, endpoints.data(), 2) != SRT_ERROR;
    for (auto &endpoint : endpoints)
    {
        srt_delete_config(endpoint.config);
    }
    check(connected, "labels: connect group");
    std::thread sender([&] {
        char payload[1316];
        std::memset(payload, 0x47, sizeof(payload));
        auto next = std::chrono::steady_clock::now();
        while (!stop)
        {
            SRT_MSGCTRL control = srt_msgctrl_default;
            static_cast<void>(srt_sendmsg2(group, payload, sizeof(payload), &control));
            next += std::chrono::milliseconds(2);
            std::this_thread::sleep_until(next);
        }
    });
    check(wait_for([&] { return accepted != SRT_INVALID_SOCK; }, 5000), "labels: accepted");

    robotweax_srt_telemetry_embed_config_v1 config;
    robotweax_srt_telemetry_embed_config_default(&config);
    robotweax_srt_telemetry_host_embed_functions(&config);
    robotweax_srt_telemetry_host_embed_path_data(&config);
    config.flags = ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP;
    config.linger_seconds = 0;
    robotweax_srt_telemetry_embed *embed = nullptr;
    check(robotweax_srt_telemetry_embed_start(&config, &embed) == ROBOTWEAX_SRT_TELEMETRY_OK, "labels: embed start");
    robotweax_srt_telemetry_label_v1 labels[] = {{"stream", "labeled"}};
    robotweax_srt_telemetry_embed_group_v1 g;
    robotweax_srt_telemetry_embed_group_default(&g);
    g.group = group;
    g.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    g.group_type = ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BROADCAST;
    g.labels = labels;
    g.label_count = 1;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &g) == ROBOTWEAX_SRT_TELEMETRY_OK, "labels: attach sender");
    g.group = accepted;
    g.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &g) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "labels: attach receiver");
    /* "lte" and "sat" as lowercase hexadecimal. */
    std::vector<std::string> all;
    const bool named = wait_for(
        [&] {
            all = sources(embed);
            int found = 0;
            for (const char *role : {"\"role\":\"sender\"", "\"role\":\"receiver\""})
            {
                for (const char *path : {"\"path\":\"pathid-6c7465\"", "\"path\":\"pathid-736174\""})
                {
                    found += count_with(all, {"\"stream\":\"labeled\"", role, path});
                }
            }
            return found == 4;
        },
        5000);
    check(named, "labels: both sides name the paths from the caller's labels");
    check(count_with(all, {"\"stream\":\"labeled\"", "\"path\":"}) == 4,
          "labels: no other path rows (no address names on the receiver)");
    robotweax_srt_telemetry_embed_stop(embed);
    stop = true;
    sender.join();
    srt_close(group);
    srt_close(listener);
    receiver.join();
    if (accepted != SRT_INVALID_SOCK)
    {
        srt_close(accepted);
    }
}
#endif

} // namespace

int main(int argc, char **argv)
{
    const bool require = argc > 1 && std::string_view(argv[1]) == "--require";
    srt_startup();
    const SRTSOCKET probe = srt_create_group(SRT_GTYPE_BACKUP);
    if (probe == SRT_INVALID_SOCK)
    {
        std::cout << "SKIP: no connection groups in this SRT library\n";
        srt_cleanup();
        return require ? 1 : 77;
    }
    srt_close(probe);
    backup_scenario();
    broadcast_scenario();
    duplicate_names_scenario();
    listener_names_reconnect_scenario();
    overlap_scenario();
    reserved_labels_scenario();
    path_data_provider_scenario();
#if defined(ROBOTWEAX_SRT_PATHID_NEGOTIATED)
    negotiated_labels_scenario();
#endif
    srt_cleanup();
    if (failures != 0)
    {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "embedded group tests passed\n";
    return 0;
}
