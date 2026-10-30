// Source connection identity and the exported observer: validation at the
// sampler setters, the getters, and the JSON snapshot's "observer",
// "connection_seq", "connected_at_unix_ms" and "session_ref".
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

constexpr std::uint64_t base_unix = 1'800'000'000'000'000'000ull;

/* Reports a connection that is 5 s old. */
robotweax_srt_telemetry_error sample(void *, std::uint64_t wall, std::uint64_t mono,
                                     robotweax_srt_telemetry_source_output_v1 *out)
{
    out->raw.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    out->raw.observed_at_unix_nano = wall;
    out->raw.sample_monotonic_nano = mono;
    out->has_ext = 1;
    out->ext.available_fields = ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_CONNECTION_AGE_SECONDS;
    out->ext.connection_age_seconds = 5.0;
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
/* The text of the source with this id, up to the next source. */
std::string source_of(const std::string &json, const std::uint64_t id)
{
    const auto at = json.find("{\"id\":" + std::to_string(id) + ",");
    if (at == std::string::npos)
        return {};
    const auto end = json.find("{\"id\":", at + 1);
    return json.substr(at, end == std::string::npos ? std::string::npos : end - at);
}
robotweax_srt_telemetry_source_identity_v1 identity(const std::uint64_t seq, const char *session_ref)
{
    robotweax_srt_telemetry_source_identity_v1 value{};
    value.struct_size = sizeof(value);
    value.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    value.connection_seq = seq;
    if (session_ref != nullptr)
        std::strncpy(value.session_ref, session_ref, sizeof(value.session_ref) - 1);
    return value;
}
robotweax_srt_telemetry_observer_v1 observer(const char *node, const char *instance)
{
    robotweax_srt_telemetry_observer_v1 value{};
    value.struct_size = sizeof(value);
    value.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    if (node != nullptr)
        std::strncpy(value.node, node, sizeof(value.node) - 1);
    if (instance != nullptr)
        std::strncpy(value.instance, instance, sizeof(value.instance) - 1);
    return value;
}
/* From inside a sampler callback every new function refuses. */
struct Reentry
{
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    bool called = false;
    bool refused = true;
};
void on_sample_reentry(void *user_data, const robotweax_srt_telemetry_source_info_v1 *source,
                       const robotweax_srt_telemetry_source_output_v1 *, const robotweax_srt_telemetry_metric_sample_v1 *)
{
    auto &reentry = *static_cast<Reentry *>(user_data);
    reentry.called = true;
    auto value = identity(1, "x");
    auto node = observer("n", "i");
    reentry.refused =
        robotweax_srt_telemetry_sampler_set_source_identity_v1(reentry.sampler, source->source_id, &value) ==
            ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE &&
        robotweax_srt_telemetry_sampler_source_identity_v1(reentry.sampler, source->source_id, &value) ==
            ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE &&
        robotweax_srt_telemetry_sampler_set_observer_v1(reentry.sampler, &node) ==
            ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE &&
        robotweax_srt_telemetry_sampler_observer_v1(reentry.sampler, &node) == ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
}
void test_callback_reentry()
{
    Reentry reentry;
    robotweax_srt_telemetry_sampler_config_v1 config{};
    robotweax_srt_telemetry_sampler_config_default(&config);
    config.callbacks.user_data = &reentry;
    config.callbacks.on_sample = on_sample_reentry;
    check(robotweax_srt_telemetry_sampler_create(&config, &reentry.sampler) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "reentry sampler created");
    robotweax_srt_telemetry_source_config_v1 source{};
    source.struct_size = sizeof(source);
    source.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    source.sample = sample;
    std::uint64_t id = 0;
    check(robotweax_srt_telemetry_sampler_add_source(reentry.sampler, &source, &id) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_sampler_tick(reentry.sampler, base_unix, 1'000'000'000) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK,
          "reentry tick");
    check(reentry.called && reentry.refused, "identity and observer calls refused inside a callback");
    robotweax_srt_telemetry_sampler_destroy(reentry.sampler);
}
} // namespace

int main()
{
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
    const auto connected = add(); /* connected, identity set */
    const auto plain = add();     /* no context, no identity */

    robotweax_srt_telemetry_health_context_v1 context{};
    robotweax_srt_telemetry_health_context_init(&context);
    context.connection = ROBOTWEAX_SRT_TELEMETRY_CONNECTION_CONNECTED;
    check(robotweax_srt_telemetry_sampler_set_health_context(sampler, connected, &context) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "context set");
    check(robotweax_srt_telemetry_sampler_tick(sampler, base_unix, 1'000'000'000) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "tick");

    /* Nothing set: every field present and null. */
    auto json = render(sampler);
    check(has(json, ",\"observer\":null,\"sources\":["), "no observer exported: null");
    check(has(source_of(json, plain), "\"connection_seq\":null,\"connected_at_unix_ms\":null,\"session_ref\":null"),
          "no identity, not connected: all null");
    check(has(source_of(json, connected), "\"connection_seq\":null,\"connected_at_unix_ms\":" +
                                              std::to_string((base_unix - 5'000'000'000ull) / 1'000'000) +
                                              ",\"session_ref\":null"),
          "a connected source's start from its connection age");

    /* Identity: set, read back, rendered with JSON escaping. */
    auto value = identity(3, "sha256:6b1f\"c2e\\");
    check(robotweax_srt_telemetry_sampler_set_source_identity_v1(sampler, connected, &value) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "identity set");
    auto read = identity(0, nullptr);
    check(robotweax_srt_telemetry_sampler_source_identity_v1(sampler, connected, &read) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              read.connection_seq == 3 && std::string(read.session_ref) == "sha256:6b1f\"c2e\\",
          "identity read back");
    json = render(sampler);
    check(has(source_of(json, connected), "\"connection_seq\":3,") &&
              has(source_of(json, connected), "\"session_ref\":\"sha256:6b1f\\\"c2e\\\\\""),
          "identity rendered and escaped");
    check(has(source_of(json, plain), "\"connection_seq\":null"), "another source keeps none");

    /* A sequence without a session reference. */
    value = identity(4, nullptr);
    check(robotweax_srt_telemetry_sampler_set_source_identity_v1(sampler, connected, &value) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              has(source_of(render(sampler), connected), "\"connection_seq\":4,") &&
              has(source_of(render(sampler), connected), "\"session_ref\":null"),
          "sequence only");

    /* Invalid identities are refused and change nothing. */
    auto bad = identity(5, "ok");
    bad.session_ref[3] = 'x'; /* text after the terminator */
    check(robotweax_srt_telemetry_sampler_set_source_identity_v1(sampler, connected, &bad) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "bytes after the terminator refused");
    bad = identity(5, "tab\there");
    check(robotweax_srt_telemetry_sampler_set_source_identity_v1(sampler, connected, &bad) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "control character refused");
    bad = identity(5, nullptr);
    std::memset(bad.session_ref, 'a', sizeof(bad.session_ref));
    check(robotweax_srt_telemetry_sampler_set_source_identity_v1(sampler, connected, &bad) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "unterminated session reference refused");
    bad = identity(5, nullptr);
    bad.padding[0] = 1;
    check(robotweax_srt_telemetry_sampler_set_source_identity_v1(sampler, connected, &bad) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "padding refused");
    bad = identity(5, nullptr);
    bad.reserved[1] = 1;
    check(robotweax_srt_telemetry_sampler_set_source_identity_v1(sampler, connected, &bad) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "reserved refused");
    bad = identity(5, nullptr);
    bad.struct_size = 8;
    check(robotweax_srt_telemetry_sampler_set_source_identity_v1(sampler, connected, &bad) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "short struct refused");
    std::string longest(ROBOTWEAX_SRT_TELEMETRY_SOURCE_SESSION_REF_CAPACITY, 'r');
    value = identity(5, longest.c_str());
    check(robotweax_srt_telemetry_sampler_set_source_identity_v1(sampler, connected, &value) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "a 256-byte session reference is accepted");
    check(robotweax_srt_telemetry_sampler_set_source_identity_v1(sampler, 99, &value) ==
              ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND &&
              robotweax_srt_telemetry_sampler_source_identity_v1(sampler, 99, &read) ==
                  ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND,
          "unknown source");
    check(robotweax_srt_telemetry_sampler_set_source_identity_v1(nullptr, connected, &value) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT &&
              robotweax_srt_telemetry_sampler_source_identity_v1(sampler, connected, nullptr) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "NULL refused");
    read.abi_version = 0;
    check(robotweax_srt_telemetry_sampler_source_identity_v1(sampler, connected, &read) ==
              ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH,
          "getter checks the caller's struct");

    /* NULL clears. */
    check(robotweax_srt_telemetry_sampler_set_source_identity_v1(sampler, connected, nullptr) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              has(source_of(render(sampler), connected), "\"connection_seq\":null"),
          "NULL clears the identity");
    read = identity(9, "x");
    check(robotweax_srt_telemetry_sampler_source_identity_v1(sampler, connected, &read) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              read.connection_seq == 0 && read.session_ref[0] == 0 && read.session_ref[1] == 0,
          "a cleared identity reads as all zero");

    /* A lost connection has no start. */
    context.connection = ROBOTWEAX_SRT_TELEMETRY_CONNECTION_LOST;
    check(robotweax_srt_telemetry_sampler_set_health_context(sampler, connected, &context) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              has(source_of(render(sampler), connected), "\"connected_at_unix_ms\":null"),
          "no start while lost");

    /* Observer: exported only once set. */
    robotweax_srt_telemetry_observer_v1 seen = observer(nullptr, nullptr);
    check(robotweax_srt_telemetry_sampler_observer_v1(sampler, &seen) == ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND,
          "no observer before it is set");
    auto node = observer("edge-fra-2", "7f3a.9");
    check(robotweax_srt_telemetry_sampler_set_observer_v1(sampler, &node) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              has(render(sampler), ",\"observer\":{\"node\":\"edge-fra-2\",\"instance\":\"7f3a.9\"},\"sources\":["),
          "observer exported");
    check(robotweax_srt_telemetry_sampler_observer_v1(sampler, &seen) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              std::string(seen.node) == "edge-fra-2" && std::string(seen.instance) == "7f3a.9",
          "observer read back");
    node = observer(nullptr, "only-instance");
    check(robotweax_srt_telemetry_sampler_set_observer_v1(sampler, &node) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              has(render(sampler), "\"observer\":{\"node\":null,\"instance\":\"only-instance\"}"),
          "an unknown node is null");
    for (const char *wrong : {"null", "a b", "edge/1", "é"})
    {
        auto bad_observer = observer(wrong, "i");
        check(robotweax_srt_telemetry_sampler_set_observer_v1(sampler, &bad_observer) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
              std::string("observer node refused: ") + wrong);
    }
    auto bad_observer = observer("n", nullptr);
    std::memset(bad_observer.instance, 'a', sizeof(bad_observer.instance));
    check(robotweax_srt_telemetry_sampler_set_observer_v1(sampler, &bad_observer) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "unterminated instance refused");
    bad_observer = observer("n", "i");
    bad_observer.padding[5] = 1;
    check(robotweax_srt_telemetry_sampler_set_observer_v1(sampler, &bad_observer) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "observer padding refused");
    check(has(render(sampler), "\"instance\":\"only-instance\""), "a refused observer changes nothing");
    check(robotweax_srt_telemetry_sampler_set_observer_v1(sampler, nullptr) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              has(render(sampler), ",\"observer\":null,") &&
              robotweax_srt_telemetry_sampler_observer_v1(sampler, &seen) == ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND,
          "NULL stops exporting the observer");

    /* Removal forgets the identity. */
    value = identity(7, "gone");
    check(robotweax_srt_telemetry_sampler_set_source_identity_v1(sampler, plain, &value) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_sampler_remove_source(sampler, plain) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_sampler_source_identity_v1(sampler, plain, &read) ==
                  ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND,
          "removal forgets the identity");

    robotweax_srt_telemetry_sampler_destroy(sampler);
    test_callback_reentry();
    if (failures != 0)
    {
        std::cerr << failures << " source identity checks failed\n";
        return 1;
    }
    std::cout << "all source identity checks passed\n";
    return 0;
}
