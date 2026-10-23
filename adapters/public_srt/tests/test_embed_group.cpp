// Embedded connection groups, live: a Main/Backup group with two members over
// loopback, attached on both sides with embed_attach_group. Checks the
// group and member sources (labels group and path), the group events in the
// journal, the member verdict cap after a path is cut, the continuation of
// the path's row when a member reconnects on it, and detach; and a Main/Backup
// sender whose primary path goes dark while both members stay connected.
//
// Exit code 77 (skipped) without connection groups, unless --require.
#include <robotweax/srt_telemetry_public_srt_host.h>

#include "impairing_relay.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

/* The member entry grew by path_label (srt_telemetry#57); a host passes
 * sizeof as member_entry_size, so a mismatch is reported, never misread. */
static_assert(offsetof(robotweax_srt_telemetry_group_member_v1, path_label) + ROBOTWEAX_SRT_TELEMETRY_GROUP_PATH_LABEL_CAPACITY <=
                  sizeof(robotweax_srt_telemetry_group_member_v1),
              "path_label ends the member entry");
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(robotweax_srt_telemetry_group_member_v1) ==
                  offsetof(robotweax_srt_telemetry_group_member_v1, path_label) + 72,
              "member entry layout (64-bit)");
#endif

int failures = 0;

void check(const bool condition, const std::string_view message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

/* Synthetic display fixture on real group-member discovery. Not RTT qualification. */
struct StandbyFixture
{
    std::uint32_t size = 48, version = 1, flags = 15, outcome = 2;
    std::uint64_t rtt = 25'000, age = 42, samples = 5;
    std::uint32_t reserved[2] = {};
};
std::atomic<int> standby_fixture_result{0};
int fixture_standby_getter(std::int32_t socket, ROBOTWEAX_SRT_StandbyRttDataV1_ *out,
                           std::size_t size)
{
    check(size == sizeof(StandbyFixture), "embed standby exact capacity");
    check((socket & SRTGROUP_MASK) == 0, "group aggregate never sent to standby getter");
    const StandbyFixture wire;
    std::memcpy(out, &wire, sizeof(wire));
    return standby_fixture_result;
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

/* A recording write syncs the file and its directory: on a loaded disk one
 * fsync took over 10 s (srt_telemetry#43). Recordings go to tmpfs where
 * there is one, and writes get a long deadline. */
constexpr int write_timeout_ms = 30000;

std::filesystem::path recording_root()
{
    std::error_code error;
    if (std::filesystem::is_directory("/dev/shm", error))
    {
        return "/dev/shm";
    }
    return std::filesystem::temp_directory_path();
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

/* Recording events of the group scenario, written on the embed's writer. */
struct GroupRecordingEvents
{
    std::mutex mutex;
    std::vector<robotweax_srt_telemetry_embed_recording_event_v1> events;
    static void callback(void *user, const robotweax_srt_telemetry_embed_recording_event_v1 *event)
    {
        auto &self = *static_cast<GroupRecordingEvents *>(user);
        std::lock_guard lock(self.mutex);
        self.events.push_back(*event);
        self.events.back().path = nullptr;
    }
    std::vector<robotweax_srt_telemetry_embed_recording_event_v1> copy()
    {
        std::lock_guard lock(mutex);
        return events;
    }
};

/* Main/Backup on both sides: rows, events, cut, reconnect, detach. */
void backup_scenario()
{
    const auto recordings = recording_root() / ("srt-telemetry-group-recordings-" + std::to_string(::getpid()));
    std::filesystem::remove_all(recordings);
    std::filesystem::create_directories(recordings);
    GroupRecordingEvents recording_events;
    const SRTSOCKET caller_group = srt_create_group(SRT_GTYPE_BACKUP);

    robotweax_srt_telemetry_embed_config_v1 config;
    robotweax_srt_telemetry_embed_config_default(&config);
    robotweax_srt_telemetry_host_embed_functions(&config);
    config.flags = ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP;
    config.linger_seconds = 30;
    std::snprintf(config.recording_directory, sizeof(config.recording_directory), "%s", recordings.c_str());
    robotweax_srt_telemetry_embed *embed = nullptr;
    check(robotweax_srt_telemetry_embed_start(&config, &embed) == ROBOTWEAX_SRT_TELEMETRY_OK, "embed start");
    check(robotweax_srt_telemetry_embed_set_recording_callback(embed, GroupRecordingEvents::callback,
                                                               &recording_events) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "group recording callback");
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
    /* Grades settle over a few windows; a starved tick delays them. */
    static_cast<void>(wait_for(
        [&]
        {
            all = sources(embed);
            return count_with(all, {"\"path\":", "\"verdict\":\"ok\"", "\"reason\":\"standby\""}) == 2 &&
                   count_with(all, {"\"path\":", "\"reason\":\"standby\"", "\"standby\":true"}) == 2 &&
                   count_with(all, {"\"path\":", "\"verdict\":\"unknown\""}) == 0;
        },
        6000));
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
    /* The receiving group's grade settles over 2 s of its paths' data: wait
     * for it rather than reading one snapshot (srt_telemetry#43). */
    {
        const bool ok = wait_for([&] { return has(group_row(sources(embed), "cam-rx"), "\"verdict\":\"ok\""); }, 15000);
        const auto row = group_row(sources(embed), "cam-rx");
        const auto health = row.find("\"health\"");
        /* The only other grades allowed are what a starved receiver really
         * measures (loss, a late reading application, RTT inflated past the
         * latency), still graded through both paths, never unknown. */
        const bool starved = (has(row, "\"reason\":\"unrecovered_loss\"") ||
                              has(row, "\"reason\":\"application_read_stall\"") ||
                              has(row, "\"reason\":\"latency_low_for_rtt\"")) &&
                             has(row, "\"group\":{\"usable_paths\":2,\"expected_paths\":2}");
        check(ok || starved,
              "the receiving group is graded OK: " + (health == std::string::npos ? row : row.substr(health, 160)));
    }

    /* A live write of a group writes the group's and each member's
     * recording; nothing is final while they go on. */
    check(robotweax_srt_telemetry_embed_write_recording(embed, caller_group) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "live write of the caller group");
    {
        const bool reported = wait_for([&] { return recording_events.copy().size() >= 3; }, write_timeout_ms);
        std::string seen;
        for (const auto &event : recording_events.copy())
        {
            seen += " " + std::to_string(event.source_id) + ":" + std::to_string(event.flags) + "/" +
                    std::to_string(event.record_count);
        }
        check(reported, "group live write reported:" + seen);
    }
    {
        const auto events = recording_events.copy();
        std::set<std::uint64_t> sources;
        bool all_written = true;
        for (const auto &event : events)
        {
            sources.insert(event.source_id);
            all_written = all_written && event.flags == ROBOTWEAX_SRT_TELEMETRY_EMBED_RECORDING_WRITTEN &&
                          event.record_count > 0;
        }
        std::string seen;
        for (const auto &event : events)
        {
            seen += " " + std::to_string(event.source_id) + ":" + std::to_string(event.flags) + "/" +
                    std::to_string(event.record_count);
        }
        check(events.size() == 3 && sources.size() == 3 && all_written,
              "group and both members written live, none final:" + seen);
    }

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
    /* The inferred data path follows per-tick packet deltas; a starved tick
     * can name the backup first, so wait until the primary has been named
     * (srt_telemetry#43). */
    const bool primary_named = wait_for(
        [&]
        {
            journal.read(embed);
            return journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_ACTIVE_MEMBER_CHANGED, "-> " + primary_path) >= 1;
        },
        6000);
    {
        std::string changes;
        for (const auto &e : journal.events)
        {
            if (e.kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_ACTIVE_MEMBER_CHANGED)
                changes += std::string(" [") + e.detail + "]";
        }
        check(primary_named, "caller: the primary carries the most observed packets:" + changes);
    }
    check(journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_ADDED) == 4, "member_added for every member");
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

    {
        /* A starved tick can fail one statistics read of a group while SRT
         * is busy with it; each failure must be followed by a recovery
         * (journaled after a second without failures). */
        std::string failed;
        const auto all_recovered = [&]
        {
            journal.read(embed);
            std::map<std::uint64_t, int> open;
            failed.clear();
            for (const auto &e : journal.events)
            {
                if (e.kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_FAILED)
                {
                    ++open[e.source_id];
                    failed += " [source " + std::to_string(e.source_id) + " code " + std::to_string(e.code) +
                              ": " + e.detail + "]";
                }
                else if (e.kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_RECOVERED && open[e.source_id] > 0)
                {
                    --open[e.source_id];
                }
            }
            for (const auto &[source, count] : open)
            {
                if (count != 0)
                    return false;
            }
            return true;
        };
        const bool recovered = wait_for(all_recovered, 5000);
        check(recovered, "no statistics read stays failed:" + failed);
    }

#if defined(ROBOTWEAX_SRT_STANDBY_RTT_VERSION) && ROBOTWEAX_SRT_STANDBY_RTT_VERSION == 1
    check(robotweax_srt_telemetry_host_embed_standby_rtt_data(embed) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "explicit native standby getter opt-in");
    check(wait_for(
              [&]
              {
                  return count_with(sources(embed),
                                    {"\"path\":", "\"standby_rtt\":{\"version\":1,\"availability\":"
                                                  "\"available\",\"negotiated\":false"}) == 4;
              },
              2000),
          "native disabled connections read available without invented observation");
#endif
    check(robotweax_srt_telemetry_embed_set_standby_rtt_data_v1(embed, fixture_standby_getter) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "inject synthetic standby display fixture");
    check(wait_for(
              [&]
              {
                  return count_with(sources(embed), {"\"path\":", "\"last_outcome\":\"measured\"",
                                                     "\"samples\":\"5\""}) == 4;
              },
              2000),
          "all group members carry independent standby observations");
    check(wait_for(
              [&] {
                  return count_with(sources(embed), {"\"path\":", "\"reason\":\"standby\"", "\"verdict\":\"ok\""}) == 2;
              },
              6000),
          "standby observation does not change DATA-health grade");
    check(has(group_row(sources(embed), "cam-tx"),
              "\"standby_rtt\":{\"version\":1,\"availability\":\"unavailable\"}"),
          "group aggregate has no invented RTT observation");
    standby_fixture_result = -1;
    check(
        wait_for(
            [&]
            {
                return count_with(
                           sources(embed),
                           {"\"path\":",
                            "\"standby_rtt\":{\"version\":1,\"availability\":\"read_failed\"}"}) ==
                       4;
            },
            2000),
        "failed member getters remove old observation");
    standby_fixture_result = 0;
    check(robotweax_srt_telemetry_embed_set_standby_rtt_data_v1(embed, nullptr) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "disable standby getter");
    check(count_with(sources(embed),
                     {"\"standby_rtt\":{\"version\":1,\"availability\":\"unavailable\"}"}) == 6,
          "disable immediately clears all member and aggregate snapshots");

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
    std::filesystem::remove_all(recordings);
}

/* Recording schema 1alpha2 on a live Backup group: group context in every
 * header, member state per sample, health transitions. */
void recording_1alpha2_scenario()
{
    const auto directory = recording_root() / ("srt-telemetry-group-1alpha2-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    robotweax_srt_telemetry_embed_config_v1 config;
    robotweax_srt_telemetry_embed_config_default(&config);
    robotweax_srt_telemetry_host_embed_functions(&config);
    config.flags = ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP;
    const auto spec = "off,jsonl=" + directory.string() + ",jsonl_schema=1alpha2";
    check(robotweax_srt_telemetry_embed_config_parse(&config, spec.c_str()) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              config.recording_schema == ROBOTWEAX_SRT_TELEMETRY_EMBED_RECORDING_1ALPHA2,
          "jsonl_schema=1alpha2 parses");
    auto refused = config;
    check(robotweax_srt_telemetry_embed_config_parse(&refused, "off,jsonl=/tmp,jsonl_schema=2") ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "an unknown recording schema is refused");
    robotweax_srt_telemetry_embed *embed = nullptr;
    check(robotweax_srt_telemetry_embed_start(&config, &embed) == ROBOTWEAX_SRT_TELEMETRY_OK, "1alpha2 embed start");
    if (embed == nullptr)
    {
        return;
    }
#if defined(ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION) && ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION == 1
    static_cast<void>(robotweax_srt_telemetry_host_embed_group_send_state(embed));
#endif
    GroupRecordingEvents events;
    check(robotweax_srt_telemetry_embed_set_recording_callback(embed, GroupRecordingEvents::callback, &events) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "1alpha2 recording callback");

    const SRTSOCKET caller = srt_create_group(SRT_GTYPE_BACKUP);
    const SRTSOCKET listener = srt_create_socket();
    int yes = 1;
    srt_setsockflag(listener, SRTO_GROUPCONNECT, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    int length = sizeof(address);
    check(srt_bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
              srt_listen(listener, 4) == 0 &&
              srt_getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0,
          "1alpha2 listen");
    std::atomic<bool> stop{false};
    std::thread receiver([&] {
        const SRTSOCKET socket = srt_accept(listener, nullptr, nullptr);
        if (socket == SRT_INVALID_SOCK)
            return;
        int timeout = 100;
        srt_setsockflag(socket, SRTO_RCVTIMEO, &timeout, sizeof(timeout));
        char buffer[1500];
        while (!stop)
        {
            SRT_MSGCTRL control = srt_msgctrl_default;
            static_cast<void>(srt_recvmsg2(socket, buffer, sizeof(buffer), &control));
        }
        srt_close(socket);
    });
    std::vector<SRT_SOCKGROUPCONFIG> endpoints;
    for (const std::uint16_t weight : {20, 10})
    {
        endpoints.push_back(srt_prepare_endpoint(nullptr, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
        endpoints.back().weight = weight;
    }
    check(srt_connect_group(caller, endpoints.data(), 2) != SRT_ERROR, "1alpha2 connect group");
    std::thread sender([&] {
        char payload[1316] = {};
        while (!stop)
        {
            SRT_MSGCTRL control = srt_msgctrl_default;
            static_cast<void>(srt_sendmsg2(caller, payload, sizeof(payload), &control));
            sleep_ms(2);
        }
    });
    robotweax_srt_telemetry_label_v1 labels[] = {{"stream", "cam-v2"}};
    robotweax_srt_telemetry_embed_group_v1 tx;
    robotweax_srt_telemetry_embed_group_default(&tx);
    tx.group = caller;
    tx.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    tx.group_type = ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP;
    tx.labels = labels;
    tx.label_count = 1;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &tx) == ROBOTWEAX_SRT_TELEMETRY_OK, "1alpha2 attach");
    /* Write once both paths are connected and graded, one of them standby
     * (a fixed wait is too short on a loaded machine, srt_telemetry#43). */
    const bool settled = wait_for(
        [&]
        {
            const auto all = sources(embed);
            return has(group_row(all, "cam-v2"), "\"group\":{\"usable_paths\":2,\"expected_paths\":2}") &&
                   count_with(all, {"\"stream\":\"cam-v2\"", "\"path\":", "\"verdict\":\"ok\""}) == 2 &&
                   count_with(all, {"\"stream\":\"cam-v2\"", "\"path\":", "\"standby\":true"}) == 1;
        },
        15000);
    check(settled, "1alpha2: both paths connected and graded, one standby");
    sleep_ms(1000); /* a few samples in that state */
    const auto write = robotweax_srt_telemetry_embed_write_recording(embed, caller);
    check(write == ROBOTWEAX_SRT_TELEMETRY_OK, "1alpha2 live write accepted: " + std::to_string(write));
    /* The message is built after the wait: arguments of one call are
     * evaluated in no fixed order. */
    const bool written = wait_for([&] { return events.copy().size() >= 3; }, write_timeout_ms);
    check(written, "1alpha2 group and members written live: " + std::to_string(events.copy().size()) + " recordings");

    std::string group_file;
    std::vector<std::string> member_files;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
    {
        std::ifstream in(entry.path());
        std::string header;
        std::getline(in, header);
        if (has(header, "\"groupRow\":\"group\""))
            group_file = entry.path().string();
        else if (has(header, "\"groupRow\":\"member\""))
            member_files.push_back(entry.path().string());
        check(has(header, "\"schemaVersion\":\"1alpha2\"") && has(header, "\"groupType\":\"backup\"") &&
                  has(header, "\"healthLines\":true"),
              "1alpha2 header: " + header);
    }
    check(!group_file.empty() && member_files.size() == 2, "one group row and two member rows written");
    int standby_samples = 0;
    int running_samples = 0;
    int health_lines = 0;
    bool active_seen = false;
    for (const auto &file : member_files)
    {
        std::ifstream in(file);
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line))
        {
            if (has(line, "robotweax-srt-telemetry-health"))
            {
                ++health_lines;
                check(has(line, "\"usablePaths\":null,\"expectedPaths\":null"), "member health lines carry no paths");
                continue;
            }
            check(has(line, ",\"member\":{\"state\":"), "every member sample has its state: " + line.substr(0, 80));
            standby_samples += has(line, "\"standby\":true") ? 1 : 0;
            running_samples += has(line, "\"state\":\"running\"") ? 1 : 0;
            active_seen = active_seen || has(line, "\"authority\":\"active\"");
        }
    }
    check(standby_samples > 0 && running_samples > 0, "a standby and a running member: " +
                                                          std::to_string(standby_samples) + " standby, " +
                                                          std::to_string(running_samples) + " running samples");
    check(health_lines >= 2, "member health transitions recorded: " + std::to_string(health_lines));
#if defined(ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION) && ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION == 1
    check(active_seen, "the sending member carries sender authority");
#endif
    {
        std::ifstream in(group_file);
        std::string line;
        bool paths = false;
        std::getline(in, line);
        while (std::getline(in, line))
        {
            if (has(line, "robotweax-srt-telemetry-health"))
                paths = paths || has(line, "\"expectedPaths\":2");
            else
                check(has(line, ",\"member\":null}"), "group samples carry no member state");
        }
        check(paths, "group health lines carry the path counts");
    }
    stop = true;
    sender.join();
    srt_close(caller);
    receiver.join();
    srt_close(listener);
    robotweax_srt_telemetry_embed_stop(embed);
    if (const char *keep = std::getenv("SRT_TELEMETRY_KEEP_1ALPHA2"); keep != nullptr)
    {
        std::filesystem::copy(directory, keep, std::filesystem::copy_options::recursive |
                                                   std::filesystem::copy_options::overwrite_existing);
    }
    std::filesystem::remove_all(directory);
}

/* Recording schema 1alpha3 on a live group pair (srt_telemetry#57): both
 * ends' rows carry the session reference and their group IDs; the accepting
 * side notes the caller's proposed group ID at admission (Robotweax SRT), and
 * with negotiated labels both ends' member rows carry the same label, so
 * SRT Doctor can pair group with group and path with path. */
int note_peer_group(void *embed, SRTSOCKET socket, int, const sockaddr *, const char *)
{
#if defined(ROBOTWEAX_SRT_COMPAT_SRT_H) || defined(ROBOTWEAX_SRT_FLAT_COMPAT_HEADER_H)
    static_cast<void>(
        robotweax_srt_telemetry_host_embed_note_peer_group(static_cast<robotweax_srt_telemetry_embed *>(embed), socket));
    const std::uint64_t domain = 1;
    srt_setsockflag(socket, SRTO_ROBOTWEAX_GROUPDOMAIN, &domain, sizeof(domain));
#else
    static_cast<void>(embed);
    static_cast<void>(socket);
#endif
    return 0;
}

std::string header_field(const std::string &line, const std::string &key)
{
    const auto at = line.find("\"" + key + "\":");
    if (at == std::string::npos)
        return "<absent>";
    auto start = at + key.size() + 3;
    if (line[start] == '"')
        return line.substr(start + 1, line.find('"', start + 1) - start - 1);
    return line.substr(start, line.find_first_of(",}", start) - start);
}

void recording_1alpha3_scenario()
{
    const bool robotweax = ROBOTWEAX_SRT_TELEMETRY_HOST_PROVIDER == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    bool labels = false;
    const auto directory = recording_root() / ("srt-telemetry-group-1alpha3-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    robotweax_srt_telemetry_embed_config_v1 config;
    robotweax_srt_telemetry_embed_config_default(&config);
    robotweax_srt_telemetry_host_embed_functions(&config);
#if defined(ROBOTWEAX_SRT_PATHID_NEGOTIATED)
    robotweax_srt_telemetry_host_embed_path_data(&config);
    labels = true;
#endif
    config.flags = ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP;
    const auto spec = "off,jsonl=" + directory.string() + ",jsonl_schema=1alpha3,linger_s=0";
    check(robotweax_srt_telemetry_embed_config_parse(&config, spec.c_str()) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "1alpha3 group spec");
    robotweax_srt_telemetry_embed *embed = nullptr;
    check(robotweax_srt_telemetry_embed_start(&config, &embed) == ROBOTWEAX_SRT_TELEMETRY_OK, "1alpha3 group start");
    if (embed == nullptr)
        return;
    GroupRecordingEvents events;
    check(robotweax_srt_telemetry_embed_set_recording_callback(embed, GroupRecordingEvents::callback, &events) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "1alpha3 group recording callback");

    const SRTSOCKET listener = srt_create_socket();
    const SRTSOCKET caller = srt_create_group(SRT_GTYPE_BACKUP);
    int yes = 1;
    srt_setsockflag(listener, SRTO_GROUPCONNECT, &yes, sizeof(yes));
#if defined(ROBOTWEAX_SRT_PATHID_NEGOTIATED)
    check(srt_setsockflag(listener, SRTO_ROBOTWEAX_PATHID_REQUIRED, &yes, sizeof(yes)) == 0 &&
              srt_setsockflag(caller, SRTO_ROBOTWEAX_PATHID_REQUIRED, &yes, sizeof(yes)) == 0,
          "1alpha3: both peers require path labels");
#endif
    if (robotweax)
        check(srt_listen_callback(listener, note_peer_group, embed) == 0, "1alpha3: listen callback");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    int length = sizeof(address);
    check(srt_bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
              srt_listen(listener, 4) == 0 &&
              srt_getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0,
          "1alpha3: listen");
    std::atomic<SRTSOCKET> accepted{SRT_INVALID_SOCK};
    std::atomic<bool> stop{false};
    std::thread receiver([&] {
        const SRTSOCKET socket = srt_accept(listener, nullptr, nullptr);
        if (socket == SRT_INVALID_SOCK)
            return;
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
        endpoints.back().weight = std::string_view(label) == "lte" ? 20 : 10;
#if defined(ROBOTWEAX_SRT_PATHID_NEGOTIATED)
        endpoints.back().config = srt_create_config();
        srt_config_add(endpoints.back().config, SRTO_ROBOTWEAX_PATHID, label, static_cast<int>(std::strlen(label)));
#else
        static_cast<void>(label);
#endif
    }
    const bool connected = srt_connect_group(caller, endpoints.data(), 2) != SRT_ERROR;
    for (auto &endpoint : endpoints)
        if (endpoint.config != nullptr)
            srt_delete_config(endpoint.config);
    check(connected, "1alpha3: connect group");
    std::thread sender([&] {
        char payload[1316] = {};
        while (!stop)
        {
            SRT_MSGCTRL control = srt_msgctrl_default;
            static_cast<void>(srt_sendmsg2(caller, payload, sizeof(payload), &control));
            sleep_ms(2);
        }
    });
    check(wait_for([&] { return accepted != SRT_INVALID_SOCK; }, 5000), "1alpha3: accepted");
    robotweax_srt_telemetry_label_v1 stream_label[] = {{"stream", "cam-v3"}};
    robotweax_srt_telemetry_embed_group_v1 g;
    robotweax_srt_telemetry_embed_group_default(&g);
    g.group = caller;
    g.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    g.group_type = ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP;
    g.labels = stream_label;
    g.label_count = 1;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &g) == ROBOTWEAX_SRT_TELEMETRY_OK, "1alpha3: attach sender");
    g.group = accepted;
    g.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &g) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "1alpha3: attach receiver");
    /* On the group handle: its members follow it. */
    check(robotweax_srt_telemetry_embed_set_session_ref(embed, caller, "pair-1") == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_embed_set_session_ref(embed, accepted, "pair-1") == ROBOTWEAX_SRT_TELEMETRY_OK,
          "1alpha3: session reference on both groups");
    const bool settled = wait_for(
        [&]
        {
            const auto all = sources(embed);
            return count_with(all, {"\"stream\":\"cam-v3\"", "\"path\":", "\"connection\":\"connected\""}) == 4;
        },
        15000);
    check(settled, "1alpha3: four member rows connected");
    sleep_ms(1000);
    check(robotweax_srt_telemetry_embed_write_recording(embed, caller) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_embed_write_recording(embed, accepted) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "1alpha3: live writes");
    check(wait_for([&] { return events.copy().size() >= 6; }, write_timeout_ms),
          "1alpha3: two groups and four members written: " + std::to_string(events.copy().size()));

    std::map<std::string, std::vector<std::string>> rows; /* "<role>/<groupRow>" -> headers */
    for (const auto &entry : std::filesystem::directory_iterator(directory))
    {
        std::ifstream in(entry.path());
        std::string header;
        std::getline(in, header);
        rows[header_field(header, "role") + "/" + header_field(header, "groupRow")].push_back(header);
        check(header_field(header, "schemaVersion") == "1alpha3" && header_field(header, "sessionRef") == "pair-1" &&
                  header_field(header, "connectedAtUnixNano") != "null",
              "1alpha3 group header: " + header);
        bool aged = false;
        for (std::string line; std::getline(in, line);)
            aged = aged || (has(line, "robotweax-srt-telemetry-sample") && !has(line, "\"connectionAgeNano\":null"));
        check(aged, "1alpha3 samples carry connection ages: " + entry.path().filename().string());
    }
    check(rows["sender/group"].size() == 1 && rows["receiver/group"].size() == 1 && rows["sender/member"].size() == 2 &&
              rows["receiver/member"].size() == 2,
          "1alpha3: one group row and two member rows per end");
    const auto caller_id = std::to_string(caller);
    const auto accepted_id = std::to_string(accepted.load());
    for (const auto &[kind, headers] : rows)
    {
        for (const auto &header : headers)
        {
            const bool sending = kind.rfind("sender", 0) == 0;
            check(header_field(header, "groupId") == (sending ? caller_id : accepted_id),
                  "1alpha3: groupId is this end's group handle: " + header);
            const auto peer = header_field(header, "peerGroupId");
            if (sending || !robotweax)
                check(peer == "null", "1alpha3: no peer group ID on the caller or without Robotweax SRT: " + header);
            else
                check(peer == caller_id, "1alpha3: the accepting side noted the caller's group ID: " + header);
        }
    }
    if (labels)
    {
        std::map<std::string, std::map<std::string, std::int64_t>> starts; /* label -> role -> start */
        for (const auto *kind : {"sender/member", "receiver/member"})
            for (const auto &header : rows[kind])
                starts[header_field(header, "pathLabel")][header_field(header, "role")] =
                    std::stoll(header_field(header, "connectedAtUnixNano"));
        check(starts.size() == 2 && starts.count("6c7465") == 1 && starts.count("736174") == 1,
              "1alpha3: both ends' member rows carry the labels lte and sat");
        for (const auto &[label, by_role] : starts)
        {
            if (by_role.size() != 2)
                continue;
            const auto difference = std::llabs(by_role.at("sender") - by_role.at("receiver"));
            std::cout << "1alpha3 group pair: path " << label << " connection starts differ by " << difference / 1000
                      << " us\n";
            check(difference < 100'000'000, "1alpha3: a path's two connection starts within 100 ms: " +
                                                std::to_string(difference) + " ns");
        }
    }
    else
    {
        for (const auto *kind : {"sender/member", "receiver/member"})
            for (const auto &header : rows[kind])
                check(header_field(header, "pathLabel") == "null", "1alpha3: no label without negotiation: " + header);
    }
    srt_close(listener); /* its callback holds the embed */
    robotweax_srt_telemetry_embed_stop(embed);
    stop = true;
    sender.join();
    srt_close(caller);
    receiver.join();
    if (accepted != SRT_INVALID_SOCK)
        srt_close(accepted);
    std::filesystem::remove_all(directory);
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
    static_cast<void>(wait_for(
        [&]
        {
            journal.read(embed);
            return journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_REMOVED, "path=lte") >= 1;
        },
        5000));
    check(journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_REMOVED, "path=lte") == 1,
          "member_removed for lte: " + std::to_string(journal.count(ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_REMOVED, "path=lte")));
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
 * code. if_offered: the listener admits labeled and unlabeled callers
 * (SRTO_ROBOTWEAX_PATHID_POLICY IF_OFFERED, Robotweax SRT 0.3.0); a
 * labeled caller is negotiated as with REQUIRED. */
