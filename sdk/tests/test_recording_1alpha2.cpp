/* Recording schema 1alpha2 (srt_telemetry#47): group context, member state
 * per sample and health transitions, as srt_doctor's 1alpha2 reader expects.
 * With --jsonl DIR the test also writes a group row and a member row there
 * for the schema test (test_recording_schema.py). */

#include <robotweax/srt_telemetry.h>
#include <robotweax/srt_telemetry_health.h>

#include <cstdint>
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
constexpr std::uint64_t wall0 = 1'800'000'000 * second;

robotweax_srt_telemetry_raw_sample_v1 raw_at(const std::uint64_t n)
{
    robotweax_srt_telemetry_raw_sample_v1 raw{};
    raw.struct_size = sizeof(raw);
    raw.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    raw.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    raw.observed_at_unix_nano = wall0 + n * second / 10;
    raw.sample_monotonic_nano = second + n * second / 10;
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

robotweax_srt_telemetry_recorder_member_v1 member(const std::uint32_t state, const bool standby,
                                                   const std::uint32_t authority)
{
    robotweax_srt_telemetry_recorder_member_v1 m{};
    m.struct_size = sizeof(m);
    m.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    m.state = state;
    m.standby = standby ? 1 : 0;
    m.authority = authority;
    return m;
}

robotweax_srt_telemetry_health_status_v1 status_at(const std::uint64_t n, const robotweax_srt_telemetry_health_verdict verdict,
                                                   const robotweax_srt_telemetry_health_reason reason, const bool standby,
                                                   const std::uint64_t since_n)
{
    robotweax_srt_telemetry_health_status_v1 s{};
    s.struct_size = sizeof(s);
    s.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    s.observed_at_unix_nano = wall0 + n * second / 10;
    s.sample_monotonic_nano = second + n * second / 10;
    s.verdict_since_monotonic_nano = second + since_n * second / 10;
    s.verdict = verdict;
    s.uncapped_verdict = verdict;
    s.reason = reason;
    s.flags = standby ? ROBOTWEAX_SRT_TELEMETRY_HEALTH_STATUS_STANDBY : 0u;
    return s;
}

int collect(void *user, const char *line, const std::size_t size)
{
    static_cast<std::vector<std::string> *>(user)->emplace_back(line, size);
    return 0;
}

robotweax_srt_telemetry_jsonl_export_config_v1 config()
{
    robotweax_srt_telemetry_jsonl_export_config_v1 c{};
    c.struct_size = sizeof(c);
    c.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    c.stream_id = "cam-9";
    c.flow_id = "cam-9#3/lte";
    c.provider_version = "1.5.7";
    c.provider_revision = "test";
    c.build_profile = "test";
    c.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    c.configured_provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    return c;
}

robotweax_srt_telemetry_jsonl_export_v2_options options(const std::uint32_t type, const std::uint32_t row,
                                                         const bool health = true)
{
    robotweax_srt_telemetry_jsonl_export_v2_options o{};
    o.struct_size = sizeof(o);
    o.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    o.group_type = type;
    o.group_row = row;
    o.health_lines = health ? 1 : 0;
    return o;
}

std::vector<std::string> export_v2(robotweax_srt_telemetry_recorder *recorder,
                                   const robotweax_srt_telemetry_jsonl_export_v2_options &o,
                                   robotweax_srt_telemetry_error *error = nullptr)
{
    std::vector<std::string> lines;
    const auto c = config();
    const auto e = robotweax_srt_telemetry_recorder_export_jsonl_v2(recorder, &c, &o, collect, &lines);
    if (error != nullptr)
    {
        *error = e;
    }
    else
    {
        check(e == ROBOTWEAX_SRT_TELEMETRY_OK, "export_jsonl_v2 succeeds");
    }
    return lines;
}

/* "s" for a sample line, "h" / "c" for a health / carried health line. */
std::string shape(const std::vector<std::string> &lines)
{
    std::string out;
    for (std::size_t i = 1; i < lines.size(); ++i)
    {
        out += has(lines[i], "robotweax-srt-telemetry-health") ? (has(lines[i], "\"carried\":true") ? "c" : "h") : "s";
    }
    return out;
}

void write_file(const std::string &path, const std::vector<std::string> &lines)
{
    std::ofstream out(path, std::ios::binary);
    for (const auto &line : lines)
    {
        out << line;
    }
}

} // namespace

