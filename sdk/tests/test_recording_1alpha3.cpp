/* Recording schema 1alpha3 (srt_telemetry#57): each sample's connection age,
 * the connection start in the header, the session reference, group IDs and
 * the negotiated path label, which SRT Doctor uses to pair two ends'
 * recordings of one session. With --jsonl DIR the test also writes a plain,
 * a group and a member recording there for test_recording_schema.py. */

#include <robotweax/srt_telemetry.h>
#include <robotweax/srt_telemetry_health.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{
int failures = 0;

void check(const bool pass, const std::string &message)
{
    if (!pass)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool has(const std::string &text, const std::string &part)
{
    return text.find(part) != std::string::npos;
}

constexpr std::uint64_t second = 1'000'000'000;
constexpr std::uint64_t milli = 1'000'000;
constexpr std::uint64_t wall0 = 1'800'000'000 * second;
constexpr std::uint64_t mono0 = 5 * second;

robotweax_srt_telemetry_raw_sample_v1 raw_at(const std::uint64_t n)
{
    robotweax_srt_telemetry_raw_sample_v1 raw{};
    raw.struct_size = sizeof(raw);
    raw.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    raw.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    raw.observed_at_unix_nano = wall0 + n * second / 10;
    raw.sample_monotonic_nano = mono0 + n * second / 10;
    raw.available_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL;
    raw.unique_sent_packet_total = n * 50;
    return raw;
}

robotweax_srt_telemetry_delta_v1 delta_at(const std::uint64_t n)
{
    robotweax_srt_telemetry_delta_v1 delta{};
    delta.struct_size = sizeof(delta);
    delta.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    delta.interval_nano = n == 0 ? 0 : second / 10;
    delta.quality_flags = n == 0 ? 1u : 0u;
    return delta;
}

robotweax_srt_telemetry_recorder_connection_v1 connection(const bool available, const std::uint64_t age)
{
    robotweax_srt_telemetry_recorder_connection_v1 c{};
    c.struct_size = sizeof(c);
    c.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    c.available = available ? 1u : 0u;
    c.connection_age_nano = age;
    return c;
}

robotweax_srt_telemetry_recorder_member_v1 running()
{
    robotweax_srt_telemetry_recorder_member_v1 m{};
    m.struct_size = sizeof(m);
    m.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    m.state = ROBOTWEAX_SRT_TELEMETRY_RECORDER_MEMBER_RUNNING;
    return m;
}

int collect(void *user, const char *line, const std::size_t size)
{
    static_cast<std::vector<std::string> *>(user)->emplace_back(line, size);
    return 0;
}

robotweax_srt_telemetry_jsonl_export_config_v1 config(const char *flow)
{
    robotweax_srt_telemetry_jsonl_export_config_v1 c{};
    c.struct_size = sizeof(c);
    c.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    c.stream_id = "cam-9";
    c.flow_id = flow;
    c.provider_version = "0.3.0";
    c.provider_revision = "test";
    c.build_profile = "test";
    c.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    c.configured_provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    return c;
}

robotweax_srt_telemetry_jsonl_export_v3_options options(const std::uint32_t type, const std::uint32_t row)
{
    robotweax_srt_telemetry_jsonl_export_v3_options o{};
    o.struct_size = sizeof(o);
    o.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    o.group_type = type;
    o.group_row = row;
    o.health_lines = 1;
    o.group_id = -1;
    o.peer_group_id = -1;
    return o;
}

std::vector<std::string> export_v3(robotweax_srt_telemetry_recorder *recorder,
                                   const robotweax_srt_telemetry_jsonl_export_v3_options &o,
                                   robotweax_srt_telemetry_error *error = nullptr, const char *flow = "cam-9")
{
    std::vector<std::string> lines;
    const auto c = config(flow);
    const auto e = robotweax_srt_telemetry_recorder_export_jsonl_v3(recorder, &c, &o, collect, &lines);
    if (error != nullptr)
        *error = e;
    else
        check(e == ROBOTWEAX_SRT_TELEMETRY_OK, "export_jsonl_v3 succeeds");
    return lines;
}

void write_file(const std::string &path, const std::vector<std::string> &lines)
{
    std::ofstream out(path, std::ios::binary);
    for (const auto &line : lines)
        out << line;
}

void push(robotweax_srt_telemetry_recorder *recorder, const std::uint64_t n, const bool available,
          const std::uint64_t age, const robotweax_srt_telemetry_recorder_member_v1 *member = nullptr)
{
    const auto raw = raw_at(n);
    const auto delta = delta_at(n);
    const auto c = connection(available, age);
    check(robotweax_srt_telemetry_recorder_push_sample_v1(recorder, &raw, &delta, member, &c) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "push_sample_v1 " + std::to_string(n));
}

std::string field(const std::string &line, const std::string &name)
{
    const auto key = "\"" + name + "\":";
    const auto at = line.find(key);
    if (at == std::string::npos)
        return "<absent>";
    const auto start = at + key.size();
    auto end = start;
    if (line[start] == '"')
        end = line.find('"', start + 1) + 1;
    else
        while (end < line.size() && line[end] != ',' && line[end] != '}')
            ++end;
    return line.substr(start, end - start);
}
} // namespace

