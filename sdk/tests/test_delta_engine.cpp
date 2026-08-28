#include <robotweax/srt_telemetry.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

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

robotweax_srt_telemetry_raw_sample_v1 sample_at(const uint64_t monotonic_nano)
{
    robotweax_srt_telemetry_raw_sample_v1 sample{};
    sample.struct_size = sizeof(sample);
    sample.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    sample.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    sample.capabilities = ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1;
    sample.sample_monotonic_nano = monotonic_nano;
    sample.observed_at_unix_nano = 1'800'000'000'000'000'000ULL + monotonic_nano;
    return sample;
}

void test_first_sample_and_rates()
{
    robotweax::srt::telemetry::DeltaEngine engine;
    auto first = sample_at(1'000'000'000ULL);
    first.available_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL |
                             ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL |
                             ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL;
    first.sent_packet_total = 100;
    first.sent_wire_byte_total = 10'000;
    first.unique_sent_wire_byte_total = 9'000;

    const auto first_delta = engine.update(first);
    check((first_delta.quality_flags & ROBOTWEAX_SRT_TELEMETRY_QUALITY_FIRST_SAMPLE) != 0,
          "the first observation is marked as a baseline");
    check(first_delta.available_delta_fields == 0, "no first-sample delta");

    auto second = first;
    second.sample_monotonic_nano += 1'000'000'000ULL;
    second.sent_packet_total += 10;
    second.sent_wire_byte_total += 1'000;
    second.unique_sent_wire_byte_total += 800;

    const auto delta = engine.update(second);
    check(delta.interval_nano == 1'000'000'000ULL, "one-second interval");
    check(delta.sent_packet_delta == 10, "packet delta");
    check(delta.sent_wire_byte_delta == 1'000, "wire-byte delta");
    check(std::abs(delta.sent_wire_bits_per_second - 8'000.0) < 0.001,
          "physical wire rate is derived from lifetime bytes");
    check(std::abs(delta.unique_sent_wire_bits_per_second - 6'400.0) < 0.001,
          "unique wire rate remains distinct from physical wire rate");
}

void test_missing_is_not_zero()
{
    robotweax::srt::telemetry::DeltaEngine engine;
    auto first = sample_at(1'000);
    first.available_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL;
    first.sent_packet_total = 4;
    static_cast<void>(engine.update(first));

    auto second = first;
    second.sample_monotonic_nano = 2'000;
    second.sent_packet_total = 5;
    second.receiver_loss_packet_total = 0;
    const auto delta = engine.update(second);

    check((delta.available_delta_fields & ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL) != 0,
          "available counter has a delta");
    check((delta.available_delta_fields &
           ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_LOSS_PACKET_TOTAL) == 0,
          "an unavailable zero is not exported as an observed zero");
}

void test_counter_reset_is_field_local()
{
    robotweax::srt::telemetry::DeltaEngine engine;
    auto first = sample_at(10'000);
    first.available_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL |
                             ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_PACKET_TOTAL;
    first.sent_packet_total = 100;
    first.received_packet_total = 200;
    static_cast<void>(engine.update(first));

    auto second = first;
    second.sample_monotonic_nano = 20'000;
    second.sent_packet_total = 2;
    second.received_packet_total = 205;
    const auto delta = engine.update(second);

    check((delta.counter_reset_fields & ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL) != 0,
          "decreasing counter is marked reset");
    check((delta.available_delta_fields & ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL) == 0,
          "reset counter does not produce an invalid delta");
    check(delta.received_packet_delta == 5, "unaffected counter still advances");
}

void test_clock_and_provider_discontinuities()
{
    robotweax::srt::telemetry::DeltaEngine engine;
    auto first = sample_at(20'000);
    static_cast<void>(engine.update(first));

    auto regressed = first;
    regressed.sample_monotonic_nano = 19'000;
    auto delta = engine.update(regressed);
    check((delta.quality_flags & ROBOTWEAX_SRT_TELEMETRY_QUALITY_MONOTONIC_CLOCK_REGRESSION) != 0,
          "clock regression creates a new baseline");

    auto changed = sample_at(30'000);
    changed.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
    delta = engine.update(changed);
    check((delta.quality_flags & ROBOTWEAX_SRT_TELEMETRY_QUALITY_PROVIDER_CHANGED) != 0,
          "provider change creates a new baseline");
}

void test_bounded_recorder_overwrite_order()
{
    robotweax::srt::telemetry::DeltaEngine engine;
    robotweax::srt::telemetry::BoundedRecorder recorder(2);
    check(recorder.capacity() == 2, "recorder preallocates requested capacity");

    for (std::uint64_t index = 1; index <= 3; ++index)
    {
        auto raw = sample_at(index * 1'000);
        raw.available_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL;
        raw.sent_packet_total = index;
        const auto delta = engine.update(raw);
        recorder.push(raw, delta);
    }

    check(recorder.size() == 2, "recorder size remains bounded");
    check(recorder.overwritten_count() == 1, "overwrite is observable");
    check(recorder.read(0).raw.sent_packet_total == 2, "oldest retained record is second");
    check(recorder.read(1).raw.sent_packet_total == 3, "newest retained record is third");

    recorder.clear();
    check(recorder.size() == 0, "clear removes retained records");
    check(recorder.overwritten_count() == 0, "clear resets overwrite accounting");
}

} // namespace

int main()
{
    test_first_sample_and_rates();
    test_missing_is_not_zero();
    test_counter_reset_is_field_local();
    test_clock_and_provider_discontinuities();
    test_bounded_recorder_overwrite_order();
    if (failures != 0)
    {
        std::cerr << failures << " test checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all telemetry delta-engine checks passed\n";
    return EXIT_SUCCESS;
}
