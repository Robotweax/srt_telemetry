// Optional peer metadata (identity and admission handshake, Robotweax SRT
// 0.3.0): validation at the sampler setter and the snapshot's "peer" object.
// With --snapshot, prints a snapshot whose sources cover every state, for
// test_peer_schema.py.
#include <robotweax/srt_telemetry_sampler.h>

#include <cstdint>
#include <cstring>
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
robotweax_srt_telemetry_error sample(void *, std::uint64_t wall, std::uint64_t mono,
                                     robotweax_srt_telemetry_source_output_v1 *out)
{
    out->raw.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    out->raw.observed_at_unix_nano = wall;
    out->raw.sample_monotonic_nano = mono;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}
std::string render(robotweax_srt_telemetry_sampler *sampler)
{
    robotweax_srt_telemetry_render_options_v1 options{};
    robotweax_srt_telemetry_render_options_default(ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON, &options);
    std::vector<char> buffer(1 << 20);
    std::size_t length = 0;
    check(robotweax_srt_telemetry_sampler_render(sampler, &options, buffer.data(), buffer.size(), &length) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "render succeeds");
    return std::string(buffer.data(), length);
}
bool has(const std::string &s, const std::string &part)
{
    return s.find(part) != std::string::npos;
}
/* The "peer" object of the source with this id. */
std::string peer_of(const std::string &json, const std::uint64_t id)
{
    const auto at = json.find("{\"id\":" + std::to_string(id) + ",");
    if (at == std::string::npos)
        return {};
    const auto start = json.find("\"peer\":", at);
    const auto end = json.find("}}", start);
    return start == std::string::npos || end == std::string::npos ? std::string{}
                                                                   : json.substr(start, end + 2 - start);
}

robotweax_srt_telemetry_peer_metadata_v1 blank()
{
    robotweax_srt_telemetry_peer_metadata_v1 m{};
    m.struct_size = sizeof(m);
    m.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    return m;
}
robotweax_srt_telemetry_peer_metadata_v1 identity(const char *name, const char *release)
{
    auto m = blank();
    m.identity_availability = 2;
    if (name != nullptr)
    {
        m.identity_fields |= ROBOTWEAX_SRT_TELEMETRY_PEER_IDENTITY_NAME;
        std::strncpy(m.identity_name, name, sizeof(m.identity_name) - 1);
    }
    if (release != nullptr)
    {
        m.identity_fields |= ROBOTWEAX_SRT_TELEMETRY_PEER_IDENTITY_RELEASE;
        std::strncpy(m.identity_release, release, sizeof(m.identity_release) - 1);
    }
    return m;
}
robotweax_srt_telemetry_peer_metadata_v1 handshake(robotweax_srt_telemetry_peer_metadata_v1 m)
{
    m.handshake_availability = 2;
    m.handshake_srt_version = 0x010507;
    m.handshake_flags = 0x000000bf; /* every known bit but STREAM */
    m.handshake_receiver_delay_milliseconds = 250;
    m.handshake_sender_delay_milliseconds = 120;
    return m;
}
} // namespace

