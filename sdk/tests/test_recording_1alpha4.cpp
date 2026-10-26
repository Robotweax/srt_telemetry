/* Recording schema 1alpha4 (srt_telemetry#60): 1alpha3 plus the observer
 * node and process instance the host supplies. With --jsonl DIR the test
 * writes recordings with and without an observer for test_recording_schema.py. */

#include <robotweax/srt_telemetry.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
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

int collect(void *user, const char *line, const std::size_t size)
{
    static_cast<std::vector<std::string> *>(user)->emplace_back(line, size);
    return 0;
}

constexpr std::uint64_t second = 1'000'000'000;

void push(robotweax_srt_telemetry_recorder *recorder, const std::uint64_t n)
{
    robotweax_srt_telemetry_raw_sample_v1 raw{};
    raw.struct_size = sizeof(raw);
    raw.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    raw.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    raw.observed_at_unix_nano = 1'800'000'000 * second + n * second / 10;
    raw.sample_monotonic_nano = 5 * second + n * second / 10;
    robotweax_srt_telemetry_delta_v1 delta{};
    delta.struct_size = sizeof(delta);
    delta.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    robotweax_srt_telemetry_recorder_connection_v1 connection{};
    connection.struct_size = sizeof(connection);
    connection.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    connection.available = 1;
    connection.connection_age_nano = second + n * second / 10;
    check(robotweax_srt_telemetry_recorder_push_sample_v1(recorder, &raw, &delta, nullptr, &connection) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "push");
}

robotweax_srt_telemetry_jsonl_export_v4_options options(const char *node, const char *instance)
{
    robotweax_srt_telemetry_jsonl_export_v4_options o{};
    o.struct_size = sizeof(o);
    o.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    o.group_id = -1;
    o.peer_group_id = -1;
    o.session_ref = "sess-1";
    o.observer_node = node;
    o.observer_instance = instance;
    return o;
}

std::vector<std::string> export_v4(robotweax_srt_telemetry_recorder *recorder,
                                   const robotweax_srt_telemetry_jsonl_export_v4_options &o,
                                   robotweax_srt_telemetry_error *error = nullptr)
{
    robotweax_srt_telemetry_jsonl_export_config_v1 c{};
    c.struct_size = sizeof(c);
    c.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    c.stream_id = "cam-9";
    c.flow_id = "live/cam-9#7";
    c.provider_version = "0.3.1";
    c.provider_revision = "test";
    c.build_profile = "test";
    c.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;
    c.configured_provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    std::vector<std::string> lines;
    const auto e = robotweax_srt_telemetry_recorder_export_jsonl_v4(recorder, &c, &o, collect, &lines);
    if (error != nullptr)
        *error = e;
    else
        check(e == ROBOTWEAX_SRT_TELEMETRY_OK, "export_jsonl_v4 succeeds");
    return lines;
}

void write_file(const std::string &path, const std::vector<std::string> &lines)
{
    std::ofstream out(path, std::ios::binary);
    for (const auto &line : lines)
        out << line;
}
} // namespace

int main(int argc, char **argv)
{
    const std::string jsonl_dir = argc > 2 && std::string(argv[1]) == "--jsonl" ? argv[2] : "";
    robotweax_srt_telemetry_recorder *recorder = nullptr;
    check(robotweax_srt_telemetry_recorder_create(8, &recorder) == ROBOTWEAX_SRT_TELEMETRY_OK, "create");
    for (std::uint64_t n = 0; n < 3; ++n)
        push(recorder, n);

    /* Both given: written as given, beside every 1alpha3 field. */
    auto lines = export_v4(recorder, options("edge-fra-2", "4f1c0a9e7b2d4c31a0e9f8d7c6b5a493"));
    check(lines.size() == 4 && field(lines[0], "schemaVersion") == "\"1alpha4\"" &&
              field(lines[0], "observerNode") == "\"edge-fra-2\"" &&
              field(lines[0], "observerInstance") == "\"4f1c0a9e7b2d4c31a0e9f8d7c6b5a493\"" &&
              field(lines[0], "sessionRef") == "\"sess-1\"" && field(lines[0], "flowId") == "\"live/cam-9#7\"" &&
              field(lines[0], "connectedAtUnixNano") != "null",
          "1alpha4 header with an observer: " + lines[0]);
    check(field(lines[1], "schemaVersion") == "\"1alpha4\"" && field(lines[1], "connectionAgeNano") != "null",
          "1alpha4 samples are 1alpha3 samples: " + lines[1]);
    if (!jsonl_dir.empty())
        write_file(jsonl_dir + "/observer.jsonl", lines);

    /* A process instance without a known node: the node stays null. */
    lines = export_v4(recorder, options(nullptr, "4f1c0a9e7b2d4c31a0e9f8d7c6b5a493"));
    check(field(lines[0], "observerNode") == "null" &&
              field(lines[0], "observerInstance") == "\"4f1c0a9e7b2d4c31a0e9f8d7c6b5a493\"",
          "node unknown, instance known: " + lines[0]);
    lines = export_v4(recorder, options(nullptr, nullptr));
    check(field(lines[0], "observerNode") == "null" && field(lines[0], "observerInstance") == "null",
          "neither known: both null: " + lines[0]);
    if (!jsonl_dir.empty())
        write_file(jsonl_dir + "/unknown.jsonl", lines);

    /* Refused, never shortened: empty, "null", outside the grammar, too long. */
    const std::string long_value(65, 'n');
    const std::string max_value(64, 'n');
    for (const char *bad : {"", "null", "edge fra", "edge/fra", "kn\xc3\xb6ten", long_value.c_str()})
    {
        robotweax_srt_telemetry_error error = ROBOTWEAX_SRT_TELEMETRY_OK;
        auto out = export_v4(recorder, options(bad, nullptr), &error);
        check(error == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT && out.empty(),
              std::string("observer node refused: ") + bad);
        out = export_v4(recorder, options(nullptr, bad), &error);
        check(error == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT && out.empty(),
              std::string("observer instance refused: ") + bad);
    }
    robotweax_srt_telemetry_error error = ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    lines = export_v4(recorder, options(max_value.c_str(), "a.b_c-1"), &error);
    check(error == ROBOTWEAX_SRT_TELEMETRY_OK && field(lines[0], "observerNode") == "\"" + max_value + "\"",
          "64 bytes and the grammar's punctuation are accepted");
    /* The 1alpha3 rules still apply. */
    auto bad_group = options("edge-fra-2", nullptr);
    bad_group.group_id = 5;
    static_cast<void>(export_v4(recorder, bad_group, &error));
    check(error == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "a plain socket has no group ID in 1alpha4 either");
    auto short_struct = options("edge-fra-2", nullptr);
    short_struct.struct_size = sizeof(robotweax_srt_telemetry_jsonl_export_v3_options);
    static_cast<void>(export_v4(recorder, short_struct, &error));
    check(error == ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH, "v3 options passed as v4 are an ABI mismatch");
    robotweax_srt_telemetry_recorder_destroy(recorder);

    if (failures != 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "recording 1alpha4 tests passed\n";
    return 0;
}