int main(int argc, char **argv)
{
    const std::string jsonl_dir = argc > 2 && std::string(argv[1]) == "--jsonl" ? argv[2] : "";
    const auto backup = ROBOTWEAX_SRT_TELEMETRY_RECORDING_GROUP_BACKUP;
    const auto member_row = ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_MEMBER;
    const auto group_row = ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_GROUP;

    robotweax_srt_telemetry_recorder *recorder = nullptr;
    check(robotweax_srt_telemetry_recorder_create(5, &recorder) == ROBOTWEAX_SRT_TELEMETRY_OK, "create");

    /* Health before the first sample has nothing to grade: ignored. */
    const auto early = status_at(0, ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN, ROBOTWEAX_SRT_TELEMETRY_REASON_NO_DATA,
                                 false, 0);
    check(robotweax_srt_telemetry_recorder_push_health_v1(recorder, &early) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "health before any sample is accepted and ignored");

    /* Samples 0-2 standby, 3-4 running with sender authority; transitions
     * after sample 0 (OK / standby) and sample 4 (Watch). */
    for (std::uint64_t n = 0; n < 5; ++n)
    {
        const auto raw = raw_at(n);
        const auto delta = delta_at(n);
        const auto m = n < 3 ? member(ROBOTWEAX_SRT_TELEMETRY_RECORDER_MEMBER_IDLE, true,
                                      ROBOTWEAX_SRT_TELEMETRY_RECORDER_AUTHORITY_NONE)
                             : member(ROBOTWEAX_SRT_TELEMETRY_RECORDER_MEMBER_RUNNING, false,
                                      ROBOTWEAX_SRT_TELEMETRY_RECORDER_AUTHORITY_ACTIVE);
        check(robotweax_srt_telemetry_recorder_push_member_v1(recorder, &raw, &delta, &m) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK,
              "push with member");
        if (n == 0)
        {
            const auto s = status_at(0, ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK, ROBOTWEAX_SRT_TELEMETRY_REASON_STANDBY, true, 0);
            check(robotweax_srt_telemetry_recorder_push_health_v1(recorder, &s) == ROBOTWEAX_SRT_TELEMETRY_OK,
                  "push health");
        }
        if (n == 4)
        {
            auto s = status_at(4, ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH,
                               ROBOTWEAX_SRT_TELEMETRY_REASON_LATENCY_LOW_FOR_RTT, false, 4);
            s.knob = ROBOTWEAX_SRT_TELEMETRY_KNOB_LATENCY;
            s.knob_direction = ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_INCREASE;
            s.suggested_value = 0.25;
            check(robotweax_srt_telemetry_recorder_push_health_v1(recorder, &s) == ROBOTWEAX_SRT_TELEMETRY_OK,
                  "push health");
        }
    }

    auto lines = export_v2(recorder, options(backup, member_row));
    check(lines.size() == 8 && shape(lines) == "shssssh", "samples with each health line after its sample: " + shape(lines));
    check(has(lines[0], "\"schemaVersion\":\"1alpha2\"") && has(lines[0], "\"groupType\":\"backup\"") &&
              has(lines[0], "\"groupRow\":\"member\"") && has(lines[0], "\"healthLines\":true") &&
              has(lines[0], "\"healthCount\":2") && has(lines[0], "\"healthOverwrittenCount\":0") &&
              has(lines[0], "\"recordCount\":5"),
          "1alpha2 header: " + lines[0]);
    check(has(lines[1], "\"schemaVersion\":\"1alpha2\"") &&
              has(lines[1], ",\"member\":{\"state\":\"idle\",\"standby\":true,\"authority\":\"none\"}}"),
          "standby member sample: " + lines[1]);
    check(has(lines[6], "\"member\":{\"state\":\"running\",\"standby\":false,\"authority\":\"active\"}"),
          "running member with sender authority: " + lines[6]);
    check(has(lines[2], "\"sampleIndex\":0") && has(lines[2], "\"verdict\":\"ok\"") &&
              has(lines[2], "\"reason\":\"standby\"") && has(lines[2], "\"standby\":true") &&
              has(lines[2], "\"usablePaths\":null,\"expectedPaths\":null") && has(lines[2], "\"carried\":false") &&
              has(lines[2], "\"observedAtUnixNano\":" + std::to_string(wall0)) &&
              has(lines[2], "\"verdictSinceUnixNano\":" + std::to_string(wall0)) &&
              has(lines[2], "\"suggestedValue\":null") && has(lines[2], "\"knobDirection\":\"none\""),
          "first health line: " + lines[2]);
    check(has(lines[7], "\"sampleIndex\":4") && has(lines[7], "\"verdict\":\"watch\"") &&
              has(lines[7], "\"knob\":\"latency\"") && has(lines[7], "\"knobDirection\":\"increase\"") &&
              has(lines[7], "\"suggestedValue\":0.25"),
          "second health line: " + lines[7]);

    /* 1alpha1 export of the same recorder leaves the additions out. */
    {
        std::vector<std::string> v1;
        const auto c = config();
        check(robotweax_srt_telemetry_recorder_export_jsonl(recorder, &c, collect, &v1) == ROBOTWEAX_SRT_TELEMETRY_OK &&
                  v1.size() == 6 && !has(v1[0], "groupType") && !has(v1[1], "member") &&
                  has(v1[1], "\"schemaVersion\":\"1alpha1\""),
              "1alpha1 export unchanged");
    }

    /* Health off: no lines, zero counts, members still written. */
    lines = export_v2(recorder, options(backup, member_row, false));
    check(shape(lines) == "sssss" && has(lines[0], "\"healthLines\":false") && has(lines[0], "\"healthCount\":0") &&
              has(lines[0], "\"healthOverwrittenCount\":0") && has(lines[1], "\"member\":{"),
          "health off");

    if (!jsonl_dir.empty())
    {
        write_file(jsonl_dir + "/member.jsonl", export_v2(recorder, options(backup, member_row)));
    }

    /* Three more samples: kept are 3-7. The OK transition (sample 0) is no
     * longer graded on a kept sample but is in force until sample 4: carried
     * at index 0. */
    for (std::uint64_t n = 5; n < 8; ++n)
    {
        const auto raw = raw_at(n);
        const auto delta = delta_at(n);
        const auto m = member(ROBOTWEAX_SRT_TELEMETRY_RECORDER_MEMBER_RUNNING, false,
                              ROBOTWEAX_SRT_TELEMETRY_RECORDER_AUTHORITY_ACTIVE);
        static_cast<void>(robotweax_srt_telemetry_recorder_push_member_v1(recorder, &raw, &delta, &m));
    }
    lines = export_v2(recorder, options(backup, member_row));
    check(shape(lines) == "scshsss" && has(lines[2], "\"sampleIndex\":0") && has(lines[2], "\"verdict\":\"ok\"") &&
              has(lines[4], "\"sampleIndex\":1") && has(lines[0], "\"healthCount\":2") &&
              has(lines[0], "\"healthOverwrittenCount\":0") && has(lines[0], "\"overwrittenCount\":3"),
          "carried transition first: " + shape(lines) + " " + lines[0]);

    /* Two more: kept are 5-9; the Watch transition (sample 4) is carried and
     * the OK one before it is overwritten. */
    for (std::uint64_t n = 8; n < 10; ++n)
    {
        const auto raw = raw_at(n);
        const auto delta = delta_at(n);
        static_cast<void>(robotweax_srt_telemetry_recorder_push_member_v1(recorder, &raw, &delta, nullptr));
    }
    lines = export_v2(recorder, options(backup, member_row));
    check(shape(lines) == "scssss" && has(lines[2], "\"verdict\":\"watch\"") && has(lines[2], "\"carried\":true") &&
              has(lines[0], "\"healthCount\":1") && has(lines[0], "\"healthOverwrittenCount\":1"),
          "older transition overwritten: " + shape(lines) + " " + lines[0]);
    check(has(lines[5], "\"member\":{\"state\":\"unknown\",\"standby\":false,\"authority\":null}"),
          "a sample pushed without member state reads unknown: " + lines[5]);

    /* A transition on the first kept sample supersedes the older one there. */
    {
        robotweax_srt_telemetry_recorder *r = nullptr;
        static_cast<void>(robotweax_srt_telemetry_recorder_create(2, &r));
        for (std::uint64_t n = 0; n < 3; ++n)
        {
            const auto raw = raw_at(n);
            const auto delta = delta_at(n);
            static_cast<void>(robotweax_srt_telemetry_recorder_push(r, &raw, &delta));
            const auto s = status_at(n, n == 1 ? ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH : ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK,
                                     ROBOTWEAX_SRT_TELEMETRY_REASON_NONE, false, n);
            if (n < 2)
            {
                static_cast<void>(robotweax_srt_telemetry_recorder_push_health_v1(r, &s));
            }
        }
        lines = export_v2(r, options(ROBOTWEAX_SRT_TELEMETRY_RECORDING_GROUP_NONE,
                                     ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_NONE));
        check(shape(lines) == "shs" && has(lines[2], "\"verdict\":\"watch\"") && has(lines[2], "\"carried\":false") &&
                  has(lines[0], "\"healthOverwrittenCount\":1") && has(lines[0], "\"groupType\":null") &&
                  has(lines[0], "\"groupRow\":null") && has(lines[1], ",\"member\":null}"),
              "superseded on the first kept sample, plain row: " + shape(lines) + " " + lines[0]);
        robotweax_srt_telemetry_recorder_destroy(r);
    }

    /* Transitions superseded before the first kept sample are dropped. */
    {
        robotweax_srt_telemetry_recorder *r = nullptr;
        static_cast<void>(robotweax_srt_telemetry_recorder_create(3, &r));
        for (std::uint64_t n = 0; n < 20; ++n)
        {
            const auto raw = raw_at(n);
            const auto delta = delta_at(n);
            static_cast<void>(robotweax_srt_telemetry_recorder_push(r, &raw, &delta));
            const auto s = status_at(n, n % 2 ? ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH : ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK,
                                     ROBOTWEAX_SRT_TELEMETRY_REASON_NONE, false, n);
            static_cast<void>(robotweax_srt_telemetry_recorder_push_health_v1(r, &s));
        }
        lines = export_v2(r, options(ROBOTWEAX_SRT_TELEMETRY_RECORDING_GROUP_NONE,
                                     ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_NONE));
        check(shape(lines) == "shshsh" && has(lines[0], "\"healthCount\":3") &&
                  has(lines[0], "\"healthOverwrittenCount\":17"),
              "bounded transitions: " + shape(lines) + " " + lines[0]);
        robotweax_srt_telemetry_recorder_clear(r);
        lines = export_v2(r, options(ROBOTWEAX_SRT_TELEMETRY_RECORDING_GROUP_NONE,
                                     ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_NONE));
        check(lines.size() == 1 && has(lines[0], "\"healthCount\":0") && has(lines[0], "\"healthOverwrittenCount\":0"),
              "clear forgets transitions");
        robotweax_srt_telemetry_recorder_destroy(r);
    }

    /* Several transitions on one sample (its source fails): all are written
     * while kept, and a hard limit of 2 x capacity + 2 bounds them. */
    {
        robotweax_srt_telemetry_recorder *r = nullptr;
        static_cast<void>(robotweax_srt_telemetry_recorder_create(3, &r));
        const auto raw = raw_at(0);
        const auto delta = delta_at(0);
        static_cast<void>(robotweax_srt_telemetry_recorder_push(r, &raw, &delta));
        for (std::uint64_t n = 0; n < 3; ++n)
        {
            const auto s = status_at(0, n % 2 ? ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH : ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK,
                                     ROBOTWEAX_SRT_TELEMETRY_REASON_NONE, false, 0);
            static_cast<void>(robotweax_srt_telemetry_recorder_push_health_v1(r, &s));
        }
        lines = export_v2(r, options(ROBOTWEAX_SRT_TELEMETRY_RECORDING_GROUP_NONE,
                                     ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_NONE));
        check(shape(lines) == "shhh" && has(lines[0], "\"healthCount\":3"),
              "several transitions on one sample: " + shape(lines));
        for (std::uint64_t n = 0; n < 50; ++n)
        {
            const auto s = status_at(0, n % 2 ? ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH : ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK,
                                     ROBOTWEAX_SRT_TELEMETRY_REASON_NONE, false, 0);
            static_cast<void>(robotweax_srt_telemetry_recorder_push_health_v1(r, &s));
        }
        lines = export_v2(r, options(ROBOTWEAX_SRT_TELEMETRY_RECORDING_GROUP_NONE,
                                     ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_NONE));
        check(lines.size() == 2 + 8 && has(lines[0], "\"healthCount\":8") &&
                  has(lines[0], "\"healthOverwrittenCount\":45"),
              "hard limit on transitions: " + lines[0]);
        robotweax_srt_telemetry_recorder_destroy(r);
    }

    /* A status no line could carry is refused at push. */
    {
        const auto base = status_at(4, ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK, ROBOTWEAX_SRT_TELEMETRY_REASON_NONE, false, 4);
        auto bad = base;
        bad.verdict = static_cast<robotweax_srt_telemetry_health_verdict>(99);
        check(robotweax_srt_telemetry_recorder_push_health_v1(recorder, &bad) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
              "unknown verdict refused");
        bad = base;
        bad.uncapped_verdict = static_cast<robotweax_srt_telemetry_health_verdict>(99);
        check(robotweax_srt_telemetry_recorder_push_health_v1(recorder, &bad) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
              "unknown uncapped verdict refused");
        bad = base;
        bad.reason = static_cast<robotweax_srt_telemetry_health_reason>(9999);
        check(robotweax_srt_telemetry_recorder_push_health_v1(recorder, &bad) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
              "unknown reason refused");
        bad = base;
        bad.knob = static_cast<robotweax_srt_telemetry_health_knob>(9999);
        check(robotweax_srt_telemetry_recorder_push_health_v1(recorder, &bad) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
              "unknown knob refused");
        bad = base;
        bad.knob_direction = static_cast<robotweax_srt_telemetry_knob_direction>(9);
        check(robotweax_srt_telemetry_recorder_push_health_v1(recorder, &bad) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
              "unknown knob direction refused");
        bad = base;
        bad.knob = ROBOTWEAX_SRT_TELEMETRY_KNOB_LATENCY;
        bad.suggested_value = std::numeric_limits<double>::quiet_NaN();
        check(robotweax_srt_telemetry_recorder_push_health_v1(recorder, &bad) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
              "non-finite suggested value refused");
    }

    /* A group row carries path counts on its health lines. */
    {
        robotweax_srt_telemetry_recorder *r = nullptr;
        static_cast<void>(robotweax_srt_telemetry_recorder_create(4, &r));
        for (std::uint64_t n = 0; n < 3; ++n)
        {
            const auto raw = raw_at(n);
            const auto delta = delta_at(n);
            static_cast<void>(robotweax_srt_telemetry_recorder_push(r, &raw, &delta));
        }
        auto s = status_at(2, ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH, ROBOTWEAX_SRT_TELEMETRY_REASON_NONE, false, 1);
        s.flags = ROBOTWEAX_SRT_TELEMETRY_HEALTH_STATUS_GROUP;
        s.group_usable_paths = 1;
        s.group_expected_paths = 2;
        static_cast<void>(robotweax_srt_telemetry_recorder_push_health_v1(r, &s));
        lines = export_v2(r, options(backup, group_row));
        check(shape(lines) == "sssh" && has(lines[4], "\"usablePaths\":1,\"expectedPaths\":2") &&
                  has(lines[4], "\"verdictSinceUnixNano\":" + std::to_string(wall0 + second / 10)) &&
                  has(lines[0], "\"groupRow\":\"group\"") && has(lines[3], ",\"member\":null}"),
              "group row: " + (lines.size() > 4 ? lines[4] : lines[0]));
        if (!jsonl_dir.empty())
        {
            write_file(jsonl_dir + "/group.jsonl", lines);
        }
        robotweax_srt_telemetry_recorder_destroy(r);
    }

    /* Refusals. */
    robotweax_srt_telemetry_error error = ROBOTWEAX_SRT_TELEMETRY_OK;
    static_cast<void>(export_v2(recorder, options(ROBOTWEAX_SRT_TELEMETRY_RECORDING_GROUP_NONE, member_row), &error));
    check(error == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "a row without a group type is refused");
    static_cast<void>(export_v2(recorder, options(9, member_row), &error));
    check(error == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "an unknown group type is refused");
    {
        auto bad = member(7, false, 0);
        const auto raw = raw_at(1);
        const auto delta = delta_at(1);
        check(robotweax_srt_telemetry_recorder_push_member_v1(recorder, &raw, &delta, &bad) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
              "an unknown member state is refused");
    }
    robotweax_srt_telemetry_recorder_destroy(recorder);

    if (failures != 0)
    {
        std::cerr << failures << " 1alpha2 recording checks failed\n";
        return 1;
    }
    std::cout << "all 1alpha2 recording checks passed\n";
    return 0;
}