int main(int argc, char **argv)
{
    const bool snapshot_only = argc > 1 && std::string(argv[1]) == "--snapshot";
    robotweax_srt_telemetry_sampler_config_v1 config{};
    robotweax_srt_telemetry_sampler_config_default(&config);
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    check(robotweax_srt_telemetry_sampler_create(&config, &sampler) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "sampler created");
    const auto add = [&]
    {
        robotweax_srt_telemetry_source_config_v1 source{};
        source.struct_size = sizeof(source);
        source.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        source.sample = sample;
        std::uint64_t id = 0;
        check(robotweax_srt_telemetry_sampler_add_source(sampler, &source, &id) == ROBOTWEAX_SRT_TELEMETRY_OK,
              "source added");
        return id;
    };
    const auto set = [&](const std::uint64_t id, const robotweax_srt_telemetry_peer_metadata_v1 *m)
    { return robotweax_srt_telemetry_sampler_set_peer_metadata_v1(sampler, id, m); };

    const auto full = add();      /* identity and handshake */
    const auto name_only = add(); /* identity name, release withheld */
    const auto unknown = add();   /* read, nothing advertised */
    const auto failed = add();    /* read failed */
    const auto none = add();      /* never set */

    auto m = handshake(identity("Robotweax SRT", "0.3.0"));
    check(set(full, &m) == ROBOTWEAX_SRT_TELEMETRY_OK, "full metadata accepted");
    m = identity("Robotweax SRT", nullptr);
    check(set(name_only, &m) == ROBOTWEAX_SRT_TELEMETRY_OK, "name without release accepted");
    m = identity(nullptr, nullptr);
    check(set(unknown, &m) == ROBOTWEAX_SRT_TELEMETRY_OK, "read, nothing advertised accepted");
    m = blank();
    m.identity_availability = 1;
    m.identity_native_result = -1;
    check(set(failed, &m) == ROBOTWEAX_SRT_TELEMETRY_OK, "read failure accepted");

    const auto json = render(sampler);
    if (snapshot_only)
    {
        std::cout << json << '\n';
        robotweax_srt_telemetry_sampler_destroy(sampler);
        return failures == 0 ? 0 : 1;
    }

    check(peer_of(json, full) ==
              "\"peer\":{\"version\":1,\"identity\":{\"availability\":\"available\",\"name\":\"Robotweax SRT\","
              "\"release\":\"0.3.0\"},\"handshake\":{\"availability\":\"available\",\"srt_version\":\"1.5.7\","
              "\"srt_version_raw\":\"0x00010507\",\"flags_raw\":\"0x000000bf\",\"flags\":{\"tsbpd_send\":true,\"tsbpd_receive\":true,\"crypt\":true,"
              "\"tlpktdrop\":true,\"nak_report\":true,\"retransmit_marking\":true,\"stream\":false,"
              "\"packet_filter\":true},\"receiver_delay_seconds\":0.25,\"sender_delay_seconds\":0.12}}",
          "full: " + peer_of(json, full));
    check(peer_of(json, name_only) ==
              "\"peer\":{\"version\":1,\"identity\":{\"availability\":\"available\",\"name\":\"Robotweax SRT\","
              "\"release\":null},\"handshake\":{\"availability\":\"unavailable\"}}",
          "name only: " + peer_of(json, name_only));
    check(has(peer_of(json, unknown), "\"identity\":{\"availability\":\"unknown\"}"),
          "unknown: " + peer_of(json, unknown));
    check(has(peer_of(json, failed), "\"identity\":{\"availability\":\"read_failed\"}"),
          "read failed: " + peer_of(json, failed));
    check(peer_of(json, none) == "\"peer\":{\"version\":1,\"identity\":{\"availability\":\"unavailable\"},"
                                 "\"handshake\":{\"availability\":\"unavailable\"}}",
          "never set: " + peer_of(json, none));

    /* Refusals: nothing the provider could not have produced is kept. */
    struct Bad
    {
        const char *what;
        robotweax_srt_telemetry_peer_metadata_v1 m;
    };
    std::vector<Bad> bad;
    const auto variant = [&](const char *what, auto change)
    {
        auto candidate = handshake(identity("Robotweax SRT", "0.3.0"));
        change(candidate);
        bad.push_back({what, candidate});
    };
    variant("old abi", [](auto &c) { c.abi_version = 99; });
    variant("short struct", [](auto &c) { c.struct_size = 8; });
    variant("availability 3", [](auto &c) { c.identity_availability = 3; });
    variant("handshake availability 1", [](auto &c) { c.handshake_availability = 1; });
    variant("unknown field bit", [](auto &c) { c.identity_fields |= 4; });
    variant("release without name",
            [](auto &c)
            {
                c.identity_fields = ROBOTWEAX_SRT_TELEMETRY_PEER_IDENTITY_RELEASE;
                std::memset(c.identity_name, 0, sizeof(c.identity_name));
            });
    variant("name flag, empty name", [](auto &c) { std::memset(c.identity_name, 0, sizeof(c.identity_name)); });
    variant("text without flag",
            [](auto &c) { c.identity_fields = ROBOTWEAX_SRT_TELEMETRY_PEER_IDENTITY_NAME; });
    variant("name with a quote", [](auto &c) { c.identity_name[3] = '"'; });
    variant("name with a control byte", [](auto &c) { c.identity_name[3] = '\n'; });
    variant("name starting with a space", [](auto &c) { c.identity_name[0] = ' '; });
    variant("name ending with a space",
            [](auto &c)
            {
                std::memset(c.identity_name, 0, sizeof(c.identity_name));
                std::strcpy(c.identity_name, "Robotweax ");
            });
    variant("space in the release", [](auto &c) { c.identity_release[1] = ' '; });
    variant("name without terminator", [](auto &c) { std::memset(c.identity_name, 'a', sizeof(c.identity_name)); });
    variant("bytes after the terminator", [](auto &c) { c.identity_name[20] = 'x'; });
    variant("failed read with fields",
            [](auto &c)
            {
                c.identity_availability = 1;
                c.identity_native_result = -1;
            });
    variant("read with a native error", [](auto &c) { c.identity_native_result = -1; });
    variant("handshake values without availability", [](auto &c) { c.handshake_availability = 0; });
    variant("padding", [](auto &c) { c.padding[1] = 1; });
    variant("reserved", [](auto &c) { c.reserved[0] = 1; });
    for (const auto &b : bad)
    {
        check(set(full, &b.m) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, std::string("refused: ") + b.what);
    }
    check(has(peer_of(render(sampler), full), "\"name\":\"Robotweax SRT\""), "a refusal keeps the last metadata");

    /* Longest values SRT allows. */
    m = identity("A2345678901234567890123456789012", "B234567890123456789012345678901234567890123456789012345678901234");
    check(set(full, &m) == ROBOTWEAX_SRT_TELEMETRY_OK, "32-byte name and 64-byte release accepted");

    /* SRT passes the advertised word on unvalidated: not dotted unless well formed. */
    m = handshake(identity(nullptr, nullptr));
    m.handshake_srt_version = 0x695d5a39;
    check(set(full, &m) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              has(peer_of(render(sampler), full), "\"srt_version\":null,\"srt_version_raw\":\"0x695d5a39\""),
          "an ill-formed version stays raw: " + peer_of(render(sampler), full));

    check(set(full, nullptr) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              has(peer_of(render(sampler), full), "\"identity\":{\"availability\":\"unavailable\"}"),
          "NULL clears");
    check(set(9999, &m) == ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND, "unknown source");
    check(robotweax_srt_telemetry_sampler_set_peer_metadata_v1(nullptr, full, &m) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "NULL sampler");
    check(robotweax_srt_telemetry_sampler_remove_source(sampler, name_only) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              set(name_only, &m) == ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND,
          "removal forgets the source");

    robotweax_srt_telemetry_sampler_destroy(sampler);
    if (failures != 0)
    {
        std::cerr << failures << " peer metadata check(s) failed\n";
        return 1;
    }
    std::cout << "peer metadata checks passed\n";
    return 0;
}