int main(int argc, char **argv)
{
    const std::string jsonl_dir = argc > 2 && std::string(argv[1]) == "--jsonl" ? argv[2] : "";
    const auto none = ROBOTWEAX_SRT_TELEMETRY_RECORDING_GROUP_NONE;
    const auto backup = ROBOTWEAX_SRT_TELEMETRY_RECORDING_GROUP_BACKUP;
    const auto plain_row = ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_NONE;
    const auto group_row = ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_GROUP;
    const auto member_row = ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_MEMBER;

    /* A plain socket: two samples while connecting (no age), then a
     * connection whose statistics started 250 ms before sample 2, a ring of
     * 4 that overwrites, and a reconnection at sample 8. */
    robotweax_srt_telemetry_recorder *recorder = nullptr;
    check(robotweax_srt_telemetry_recorder_create(4, &recorder) == ROBOTWEAX_SRT_TELEMETRY_OK, "create");
    push(recorder, 0, false, 0);
    push(recorder, 1, false, 0);
    {
        auto lines = export_v3(recorder, options(none, plain_row));
        check(lines.size() == 3 && field(lines[0], "connectedAtUnixNano") == "null" &&
                  field(lines[0], "connectedAtMonotonicNano") == "null" && field(lines[1], "connectionAgeNano") == "null",
              "no connection age yet: no connection start: " + (lines.empty() ? "" : lines[0]));
    }
    for (std::uint64_t n = 2; n < 8; ++n)
        push(recorder, n, true, 250 * milli + (n - 2) * second / 10);
    auto lines = export_v3(recorder, options(none, plain_row));
    check(lines.size() == 5, "a ring of 4: header and 4 samples");
    /* Samples 4..7 kept; the connection started at sample 2's time - 250 ms. */
    const auto start_wall = wall0 + 2 * second / 10 - 250 * milli;
    const auto start_mono = mono0 + 2 * second / 10 - 250 * milli;
    check(field(lines[0], "schemaVersion") == "\"1alpha3\"" && field(lines[0], "overwrittenCount") == "4" &&
              field(lines[0], "connectedAtUnixNano") == std::to_string(start_wall) &&
              field(lines[0], "connectedAtMonotonicNano") == std::to_string(start_mono),
          "the connection start survives the overwrite: " + lines[0]);
    check(field(lines[0], "sessionRef") == "null" && field(lines[0], "groupId") == "null" &&
              field(lines[0], "peerGroupId") == "null" && field(lines[0], "pathLabel") == "null",
          "a plain socket without a session reference: " + lines[0]);
    check(field(lines[1], "schemaVersion") == "\"1alpha3\"" &&
              field(lines[1], "connectionAgeNano") == std::to_string(250 * milli + 2 * second / 10) &&
              has(lines[1], ",\"member\":null,\"connectionAgeNano\":"),
          "each sample carries its connection age: " + lines[1]);
    /* A reconnection: the age starts again; the header keeps describing the
     * first kept sample's connection until the ring has moved past it. */
    push(recorder, 8, true, 20 * milli);
    push(recorder, 9, true, 120 * milli);
    lines = export_v3(recorder, options(none, plain_row));
    check(field(lines[3], "connectionAgeNano") == std::to_string(20 * milli) &&
              field(lines[0], "connectedAtUnixNano") == std::to_string(start_wall),
          "a new connection shows as a decreasing age: " + lines[3]);
    for (std::uint64_t n = 10; n < 12; ++n)
        push(recorder, n, n == 11, 20 * milli + (n - 8) * second / 10);
    lines = export_v3(recorder, options(none, plain_row));
    check(field(lines[0], "connectedAtUnixNano") == std::to_string(wall0 + 8 * second / 10 - 20 * milli) &&
              field(lines[3], "connectionAgeNano") == "null",
          "the header follows the first kept sample's connection, skipping samples without an age: " + lines[0]);

    /* Session reference and health lines in 1alpha3. */
    auto with_ref = options(none, plain_row);
    with_ref.session_ref = "sha256:6b1f0c2e";
    auto status = robotweax_srt_telemetry_health_status_v1{};
    status.struct_size = sizeof(status);
    status.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    status.observed_at_unix_nano = wall0 + 11 * second / 10;
    status.sample_monotonic_nano = mono0 + 11 * second / 10;
    status.verdict_since_monotonic_nano = status.sample_monotonic_nano;
    status.verdict = ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK;
    status.uncapped_verdict = ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK;
    status.reason = ROBOTWEAX_SRT_TELEMETRY_REASON_NONE;
    check(robotweax_srt_telemetry_recorder_push_health_v1(recorder, &status) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "push health");
    lines = export_v3(recorder, with_ref);
    check(field(lines[0], "sessionRef") == "\"sha256:6b1f0c2e\"", "session reference: " + lines[0]);
    check(lines.size() == 6 && field(lines.back(), "schemaVersion") == "\"1alpha3\"" &&
              has(lines.back(), "\"kind\":\"robotweax-srt-telemetry-health\""),
          "health lines say 1alpha3");
    if (!jsonl_dir.empty())
        write_file(jsonl_dir + "/plain.jsonl", lines);

    /* push_member_v1 and push_sample_v1 without a connection: no age. */
    {
        const auto raw = raw_at(12);
        const auto delta = delta_at(12);
        check(robotweax_srt_telemetry_recorder_push(recorder, &raw, &delta) == ROBOTWEAX_SRT_TELEMETRY_OK &&
                  robotweax_srt_telemetry_recorder_push_sample_v1(recorder, &raw, &delta, nullptr, nullptr) ==
                      ROBOTWEAX_SRT_TELEMETRY_OK,
              "push without a connection");
        lines = export_v3(recorder, options(none, plain_row));
        /* header, samples 10 and 11, the health line graded on 11, then the two new samples */
        check(lines.size() == 6 && field(lines[4], "connectionAgeNano") == "null" &&
                  field(lines[5], "connectionAgeNano") == "null",
              "samples pushed without a connection have no age");
        /* 1alpha2 export of the same ring: no 1alpha3 fields. */
        robotweax_srt_telemetry_jsonl_export_v2_options v2{};
        v2.struct_size = sizeof(v2);
        v2.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        std::vector<std::string> v2_lines;
        const auto c = config("cam-9");
        check(robotweax_srt_telemetry_recorder_export_jsonl_v2(recorder, &c, &v2, collect, &v2_lines) ==
                      ROBOTWEAX_SRT_TELEMETRY_OK &&
                  !has(v2_lines[0], "connectedAt") && !has(v2_lines[1], "connectionAgeNano") &&
                  field(v2_lines[0], "schemaVersion") == "\"1alpha2\"",
              "1alpha2 export is unchanged");
    }

    /* Refusals. */
    {
        const auto raw = raw_at(13);
        const auto delta = delta_at(13);
        auto c = connection(true, std::numeric_limits<std::uint64_t>::max());
        check(robotweax_srt_telemetry_recorder_push_sample_v1(recorder, &raw, &delta, nullptr, &c) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
              "an age of UINT64_MAX is refused");
        c = connection(false, 0);
        c.available = 2;
        check(robotweax_srt_telemetry_recorder_push_sample_v1(recorder, &raw, &delta, nullptr, &c) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
              "available must be 0 or 1");
        c = connection(true, 1);
        c.reserved = 1;
        check(robotweax_srt_telemetry_recorder_push_sample_v1(recorder, &raw, &delta, nullptr, &c) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
              "reserved must be zero");
        c = connection(true, 1);
        c.struct_size = 8;
        check(robotweax_srt_telemetry_recorder_push_sample_v1(recorder, &raw, &delta, nullptr, &c) ==
                  ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH,
              "a short connection struct is an ABI mismatch");

        const auto refused = [&](robotweax_srt_telemetry_jsonl_export_v3_options o, const std::string &what)
        {
            robotweax_srt_telemetry_error error = ROBOTWEAX_SRT_TELEMETRY_OK;
            const auto out = export_v3(recorder, o, &error);
            check(error == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT && out.empty(), what);
        };
        auto o = options(none, plain_row);
        o.group_id = 7;
        refused(o, "a plain socket has no group ID");
        o = options(none, plain_row);
        o.peer_group_id = 7;
        refused(o, "a plain socket has no peer group ID");
        o = options(backup, group_row);
        o.path_label_length = 2;
        o.path_label[0] = 'a';
        o.path_label[1] = 'b';
        refused(o, "a group row has no path label");
        o = options(backup, member_row);
        o.path_label_length = 33;
        refused(o, "a path label of at most 32 bytes");
        o = options(backup, member_row);
        o.path_label_length = 1;
        o.path_label[0] = 'a';
        o.path_label[1] = 'b';
        refused(o, "bytes after the label must be zero");
        o = options(backup, member_row);
        o.group_id = -2;
        refused(o, "group IDs are -1 or a handle");
        o = options(none, plain_row);
        o.session_ref = "";
        refused(o, "an empty session reference");
        o.session_ref = "tab\there";
        refused(o, "a session reference of printable ASCII");
        const std::string long_ref(257, 'x');
        o.session_ref = long_ref.c_str();
        refused(o, "a session reference of at most 256 bytes");
        const std::string max_ref(256, 'x');
        o.session_ref = max_ref.c_str();
        robotweax_srt_telemetry_error error = ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
        static_cast<void>(export_v3(recorder, o, &error));
        check(error == ROBOTWEAX_SRT_TELEMETRY_OK, "a session reference of 256 bytes");
        o = options(none, plain_row);
        o.reserved[1] = 1;
        refused(o, "reserved must be zero");
        o = options(none, plain_row);
        o.struct_size = sizeof(o) - 8;
        static_cast<void>(export_v3(recorder, o, &error));
        check(error == ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH, "a short options struct is an ABI mismatch");
    }
    robotweax_srt_telemetry_recorder_destroy(recorder);

    /* A Backup group's row and one member: group IDs on both, the label in
     * hex on the member ("lte" and a non-printable byte). */
    robotweax_srt_telemetry_recorder *group = nullptr;
    robotweax_srt_telemetry_recorder *member = nullptr;
    check(robotweax_srt_telemetry_recorder_create(10, &group) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_recorder_create(10, &member) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "create group and member recorders");
    const auto m = running();
    for (std::uint64_t n = 0; n < 4; ++n)
    {
        push(group, n, true, 2 * second + n * second / 10);
        push(member, n, n > 0, 1500 * milli + n * second / 10, &m);
    }
    auto group_options = options(backup, group_row);
    group_options.group_id = 0x40000005;
    group_options.peer_group_id = 0x4000000a;
    group_options.session_ref = "s=7f3a;x-ch=live";
    auto group_lines = export_v3(group, group_options, nullptr, "cam-9#3");
    check(field(group_lines[0], "groupId") == std::to_string(0x40000005) &&
              field(group_lines[0], "peerGroupId") == std::to_string(0x4000000a) &&
              field(group_lines[0], "pathLabel") == "null" &&
              field(group_lines[0], "connectedAtUnixNano") == std::to_string(wall0 - 2 * second) &&
              field(group_lines[0], "groupRow") == "\"group\"",
          "group row: IDs, no label, start of its first member: " + group_lines[0]);
    auto member_options = options(backup, member_row);
    member_options.group_id = 0x40000005;
    member_options.peer_group_id = -1;
    member_options.path_label_length = 4;
    std::memcpy(member_options.path_label, "lte\x01", 4);
    auto member_lines = export_v3(member, member_options, nullptr, "cam-9#3/lte");
    check(field(member_lines[0], "pathLabel") == "\"6c746501\"" && field(member_lines[0], "peerGroupId") == "null" &&
              field(member_lines[0], "connectedAtUnixNano") == std::to_string(wall0 + second / 10 - 1600 * milli) &&
              field(member_lines[1], "connectionAgeNano") == "null" &&
              has(member_lines[2], "\"member\":{\"state\":\"running\"") &&
              has(member_lines[2], "\"authority\":null},\"connectionAgeNano\":"),
          "member row: hex label, start from its first connected sample: " + member_lines[0]);
    if (!jsonl_dir.empty())
    {
        write_file(jsonl_dir + "/group.jsonl", group_lines);
        write_file(jsonl_dir + "/member.jsonl", member_lines);
    }
    robotweax_srt_telemetry_recorder_destroy(group);
    robotweax_srt_telemetry_recorder_destroy(member);

    if (failures != 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "recording 1alpha3 tests passed\n";
    return 0;
}
