#include <robotweax/srt_telemetry_metrics.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
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

bool near(const double actual, const double expected)
{
    return std::abs(actual - expected) <= 1e-9 * std::max(1.0, std::abs(expected));
}

/* p95 values are kept as 32-bit floats (about 7 significant digits). */
bool near_float(const double actual, const double expected)
{
    return std::abs(actual - expected) <= 1e-6 * std::max(1.0, std::abs(expected));
}

constexpr std::uint64_t second = UINT64_C(1'000'000'000);
constexpr std::uint64_t base_unix = UINT64_C(1'800'000'000) * second;

std::uint64_t bit(const robotweax_srt_telemetry_metric metric)
{
    return UINT64_C(1) << static_cast<unsigned>(metric);
}

robotweax_srt_telemetry_raw_sample_v1 raw_at(const std::uint64_t unix_nano)
{
    robotweax_srt_telemetry_raw_sample_v1 raw{};
    raw.struct_size = sizeof(raw);
    raw.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    raw.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
    raw.observed_at_unix_nano = unix_nano;
    raw.sample_monotonic_nano = unix_nano - base_unix + second;
    return raw;
}

robotweax_srt_telemetry_delta_v1 delta_of(const std::uint64_t interval)
{
    robotweax_srt_telemetry_delta_v1 delta{};
    delta.struct_size = sizeof(delta);
    delta.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    delta.interval_nano = interval;
    return delta;
}

robotweax_srt_telemetry_metric_sample_v1 empty_metrics()
{
    robotweax_srt_telemetry_metric_sample_v1 metrics{};
    metrics.struct_size = sizeof(metrics);
    metrics.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    return metrics;
}

void test_catalog()
{
    for (int index = 0; index < ROBOTWEAX_SRT_TELEMETRY_METRIC_DEFINED_COUNT; ++index)
    {
        const auto *info =
            robotweax_srt_telemetry_metric_info(static_cast<robotweax_srt_telemetry_metric>(index));
        check(info != nullptr && info->name != nullptr && info->unit != nullptr &&
                  std::strlen(info->name) > 0,
              "every defined metric has a name and unit");
    }
    check(robotweax_srt_telemetry_metric_info(ROBOTWEAX_SRT_TELEMETRY_METRIC_DEFINED_COUNT) ==
              nullptr,
          "no info past the defined metrics");
    check(std::string_view(
              robotweax_srt_telemetry_metric_info(ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS)->name) ==
              "rtt_seconds",
          "stable metric names");
}

void test_compute()
{
    auto raw = raw_at(base_unix);
    raw.available_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_SECONDS |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_BYTES |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_TSBPD_DELAY_SECONDS |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_FLIGHT_SIZE_PACKETS |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_FLOW_WINDOW_PACKETS |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_SECONDS |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_BYTES |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_TSBPD_DELAY_SECONDS;
    raw.rtt_seconds = 0.02;
    raw.receive_buffer_seconds = 0.06;
    raw.receive_buffer_bytes = 50'000;
    raw.receiver_tsbpd_delay_seconds = 0.12;
    raw.flight_size_packets = 10;
    raw.flow_window_packets = 1'000;
    raw.send_buffer_seconds = 0.0;
    raw.send_buffer_bytes = 0;
    raw.sender_tsbpd_delay_seconds = 0.12;

    auto delta = delta_of(second / 10);
    delta.available_delta_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL |
                                   ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_LOSS_PACKET_TOTAL |
                                   ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL;
    delta.unique_sent_wire_byte_delta = 125'000; /* 1 Mbit in 100 ms = 10 Mbit/s */
    delta.receiver_loss_packet_delta = 3;
    delta.unique_sent_packet_delta = 0;

    robotweax_srt_telemetry_ext_sample_v1 ext{};
    ext.struct_size = sizeof(ext);
    ext.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    ext.available_fields = ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_DATA_ARRIVAL_RATE_BITS_PER_SECOND;
    ext.data_arrival_rate_bits_per_second = 5e6;
    robotweax_srt_telemetry_ext_delta_v1 ext_delta{};
    ext_delta.struct_size = sizeof(ext_delta);
    ext_delta.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    ext_delta.available_delta_fields = ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_BELATED_PACKET_SINCE_CLEAR;
    ext_delta.belated_packet_delta = 2;

    auto metrics = empty_metrics();
    check(robotweax_srt_telemetry_metrics_compute(&raw, &ext, &delta, &ext_delta, &metrics) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "compute succeeds");
    const auto &v = metrics.values;
    check(near(v[ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND], 10e6) &&
              near(metrics.rate_numerators[ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND],
                   1e6),
          "bit rate and its numerator");
    check(near(v[ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_LOSS_PACKETS_PER_SECOND], 30.0),
          "packet rate");
    check(near(v[ROBOTWEAX_SRT_TELEMETRY_METRIC_BELATED_PACKETS_PER_SECOND], 20.0),
          "extended rate");
    check(near(v[ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO], 0.5),
          "recovery headroom while receiving");
    check(near(v[ROBOTWEAX_SRT_TELEMETRY_METRIC_FLIGHT_TO_FLOW_WINDOW_RATIO], 0.01),
          "flight to flow ratio");
    const auto available = metrics.available_metrics;
    check((available & bit(ROBOTWEAX_SRT_TELEMETRY_METRIC_SEND_BUFFER_TO_PEER_LATENCY_RATIO)) == 0,
          "no send-buffer ratio while nothing is sent or queued");
    check((available & bit(ROBOTWEAX_SRT_TELEMETRY_METRIC_LINK_CAPACITY_BITS_PER_SECOND)) == 0,
          "Robotweax arrival rate is never reported as link capacity");
    check((available & bit(ROBOTWEAX_SRT_TELEMETRY_METRIC_SENT_BITS_PER_SECOND)) == 0,
          "a rate without its counter is unavailable, not zero");

    /* Idle receiver: no arrivals and an empty buffer. */
    raw.receive_buffer_bytes = 0;
    raw.receive_buffer_seconds = 0.0;
    check(robotweax_srt_telemetry_metrics_compute(&raw, nullptr, &delta, nullptr, &metrics) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "compute without ext");
    check((metrics.available_metrics & bit(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO)) ==
              0,
          "an idle direction has no headroom value instead of a false zero");

    /* Gap blocking delivery: data still arrives, acknowledged span is zero. */
    delta.available_delta_fields |= ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL;
    delta.unique_received_packet_delta = 40;
    static_cast<void>(
        robotweax_srt_telemetry_metrics_compute(&raw, nullptr, &delta, nullptr, &metrics));
    check((metrics.available_metrics & bit(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO)) !=
                  0 &&
              metrics.values[ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO] == 0.0,
          "zero headroom while data arrives is reported");

    auto first = delta_of(0);
    first.quality_flags = ROBOTWEAX_SRT_TELEMETRY_QUALITY_FIRST_SAMPLE;
    first.available_delta_fields = delta.available_delta_fields;
    static_cast<void>(
        robotweax_srt_telemetry_metrics_compute(&raw, nullptr, &first, nullptr, &metrics));
    check((metrics.available_metrics &
           bit(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND)) == 0 &&
              metrics.interval_nano == 0 &&
              (metrics.quality_flags & ROBOTWEAX_SRT_TELEMETRY_QUALITY_FIRST_SAMPLE) != 0,
          "no rates on a baseline sample; quality carried over");

    check(robotweax_srt_telemetry_metrics_compute(nullptr, nullptr, &delta, nullptr, &metrics) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "raw is required");
    auto short_metrics = empty_metrics();
    short_metrics.struct_size = 8;
    check(robotweax_srt_telemetry_metrics_compute(&raw, nullptr, &delta, nullptr,
                                                  &short_metrics) ==
              ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH,
          "truncated output is an ABI mismatch");
}

struct Collected
{
    std::vector<robotweax_srt_telemetry_window_summary_v1> summaries;
    int fail_after = -1;
};

int collect(void *user_data, const robotweax_srt_telemetry_window_summary_v1 *summary)
{
    auto &collected = *static_cast<Collected *>(user_data);
    if (collected.fail_after >= 0 &&
        static_cast<int>(collected.summaries.size()) >= collected.fail_after)
    {
        return 1;
    }
    collected.summaries.push_back(*summary);
    return 0;
}

/* A metric sample with one gauge (RTT) and one rate (unique sent bits). */
robotweax_srt_telemetry_metric_sample_v1 metric_at(const std::uint64_t unix_nano,
                                                   const double rtt, const double bits,
                                                   const std::uint64_t interval)
{
    auto sample = empty_metrics();
    sample.observed_at_unix_nano = unix_nano;
    sample.sample_monotonic_nano = unix_nano;
    sample.interval_nano = interval;
    sample.values[ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS] = rtt;
    sample.available_metrics = bit(ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS);
    if (interval > 0)
    {
        const auto seconds = static_cast<double>(interval) / 1e9;
        sample.values[ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND] = bits / seconds;
        sample.rate_numerators[ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND] = bits;
        sample.available_metrics |= bit(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND);
    }
    return sample;
}

void test_windows()
{
    robotweax_srt_telemetry_window_config_v1 config{};
    robotweax_srt_telemetry_window_config_default(&config);
    robotweax_srt_telemetry_window_aggregator *aggregator = nullptr;
    check(robotweax_srt_telemetry_window_aggregator_create(&config, &aggregator) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "aggregator creation");
    Collected collected;

    /* 25 samples, 100 ms apart, from base+0.05 s. RTT 10 ms except a 300 ms
     * spike at sample 7; 100 kbit per sample except a 1 Mbit burst at 12. */
    const auto period = second / 10;
    for (int index = 0; index < 25; ++index)
    {
        const auto unix_nano = base_unix + second / 20 + static_cast<std::uint64_t>(index) * period;
        const auto rtt = index == 7 ? 0.3 : 0.01;
        const auto bits = index == 12 ? 1e6 : 1e5;
        auto sample = metric_at(unix_nano, rtt, bits, index == 0 ? 0 : period);
        if (index == 0)
        {
            sample.quality_flags = ROBOTWEAX_SRT_TELEMETRY_QUALITY_FIRST_SAMPLE;
        }
        check(robotweax_srt_telemetry_window_aggregator_push(aggregator, &sample, &collect,
                                                             &collected) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK,
              "push");
    }
    check(collected.summaries.size() == 2, "two complete 1 s windows closed");
    if (collected.summaries.size() == 2)
    {
        const auto &first = collected.summaries[0];
        check(first.window_nano == second && first.start_unix_nano == base_unix &&
                  first.end_unix_nano == base_unix + second,
              "first window aligned to the second");
        check(first.sample_count == 10, "ten samples in the first second");
        check((first.quality_flags & ROBOTWEAX_SRT_TELEMETRY_WINDOW_BASELINE) != 0,
              "baseline window is flagged");
        const auto &rtt = first.metrics[ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS];
        check(near(rtt.max, 0.3) && near(rtt.min, 0.01) && near(rtt.last, 0.01),
              "the RTT spike survives as the window max");
        check((rtt.flags & ROBOTWEAX_SRT_TELEMETRY_METRIC_SUMMARY_PERCENTILE_AVAILABLE) != 0 &&
                  near_float(rtt.p95, 0.3),
              "nearest-rank p95 of ten samples is the largest");
        const auto &bits = first.metrics[ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND];
        check(bits.sample_count == 9 && near(bits.total, 9e5) && near(bits.rate, 1e6),
              "rate window: exact total over covered time");

        const auto &second_window = collected.summaries[1];
        const auto &burst =
            second_window.metrics[ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND];
        check(near(burst.max, 1e7) && near(burst.total, 1.9e6) && near(burst.rate, 1.9e6),
              "a 100 ms burst shows as a 10 Mbit/s max inside a 1.9 Mbit/s second");
        check((second_window.quality_flags & ROBOTWEAX_SRT_TELEMETRY_WINDOW_BASELINE) == 0,
              "later windows are not baseline windows");
    }

    collected.summaries.clear();
    check(robotweax_srt_telemetry_window_aggregator_flush(aggregator, &collect, &collected) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "flush");
    check(collected.summaries.size() == 3, "flush emits the open 1 s, 10 s and 60 s windows");
    if (collected.summaries.size() == 3)
    {
        check(collected.summaries[0].window_nano == second &&
                  collected.summaries[1].window_nano == 10 * second &&
                  collected.summaries[2].window_nano == 60 * second,
              "shortest window first");
        const auto &ten = collected.summaries[1];
        check((ten.quality_flags & ROBOTWEAX_SRT_TELEMETRY_WINDOW_PARTIAL) != 0 &&
                  ten.sample_count == 25,
              "partial 10 s window holds every sample");
        const auto &rtt = ten.metrics[ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS];
        check(near(rtt.max, 0.3) && near_float(rtt.p95, 0.01),
              "one spike in 25 samples is the max but not the p95");
    }
    collected.summaries.clear();
    check(robotweax_srt_telemetry_window_aggregator_flush(aggregator, &collect, &collected) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              collected.summaries.empty(),
          "a second flush emits nothing");

    /* Gap and clock step. */
    auto late = metric_at(base_unix + 30 * second, 0.01, 1e5, 5 * second);
    static_cast<void>(
        robotweax_srt_telemetry_window_aggregator_push(aggregator, &late, &collect, &collected));
    auto stepped = metric_at(base_unix + 20 * second, 0.01, 1e5, period);
    static_cast<void>(robotweax_srt_telemetry_window_aggregator_push(aggregator, &stepped, &collect,
                                                                     &collected));
    check(!collected.summaries.empty() &&
              (collected.summaries[0].quality_flags & ROBOTWEAX_SRT_TELEMETRY_WINDOW_SAMPLING_GAP) !=
                  0 &&
              (collected.summaries[0].quality_flags & ROBOTWEAX_SRT_TELEMETRY_WINDOW_CLOCK_STEP) != 0,
          "a long interval is a gap; wall time stepping back closes the window as a clock step");
    robotweax_srt_telemetry_window_aggregator_destroy(aggregator);
}

void test_truncation_and_sink_failure()
{
    robotweax_srt_telemetry_window_config_v1 config{};
    robotweax_srt_telemetry_window_config_default(&config);
    config.expected_period_nano = second / 2; /* buffers sized for 2 Hz */
    config.window_count = 1;
    robotweax_srt_telemetry_window_aggregator *aggregator = nullptr;
    static_cast<void>(robotweax_srt_telemetry_window_aggregator_create(&config, &aggregator));
    Collected collected;
    for (int index = 0; index < 50; ++index) /* 50 Hz instead */
    {
        auto sample = metric_at(base_unix + static_cast<std::uint64_t>(index) * second / 50, 0.01,
                                1e3, second / 50);
        static_cast<void>(robotweax_srt_telemetry_window_aggregator_push(aggregator, &sample,
                                                                         &collect, &collected));
    }
    static_cast<void>(
        robotweax_srt_telemetry_window_aggregator_flush(aggregator, &collect, &collected));
    check(collected.summaries.size() == 1 &&
              (collected.summaries[0].metrics[ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS].flags &
               ROBOTWEAX_SRT_TELEMETRY_METRIC_SUMMARY_PERCENTILE_TRUNCATED) != 0,
          "more samples than the p95 buffer is flagged, not silently wrong");
    check(collected.summaries.size() == 1 &&
              collected.summaries[0].metrics[ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS]
                      .sample_count == 50,
          "min/max/mean still use every sample");

    auto sample = metric_at(base_unix + 5 * second, 0.01, 1e3, second / 50);
    static_cast<void>(
        robotweax_srt_telemetry_window_aggregator_push(aggregator, &sample, &collect, &collected));
    Collected failing;
    failing.fail_after = 0;
    auto next = metric_at(base_unix + 7 * second, 0.01, 1e3, second / 50);
    check(robotweax_srt_telemetry_window_aggregator_push(aggregator, &next, &collect, &failing) ==
              ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE,
          "sink failure is reported");
    robotweax_srt_telemetry_window_aggregator_destroy(aggregator);
}

void test_config_validation()
{
    robotweax_srt_telemetry_window_aggregator *aggregator = nullptr;
    robotweax_srt_telemetry_window_config_v1 config{};
    robotweax_srt_telemetry_window_config_default(&config);
    config.window_nano[1] = 7 * second; /* does not divide a day */
    check(robotweax_srt_telemetry_window_aggregator_create(&config, &aggregator) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "windows must divide a day");
    robotweax_srt_telemetry_window_config_default(&config);
    config.window_nano[0] = second / 20; /* shorter than the period */
    check(robotweax_srt_telemetry_window_aggregator_create(&config, &aggregator) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "windows must not be shorter than the period");
    robotweax_srt_telemetry_window_config_default(&config);
    config.percentile_metrics |= UINT64_C(1) << 40;
    check(robotweax_srt_telemetry_window_aggregator_create(&config, &aggregator) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "percentiles only for defined metrics");
    robotweax_srt_telemetry_window_config_default(&config);
    config.struct_size = 8;
    check(robotweax_srt_telemetry_window_aggregator_create(&config, &aggregator) ==
              ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH,
          "truncated config is an ABI mismatch");
    check(aggregator == nullptr, "no aggregator on failure");
}

} // namespace

int main()
{
    test_catalog();
    test_compute();
    test_windows();
    test_truncation_and_sink_failure();
    test_config_validation();
    if (failures != 0)
    {
        std::cerr << failures << " metric and window checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all metric and window checks passed\n";
    return EXIT_SUCCESS;
}