void negotiated_labels_scenario(const bool if_offered = false)
{
    const SRTSOCKET listener = srt_create_socket();
    int yes = 1;
    srt_setsockflag(listener, SRTO_GROUPCONNECT, &yes, sizeof(yes));
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    bool listener_policy = false;
#if defined(ROBOTWEAX_SRT_EXTENSION_IF_OFFERED)
    if (if_offered)
    {
        int policy = ROBOTWEAX_SRT_EXTENSION_IF_OFFERED;
        listener_policy = srt_setsockflag(listener, SRTO_ROBOTWEAX_PATHID_POLICY, &policy, sizeof(policy)) == 0;
    }
#endif
    if (!if_offered)
    {
        listener_policy = srt_setsockflag(listener, SRTO_ROBOTWEAX_PATHID_REQUIRED, &yes, sizeof(yes)) == 0;
    }
    check(srt_setsockflag(group, SRTO_ROBOTWEAX_PATHID_REQUIRED, &yes, sizeof(yes)) == 0 && listener_policy,
          if_offered ? "labels: caller requires, listener admits if offered"
                     : "labels: both peers require path labels");
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

/* Main/Backup sender, paths "lte" (weight 20) and "sat" (weight 10) each
 * through an impairing relay; "lte" goes dark while both members stay
 * connected (a long peer idle timeout). SRT moves the stream to "sat" but
 * keeps putting packets into "lte" until it breaks, so per tick both paths
 * may carry and the inferred active path flaps. The sending group must not
 * take the dark path's grade while "sat" carries and both paths are still
 * usable: no Watch, At risk or Failing (srt_telemetry#38). With the send
 * authority the group follows SRT's selection; without it, the paths that
 * carried together. */
void backup_outage_scenario(const bool authority)
{
    using robotweax_srt_telemetry_lab::Impairment;
    using robotweax_srt_telemetry_lab::Relay;
    const std::string stream = authority ? "outage-auth" : "outage";
    const SRTSOCKET listener = srt_create_socket();
    int yes = 1;
    srt_setsockflag(listener, SRTO_GROUPCONNECT, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    int length = sizeof(address);
    Relay relays[2];
    check(srt_bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
              srt_listen(listener, 4) == 0 &&
              srt_getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0 &&
              relays[0].start(address) && relays[1].start(address),
          "outage: listen and relays");
    Impairment fast;
    fast.delay_ms = 2;
    relays[0].set(fast);
    relays[1].set(fast);
    std::atomic<bool> stop{false};
    std::atomic<SRTSOCKET> accepted{SRT_INVALID_SOCK};
    std::thread receiver([&] {
        const SRTSOCKET socket = srt_accept(listener, nullptr, nullptr);
        if (socket == SRT_INVALID_SOCK)
            return;
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
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BACKUP);
    int idle = 30000; /* both members stay usable through the outage */
    int latency = 250;
    srt_setsockflag(group, SRTO_PEERIDLETIMEO, &idle, sizeof(idle));
    srt_setsockflag(group, SRTO_LATENCY, &latency, sizeof(latency));
    std::vector<SRT_SOCKGROUPCONFIG> endpoints;
    for (int i = 0; i < 2; ++i)
    {
        const auto &relay = relays[i].address();
        endpoints.push_back(srt_prepare_endpoint(nullptr, reinterpret_cast<const sockaddr *>(&relay), sizeof(relay)));
        endpoints.back().weight = i == 0 ? 20 : 10;
    }
    check(srt_connect_group(group, endpoints.data(), 2) != SRT_ERROR, "outage: connect");
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
    robotweax_srt_telemetry_embed *embed = start_embed(0);
#if defined(ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION) && ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION == 1
    if (authority)
        check(robotweax_srt_telemetry_host_embed_group_send_state(embed) == ROBOTWEAX_SRT_TELEMETRY_OK,
              "outage: send authority enabled");
#endif
    robotweax_srt_telemetry_label_v1 labels[] = {{"stream", stream.c_str()}};
    robotweax_srt_telemetry_embed_group_v1 tx;
    robotweax_srt_telemetry_embed_group_default(&tx);
    tx.group = group;
    tx.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    tx.group_type = ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP;
    tx.expected_paths = 2;
    tx.labels = labels;
    tx.label_count = 1;
    tx.path_name = name_by_weight;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &tx) == ROBOTWEAX_SRT_TELEMETRY_OK, "outage: attach");
    const auto row = [&] { return group_row(sources(embed), stream); };
    const auto path = [&](const char *name)
    { return find_source(sources(embed), {"\"stream\":\"" + stream + "\"", std::string("\"path\":\"") + name + "\""}); };
    check(wait_for(
              [&] {
                  return has(row(), "\"verdict\":\"ok\"") &&
                         has(row(), "\"group\":{\"usable_paths\":2,\"expected_paths\":2}") &&
                         has(path("sat"), "\"standby\":true");
              },
              15000),
          "outage: OK on lte with sat standby before the outage: " + row().substr(0, 0));

    Impairment dark;
    dark.blackhole = true;
    relays[0].set(dark);
    std::string false_grade;
    bool sat_carried = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(6000);
    while (std::chrono::steady_clock::now() < end)
    {
        const auto all = sources(embed);
        const auto g = group_row(all, stream);
        const auto sat = find_source(all, {"\"stream\":\"" + stream + "\"", "\"path\":\"sat\""});
        sat_carried = sat_carried || (has(sat, "\"standby\":false") && has(sat, "\"verdict\":\"ok\""));
        if (has(g, "\"group\":{\"usable_paths\":2,\"expected_paths\":2}") &&
            (has(g, "\"verdict\":\"watch\"") || has(g, "\"verdict\":\"at_risk\"") ||
             has(g, "\"verdict\":\"failing\"")) &&
            false_grade.empty())
        {
            const auto h = g.find("\"health\"");
            false_grade = h == std::string::npos ? g : g.substr(h, 120);
        }
        sleep_ms(50);
    }
    check(sat_carried, "outage: sat took over, graded OK");
    check(false_grade.empty(), "outage: the sending group never takes the dark path's grade while sat carries: " +
                                   false_grade);
    robotweax_srt_telemetry_embed_stop(embed);
    stop = true;
    sender.join();
    srt_close(group);
    srt_close(listener);
    receiver.join();
    if (accepted != SRT_INVALID_SOCK)
        srt_close(accepted);
}

#if defined(ROBOTWEAX_SRT_PEER_IDENTITY_VERSION) && defined(ROBOTWEAX_SRT_PEER_HANDSHAKE_VERSION)
int note_member_handshake(void *embed, SRTSOCKET socket, int, const sockaddr *, const char *)
{
    static_cast<void>(robotweax_srt_telemetry_host_embed_note_peer_handshake(
        static_cast<robotweax_srt_telemetry_embed *>(embed), socket));
    /* Robotweax SRT admits a group declaration from a listen callback only
     * with a group domain (since 0.2.8); returning 0 alone refuses it. */
    const std::uint64_t domain = 1;
    srt_setsockflag(socket, SRTO_ROBOTWEAX_GROUPDOMAIN, &domain, sizeof(domain));
    return 0;
}

/* Robotweax SRT 0.3.0 peer metadata on group members: SRT answers per member
 * socket, never for a group handle. The listener advertises its identity, so
 * each caller member reads it; the listen callback runs once per member, so
 * each accepted member shows its caller's handshake. Group rows have none. */
void peer_metadata_group_scenario()
{
    robotweax_srt_telemetry_embed *embed = start_embed(0);
    if (embed == nullptr)
        return;
    check(robotweax_srt_telemetry_host_embed_peer_identity(embed) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "group peer: identity enabled");
    const SRTSOCKET listener = srt_create_socket();
    int yes = 1;
    std::int32_t full = ROBOTWEAX_SRT_IDENTITY_NAME_AND_RELEASE;
    check(srt_setsockflag(listener, SRTO_GROUPCONNECT, &yes, sizeof(yes)) == 0 &&
              srt_setsockflag(listener, SRTO_ROBOTWEAX_IDENTITY_ADVERTISEMENT, &full, sizeof(full)) == 0 &&
              srt_listen_callback(listener, note_member_handshake, embed) == 0,
          "group peer: listener advertises and notes handshakes");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    int length = sizeof(address);
    check(srt_bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
              srt_listen(listener, 4) == 0 &&
              srt_getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0,
          "group peer: listen");
    std::atomic<SRTSOCKET> accepted{SRT_INVALID_SOCK};
    std::atomic<bool> stop{false};
    std::thread receiver([&] {
        const SRTSOCKET socket = srt_accept(listener, nullptr, nullptr);
        if (socket == SRT_INVALID_SOCK)
            return;
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
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    std::vector<SRT_SOCKGROUPCONFIG> endpoints;
    for (const std::uint16_t weight : {20, 10})
    {
        endpoints.push_back(srt_prepare_endpoint(nullptr, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
        endpoints.back().weight = weight;
    }
    check(srt_connect_group(group, endpoints.data(), 2) != SRT_ERROR, "group peer: connect");
    std::thread sender([&] {
        char payload[1316] = {};
        while (!stop)
        {
            SRT_MSGCTRL control = srt_msgctrl_default;
            static_cast<void>(srt_sendmsg2(group, payload, sizeof(payload), &control));
            sleep_ms(2);
        }
    });
    check(wait_for([&] { return accepted != SRT_INVALID_SOCK; }, 5000), "group peer: accepted");
    robotweax_srt_telemetry_label_v1 labels[] = {{"stream", "peer-group"}};
    robotweax_srt_telemetry_embed_group_v1 tx;
    robotweax_srt_telemetry_embed_group_default(&tx);
    tx.group = group;
    tx.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    tx.group_type = ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BROADCAST;
    tx.labels = labels;
    tx.label_count = 1;
    tx.path_name = name_by_weight;
    auto rx = tx;
    rx.group = accepted;
    rx.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &tx) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_embed_attach_group(embed, &rx) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "group peer: both groups attached");
    const std::string release = std::string("\"release\":\"") + ROBOTWEAX_SRT_VERSION_STRING + "\"";
    const std::string listener_identity = "\"identity\":{\"availability\":\"available\",\"name\":\"Robotweax SRT\"," + release;
    const bool read = wait_for(
        [&]
        {
            const auto all = sources(embed);
            return count_with(all, {"\"stream\":\"peer-group\"", "\"role\":\"sender\"", "\"path\":",
                                    listener_identity}) == 2 &&
                   count_with(all, {"\"stream\":\"peer-group\"", "\"role\":\"receiver\"", "\"path\":",
                                    "\"handshake\":{\"availability\":\"available\",\"srt_version\":\"1.5.7\""}) == 2;
        },
        8000);
    const auto all = sources(embed);
    check(read, "group peer: caller members read the listener's identity, accepted members their caller's handshake: " +
                    std::to_string(count_with(all, {"\"stream\":\"peer-group\"", "\"path\":", listener_identity})) +
                    " identities");
    check(count_with(all, {"\"stream\":\"peer-group\"", "\"role\":\"receiver\"", "\"path\":",
                           "\"identity\":{\"availability\":\"unknown\"}"}) == 2,
          "group peer: callers that do not advertise read unknown");
    check(count_with(all, {"\"stream\":\"peer-group\"", "\"peer\":{\"version\":1,\"identity\":{\"availability\":"
                           "\"unavailable\"},\"handshake\":{\"availability\":\"unavailable\"}}"}) == 2,
          "group peer: the group rows have no peer metadata");
    /* The listener's callback holds the embed: close it before stop. */
    srt_close(listener);
    robotweax_srt_telemetry_embed_stop(embed);
    stop = true;
    sender.join();
    srt_close(group);
    receiver.join();
    if (accepted != SRT_INVALID_SOCK)
        srt_close(accepted);
}
#endif

#if defined(ROBOTWEAX_SRT_STANDBY_RTT_VERSION) && ROBOTWEAX_SRT_STANDBY_RTT_VERSION == 1
/* Plain own-to-own loopback integration of the opt-in getter into member JSON.
 * No DATA is sent. This is not fleet, WAN or encrypted-control qualification. */
void native_standby_observation_scenario()
{
    const auto listener = srt_create_socket();
    const auto caller = srt_create_group(SRT_GTYPE_BACKUP);
    bool yes = true;
    check(srt_setsockflag(listener, SRTO_GROUPCONNECT, &yes, sizeof(yes)) == 0,
          "standby group listener");
    for (const auto socket : {listener, caller})
    {
        check(srt_setsockflag(socket, SRTO_ROBOTWEAX_STANDBYRTT_REQUIRED, &yes, sizeof(yes)) == 0,
              "bilateral explicit standby setup");
        int timeout = 2000;
        check(srt_setsockflag(socket, SRTO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
              "bounded accept");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(0x7f000001);
    int length = sizeof(address);
    check(srt_bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
              srt_listen(listener, 4) == 0 &&
              srt_getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0,
          "standby native loopback listen");
    std::vector<SRT_SOCKGROUPCONFIG> endpoints;
    for (int i = 0; i < 2; ++i)
        endpoints.push_back(
            srt_prepare_endpoint(nullptr, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
    check(srt_connect_group(caller, endpoints.data(), 2) != SRT_INVALID_SOCK,
          "standby group connect");
    const auto receiver = srt_accept(listener, nullptr, nullptr);
    check(receiver != SRT_INVALID_SOCK, "standby group accept");
    robotweax_srt_telemetry_embed_config_v1 config{};
    robotweax_srt_telemetry_embed_config_default(&config);
    robotweax_srt_telemetry_host_embed_functions(&config);
    config.flags = ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP;
    robotweax_srt_telemetry_embed *embed = nullptr;
    check(robotweax_srt_telemetry_embed_start(&config, &embed) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "standby native embed start");
    check(robotweax_srt_telemetry_host_embed_standby_rtt_data(embed) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "standby native opt-in");
    robotweax_srt_telemetry_embed_group_v1 group{};
    robotweax_srt_telemetry_embed_group_default(&group);
    group.group = caller;
    group.group_type = ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP;
    group.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &group) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "standby attach caller");
    group.group = receiver;
    group.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;
    check(robotweax_srt_telemetry_embed_attach_group(embed, &group) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "standby attach receiver");
    check(wait_for(
              [&]
              {
                  return count_with(
                             sources(embed),
                             {"\"path\":",
                              "\"standby_rtt\":{\"version\":1,\"availability\":\"available\","
                              "\"negotiated\":true,\"admitted\":true,\"observed\":true",
                              "\"rtt_microseconds\":\"", "\"fresh_at_capture\":true"}) == 4;
              },
              7000),
          "native positive measurements reach all four member JSON rows");
    check(robotweax_srt_telemetry_embed_set_standby_rtt_data_v1(embed, nullptr) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "native getter disable");
    check(count_with(sources(embed),
                     {"\"standby_rtt\":{\"version\":1,\"availability\":\"unavailable\"}"}) == 6,
          "native observations clear immediately");
    robotweax_srt_telemetry_embed_stop(embed);
    srt_close(caller);
    if (receiver != SRT_INVALID_SOCK)
        srt_close(receiver);
    srt_close(listener);
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
#if defined(ROBOTWEAX_SRT_STANDBY_RTT_VERSION) && ROBOTWEAX_SRT_STANDBY_RTT_VERSION == 1
    native_standby_observation_scenario();
#endif
#if defined(ROBOTWEAX_SRT_PEER_IDENTITY_VERSION) && defined(ROBOTWEAX_SRT_PEER_HANDSHAKE_VERSION)
    peer_metadata_group_scenario();
#endif
    backup_scenario();
    backup_outage_scenario(false);
#if defined(ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION) && ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION == 1
    backup_outage_scenario(true);
#endif
    recording_1alpha2_scenario();
    recording_1alpha3_scenario();
    broadcast_scenario();
    duplicate_names_scenario();
    listener_names_reconnect_scenario();
    overlap_scenario();
    reserved_labels_scenario();
    path_data_provider_scenario();
#if defined(ROBOTWEAX_SRT_PATHID_NEGOTIATED)
    negotiated_labels_scenario();
#if defined(ROBOTWEAX_SRT_EXTENSION_IF_OFFERED)
    negotiated_labels_scenario(true);
#endif
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
