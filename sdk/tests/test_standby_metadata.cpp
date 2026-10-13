#include <chrono>
#include <cstdint>
#include <iostream>
#include <robotweax/srt_telemetry_sampler.h>
#include <string>
#include <thread>

namespace
{
int failures = 0;
void check(bool pass, const char *message)
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
std::string render(robotweax_srt_telemetry_sampler *sampler,
                   robotweax_srt_telemetry_render_format format)
{
    robotweax_srt_telemetry_render_options_v1 options{};
    robotweax_srt_telemetry_render_options_default(format, &options);
    char buffer[65536];
    std::size_t length = 0;
    check(robotweax_srt_telemetry_sampler_render(sampler, &options, buffer, sizeof(buffer),
                                                 &length) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "render succeeds");
    return std::string(buffer, length);
}
bool has(const std::string &s, const std::string &part)
{
    return s.find(part) != std::string::npos;
}
} // namespace
int main(int argc, char **argv)
{
    robotweax_srt_telemetry_sampler_config_v1 config{};
    robotweax_srt_telemetry_sampler_config_default(&config);
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    check(robotweax_srt_telemetry_sampler_create(&config, &sampler) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "sampler created");
    robotweax_srt_telemetry_source_config_v1 source{};
    source.struct_size = sizeof(source);
    source.abi_version = 1;
    source.sample = sample;
    std::uint64_t id = 0;
    check(robotweax_srt_telemetry_sampler_add_source(sampler, &source, &id) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "source created");
    auto json = [&] { return render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON); };
    check(has(json(), "\"standby_rtt\":{\"version\":1,\"availability\":\"unavailable\"}"),
          "default does not infer provider support");
    robotweax_srt_telemetry_standby_rtt_metadata_v1 m{};
    m.struct_size = sizeof(m);
    m.abi_version = 1;
    m.availability = 2;
    m.age_microseconds = UINT64_MAX;
    const auto set = [&]
    { return robotweax_srt_telemetry_sampler_set_standby_rtt_metadata_v1(sampler, id, &m); };
    check(set() == ROBOTWEAX_SRT_TELEMETRY_OK && has(json(), "\"rtt_seconds\":null"),
          "disabled getter has no zero measurement");
    m.flags = 3;
    m.outcome = 1;
    check(set() == ROBOTWEAX_SRT_TELEMETRY_OK && has(json(), "\"observed\":false"),
          "pending no sample");
    const auto prom_before = render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS);
    m.flags = 15;
    m.outcome = 2;
    m.rtt_microseconds = 25'000;
    m.age_microseconds = 42;
    m.samples = UINT64_MAX;
    m.sampled_monotonic_nano = UINT64_MAX;
    check(set() == ROBOTWEAX_SRT_TELEMETRY_OK, "measured accepted");
    const auto measured = json();
    check(has(measured, "\"rtt_seconds\":0.025") &&
              has(measured, "\"samples\":\"18446744073709551615\"") &&
              has(measured, "\"last_outcome\":\"measured\"") && has(measured, "\"fresh\":true"),
          "JSON uses seconds, exact uint64 counts and last outcome");
    check(prom_before == render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS),
          "standby observation never enters metric windows/Prometheus");
    check(!has(render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_OTLP_JSON), "standby_rtt"),
          "standby observation never enters OTLP metrics");
    m.age_microseconds = 3'000'000;
    check(set() == ROBOTWEAX_SRT_TELEMETRY_OK, "fresh at boundary accepted");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(has(json(), "\"fresh_at_capture\":true,\"fresh\":false"),
          "render-time ageing cannot retain stale freshness when sampling stops");
    m.flags = 23;
    m.age_microseconds = 8'000'000;
    m.outcome = 5;
    check(set() == ROBOTWEAX_SRT_TELEMETRY_OK && has(json(), "\"terminal\":true") &&
              has(json(), "\"last_outcome\":\"paused\"") && has(json(), "\"rtt_seconds\":0.025"),
          "terminal stale history survives cancelled attempt");
    auto good = m;
    m.flags = 31;
    check(set() == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "inconsistent freshness rejected");
    m = good;
    m.reserved[0] = 1;
    check(set() == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "reserved metadata rejected");
    m = good;
    m.availability = 1;
    check(set() == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "failed read cannot carry old sample");
    m = {};
    m.struct_size = sizeof(m);
    m.abi_version = 1;
    m.availability = 1;
    m.native_result = -1;
    m.age_microseconds = UINT64_MAX;
    check(set() == ROBOTWEAX_SRT_TELEMETRY_OK &&
              has(json(), "\"standby_rtt\":{\"version\":1,\"availability\":\"read_failed\"}"),
          "failure clears old value and age");
    check(robotweax_srt_telemetry_sampler_set_standby_rtt_metadata_v1(sampler, id, nullptr) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "disable clears metadata");
    check(robotweax_srt_telemetry_sampler_remove_source(sampler, id) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "remove source");
    check(set() == ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND, "removed ID no longer readable");
    check(robotweax_srt_telemetry_sampler_add_source(sampler, &source, &id) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              has(json(), "\"standby_rtt\":{\"version\":1,\"availability\":\"unavailable\"}"),
          "new source does not inherit previous history");
    if (argc > 1 && std::string(argv[1]) == "--snapshot")
    {
        m = good;
        check(set() == ROBOTWEAX_SRT_TELEMETRY_OK, "snapshot");
        std::cout << json();
    }
    robotweax_srt_telemetry_sampler_destroy(sampler);
    return failures ? 1 : 0;
}
