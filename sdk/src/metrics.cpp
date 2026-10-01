#include <robotweax/srt_telemetry_metrics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace
{

using Info = robotweax_srt_telemetry_metric_info_v1;

constexpr auto rate = ROBOTWEAX_SRT_TELEMETRY_METRIC_KIND_RATE;
constexpr auto gauge = ROBOTWEAX_SRT_TELEMETRY_METRIC_KIND_GAUGE;
constexpr auto ratio = ROBOTWEAX_SRT_TELEMETRY_METRIC_KIND_RATIO;
constexpr auto sender = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
constexpr auto receiver = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;
constexpr auto both = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_UNKNOWN;

constexpr std::array<Info, ROBOTWEAX_SRT_TELEMETRY_METRIC_DEFINED_COUNT> catalog{{
    {"sent_bits_per_second", "bit/s", rate, sender},
    {"unique_sent_bits_per_second", "bit/s", rate, sender},
    {"retransmitted_bits_per_second", "bit/s", rate, sender},
    {"received_bits_per_second", "bit/s", rate, receiver},
    {"unique_received_bits_per_second", "bit/s", rate, receiver},
    {"unique_sent_packets_per_second", "{packet}/s", rate, sender},
    {"unique_received_packets_per_second", "{packet}/s", rate, receiver},
    {"sender_loss_packets_per_second", "{packet}/s", rate, sender},
    {"receiver_loss_packets_per_second", "{packet}/s", rate, receiver},
    {"retransmitted_packets_per_second", "{packet}/s", rate, sender},
    {"received_retransmitted_packets_per_second", "{packet}/s", rate, receiver},
    {"sender_drop_packets_per_second", "{packet}/s", rate, sender},
    {"receiver_drop_packets_per_second", "{packet}/s", rate, receiver},
    {"undecryptable_packets_per_second", "{packet}/s", rate, receiver},
    {"belated_packets_per_second", "{packet}/s", rate, receiver},
    {"filter_recovered_packets_per_second", "{packet}/s", rate, receiver},
    {"filter_unrecovered_packets_per_second", "{packet}/s", rate, receiver},
    {"sent_nak_per_second", "{packet}/s", rate, receiver},
    {"received_nak_per_second", "{packet}/s", rate, sender},
    {"rtt_seconds", "s", gauge, both},
    {"send_buffer_seconds", "s", gauge, sender},
    {"receive_buffer_seconds", "s", gauge, receiver},
    {"receiver_tsbpd_delay_seconds", "s", gauge, receiver},
    {"sender_tsbpd_delay_seconds", "s", gauge, sender},
    {"flight_size_packets", "{packet}", gauge, sender},
    {"flow_window_packets", "{packet}", gauge, sender},
    {"available_receive_buffer_bytes", "By", gauge, receiver},
    {"packet_send_period_seconds", "s", gauge, sender},
    {"link_capacity_bits_per_second", "bit/s", gauge, both},
    {"recovery_headroom_ratio", "1", ratio, receiver},
    {"flight_to_flow_window_ratio", "1", ratio, sender},
    {"send_buffer_to_drop_threshold_ratio", "1", ratio, sender},
}};

constexpr std::uint64_t defined_metrics =
    (UINT64_C(1) << ROBOTWEAX_SRT_TELEMETRY_METRIC_DEFINED_COUNT) - 1;

constexpr std::uint64_t day_nano = UINT64_C(86'400'000'000'000);

/* Bits of every rate metric (catalog indices 0..18). */
constexpr std::uint64_t rate_metrics =
    (UINT64_C(1) << (ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVED_NAK_PER_SECOND + 1)) - 1;

template <typename T> bool valid_struct(const T *value)
{
    return value != nullptr && value->struct_size >= sizeof(T) &&
           value->abi_version == ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
}

bool is_rate(const std::size_t metric)
{
    return metric < catalog.size() && catalog[metric].kind == rate;
}

} // namespace

extern "C" const robotweax_srt_telemetry_metric_info_v1 *
robotweax_srt_telemetry_metric_info(const robotweax_srt_telemetry_metric metric)
{
    const auto index = static_cast<std::size_t>(metric);
    return index < catalog.size() ? &catalog[index] : nullptr;
}

extern "C" void
robotweax_srt_telemetry_window_config_default(robotweax_srt_telemetry_window_config_v1 *config)
{
    if (config == nullptr)
    {
        return;
    }
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config->expected_period_nano = UINT64_C(100'000'000);
    config->window_count = 3;
    config->window_nano[0] = UINT64_C(1'000'000'000);
    config->window_nano[1] = UINT64_C(10'000'000'000);
    config->window_nano[2] = UINT64_C(60'000'000'000);
    config->percentile_metrics = ROBOTWEAX_SRT_TELEMETRY_DEFAULT_PERCENTILE_METRICS;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_metrics_compute(
    const robotweax_srt_telemetry_raw_sample_v1 *raw,
    const robotweax_srt_telemetry_ext_sample_v1 *ext,
    const robotweax_srt_telemetry_delta_v1 *delta,
    const robotweax_srt_telemetry_ext_delta_v1 *ext_delta,
    robotweax_srt_telemetry_metric_sample_v1 *metrics)
{
    if (raw == nullptr || delta == nullptr || metrics == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (!valid_struct(raw) || !valid_struct(delta) || !valid_struct(metrics) ||
        (ext != nullptr && !valid_struct(ext)) || (ext_delta != nullptr && !valid_struct(ext_delta)))
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }

    std::memset(metrics, 0, sizeof(*metrics));
    metrics->struct_size = sizeof(*metrics);
    metrics->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    metrics->observed_at_unix_nano = raw->observed_at_unix_nano;
    metrics->sample_monotonic_nano = raw->sample_monotonic_nano;
    metrics->quality_flags = delta->quality_flags;

    const auto set = [&](const robotweax_srt_telemetry_metric metric, const double value,
                         const double numerator)
    {
        if (!std::isfinite(value))
        {
            return;
        }
        const auto index = static_cast<std::size_t>(metric);
        metrics->values[index] = value;
        metrics->rate_numerators[index] = numerator;
        metrics->available_metrics |= UINT64_C(1) << index;
    };

    const auto seconds = static_cast<double>(delta->interval_nano) / 1e9;
    if (delta->interval_nano > 0)
    {
        metrics->interval_nano = delta->interval_nano;
        const auto v1_rate = [&](const robotweax_srt_telemetry_metric metric,
                                 const std::uint64_t field, const std::uint64_t count,
                                 const double scale)
        {
            if ((delta->available_delta_fields & field) != 0)
            {
                const auto numerator = static_cast<double>(count) * scale;
                set(metric, numerator / seconds, numerator);
            }
        };
        v1_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_SENT_BITS_PER_SECOND,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL, delta->sent_wire_byte_delta,
                8.0);
        v1_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL,
                delta->unique_sent_wire_byte_delta, 8.0);
        v1_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_RETRANSMITTED_BITS_PER_SECOND,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_WIRE_BYTE_TOTAL,
                delta->retransmitted_wire_byte_delta, 8.0);
        v1_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVED_BITS_PER_SECOND,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_WIRE_BYTE_TOTAL,
                delta->received_wire_byte_delta, 8.0);
        v1_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_RECEIVED_BITS_PER_SECOND,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL,
                delta->unique_received_wire_byte_delta, 8.0);
        v1_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_PACKETS_PER_SECOND,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL,
                delta->unique_sent_packet_delta, 1.0);
        v1_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_RECEIVED_PACKETS_PER_SECOND,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL,
                delta->unique_received_packet_delta, 1.0);
        v1_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_SENDER_LOSS_PACKETS_PER_SECOND,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_LOSS_PACKET_TOTAL,
                delta->sender_loss_packet_delta, 1.0);
        v1_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_LOSS_PACKETS_PER_SECOND,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_LOSS_PACKET_TOTAL,
                delta->receiver_loss_packet_delta, 1.0);
        v1_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_RETRANSMITTED_PACKETS_PER_SECOND,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_PACKET_TOTAL,
                delta->retransmitted_packet_delta, 1.0);
        v1_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_SENDER_DROP_PACKETS_PER_SECOND,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_DROP_PACKET_TOTAL,
                delta->sender_drop_packet_delta, 1.0);
        v1_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_DROP_PACKETS_PER_SECOND,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_DROP_PACKET_TOTAL,
                delta->receiver_drop_packet_delta, 1.0);
        v1_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNDECRYPTABLE_PACKETS_PER_SECOND,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_UNDECRYPTABLE_PACKET_TOTAL,
                delta->receiver_undecryptable_packet_delta, 1.0);

        if (ext_delta != nullptr)
        {
            const auto ext_rate = [&](const robotweax_srt_telemetry_metric metric,
                                      const std::uint64_t field, const std::uint64_t count)
            {
                if ((ext_delta->available_delta_fields & field) != 0)
                {
                    const auto numerator = static_cast<double>(count);
                    set(metric, numerator / seconds, numerator);
                }
            };
            ext_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVED_RETRANSMITTED_PACKETS_PER_SECOND,
                     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_RETRANSMITTED_PACKET_SINCE_CLEAR,
                     ext_delta->received_retransmitted_packet_delta);
            ext_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_BELATED_PACKETS_PER_SECOND,
                     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_BELATED_PACKET_SINCE_CLEAR,
                     ext_delta->belated_packet_delta);
            ext_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_FILTER_RECOVERED_PACKETS_PER_SECOND,
                     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_FILTER_RECOVERED_PACKET_TOTAL,
                     ext_delta->filter_recovered_packet_delta);
            ext_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_FILTER_UNRECOVERED_PACKETS_PER_SECOND,
                     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_FILTER_UNRECOVERED_PACKET_TOTAL,
                     ext_delta->filter_unrecovered_packet_delta);
            ext_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_SENT_NAK_PER_SECOND,
                     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENT_NAK_PACKET_TOTAL,
                     ext_delta->sent_nak_packet_delta);
            ext_rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVED_NAK_PER_SECOND,
                     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_NAK_PACKET_TOTAL,
                     ext_delta->received_nak_packet_delta);
        }
    }

    const auto has = [&](const std::uint64_t field) { return (raw->available_fields & field) != 0; };
    const auto gauge_value = [&](const robotweax_srt_telemetry_metric metric,
                                 const std::uint64_t field, const double value)
    {
        if (has(field))
        {
            set(metric, value, 0.0);
        }
    };
    gauge_value(ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS, ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS,
                raw->rtt_seconds);
    gauge_value(ROBOTWEAX_SRT_TELEMETRY_METRIC_SEND_BUFFER_SECONDS,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_SECONDS, raw->send_buffer_seconds);
    gauge_value(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVE_BUFFER_SECONDS,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_SECONDS, raw->receive_buffer_seconds);
    gauge_value(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_TSBPD_DELAY_SECONDS,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_TSBPD_DELAY_SECONDS,
                raw->receiver_tsbpd_delay_seconds);
    gauge_value(ROBOTWEAX_SRT_TELEMETRY_METRIC_SENDER_TSBPD_DELAY_SECONDS,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_TSBPD_DELAY_SECONDS,
                raw->sender_tsbpd_delay_seconds);
    gauge_value(ROBOTWEAX_SRT_TELEMETRY_METRIC_FLIGHT_SIZE_PACKETS,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_FLIGHT_SIZE_PACKETS,
                static_cast<double>(raw->flight_size_packets));
    gauge_value(ROBOTWEAX_SRT_TELEMETRY_METRIC_FLOW_WINDOW_PACKETS,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_FLOW_WINDOW_PACKETS,
                static_cast<double>(raw->flow_window_packets));
    gauge_value(ROBOTWEAX_SRT_TELEMETRY_METRIC_AVAILABLE_RECEIVE_BUFFER_BYTES,
                ROBOTWEAX_SRT_TELEMETRY_FIELD_AVAILABLE_RECEIVE_BUFFER_BYTES,
                static_cast<double>(raw->available_receive_buffer_bytes));

    if (ext != nullptr)
    {
        if ((ext->available_fields & ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_PACKET_SEND_PERIOD_SECONDS) !=
            0)
        {
            set(ROBOTWEAX_SRT_TELEMETRY_METRIC_PACKET_SEND_PERIOD_SECONDS,
                ext->packet_send_period_seconds, 0.0);
        }
        if ((ext->available_fields &
             ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_ESTIMATED_LINK_CAPACITY_BITS_PER_SECOND) != 0)
        {
            set(ROBOTWEAX_SRT_TELEMETRY_METRIC_LINK_CAPACITY_BITS_PER_SECOND,
                ext->estimated_link_capacity_bits_per_second, 0.0);
        }
    }

    /* Ratios. Each is computed only when the endpoint is actually carrying
     * data in that direction, so an idle direction is unavailable, not 0. */
    /* Headroom needs a stream dense enough that one latency period holds
     * several packets; otherwise a single buffered packet reads as zero. */
    constexpr double minimum_packets_per_latency = 8.0;
    const auto dense_receive =
        delta->interval_nano > 0 &&
        (delta->available_delta_fields & ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL) !=
            0 &&
        has(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_TSBPD_DELAY_SECONDS) &&
        static_cast<double>(delta->unique_received_packet_delta) / seconds *
                raw->receiver_tsbpd_delay_seconds >=
            minimum_packets_per_latency;
    if (dense_receive && has(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_SECONDS) &&
        raw->receiver_tsbpd_delay_seconds > 0.0)
    {
        set(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO,
            raw->receive_buffer_seconds / raw->receiver_tsbpd_delay_seconds, 0.0);
    }
    if (has(ROBOTWEAX_SRT_TELEMETRY_FIELD_FLIGHT_SIZE_PACKETS) &&
        has(ROBOTWEAX_SRT_TELEMETRY_FIELD_FLOW_WINDOW_PACKETS) && raw->flow_window_packets > 0)
    {
        set(ROBOTWEAX_SRT_TELEMETRY_METRIC_FLIGHT_TO_FLOW_WINDOW_RATIO,
            static_cast<double>(raw->flight_size_packets) /
                static_cast<double>(raw->flow_window_packets),
            0.0);
    }
    const auto sending =
        (has(ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_BYTES) && raw->send_buffer_bytes > 0) ||
        ((delta->available_delta_fields & ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL) !=
             0 &&
         delta->unique_sent_packet_delta > 0);
    if (sending && has(ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_SECONDS) &&
        has(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_TSBPD_DELAY_SECONDS) &&
        raw->sender_tsbpd_delay_seconds > 0.0)
    {
        /* Sender too-late drop threshold with SRTO_SNDDROPDELAY 0. */
        const auto threshold = std::max(raw->sender_tsbpd_delay_seconds, 1.0) + 0.020;
        set(ROBOTWEAX_SRT_TELEMETRY_METRIC_SEND_BUFFER_TO_DROP_THRESHOLD_RATIO,
            raw->send_buffer_seconds / threshold, 0.0);
    }
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

/* ---------------------------------------------------------------- windows */

namespace
{

struct Accumulator
{
    std::uint32_t count = 0;
    std::uint32_t percentile_seen = 0;
    double min = 0.0;
    double max = 0.0;
    double sum = 0.0;
    double last = 0.0;
    double total = 0.0;
    std::uint64_t covered_nano = 0;
    bool truncated = false;
    std::size_t percentile_used = 0;
};

struct Window
{
    std::uint64_t length_nano = 0;
    std::size_t percentile_capacity = 0;
    bool open = false;
    std::uint64_t start = 0;
    /* Start of the window last closed by flush, to mark a continuation. */
    bool flushed = false;
    std::uint64_t flushed_start = 0;
    std::uint32_t sample_count = 0;
    std::uint32_t flags = 0;
    std::uint64_t covered_nano = 0;
    std::array<Accumulator, ROBOTWEAX_SRT_TELEMETRY_METRIC_DEFINED_COUNT> metrics{};
    /* One block of percentile_capacity floats per percentile metric. */
    std::vector<float> percentile_values;
};

} // namespace

struct robotweax_srt_telemetry_window_aggregator
{
    robotweax_srt_telemetry_window_config_v1 config{};
    std::vector<std::size_t> percentile_slot; /* metric -> block index or npos */
    std::vector<Window> windows;              /* shortest first */
    bool has_last_unix = false;
    std::uint64_t last_unix = 0;
    std::uint64_t random_state = UINT64_C(0x9E3779B97F4A7C15);
};

namespace
{

constexpr std::size_t no_slot = std::numeric_limits<std::size_t>::max();

void reset_window(Window &window, const std::uint64_t start)
{
    window.open = true;
    window.start = start;
    window.sample_count = 0;
    window.flags = 0;
    window.covered_nano = 0;
    window.metrics.fill(Accumulator{});
}

int emit(robotweax_srt_telemetry_window_aggregator &aggregator, Window &window,
         const std::uint32_t extra_flags, const robotweax_srt_telemetry_window_sink sink,
         void *user_data)
{
    if (!window.open || window.sample_count == 0)
    {
        window.open = false;
        return 0;
    }
    robotweax_srt_telemetry_window_summary_v1 summary{};
    summary.struct_size = sizeof(summary);
    summary.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    summary.window_nano = window.length_nano;
    summary.start_unix_nano = window.start;
    summary.end_unix_nano = window.start + window.length_nano;
    summary.covered_nano = window.covered_nano;
    summary.sample_count = window.sample_count;
    summary.quality_flags = window.flags | extra_flags;

    for (std::size_t metric = 0; metric < window.metrics.size(); ++metric)
    {
        const auto &accumulator = window.metrics[metric];
        if (accumulator.count == 0)
        {
            continue;
        }
        auto &out = summary.metrics[metric];
        summary.available_metrics |= UINT64_C(1) << metric;
        out.sample_count = accumulator.count;
        out.min = accumulator.min;
        out.max = accumulator.max;
        out.mean = accumulator.sum / static_cast<double>(accumulator.count);
        out.last = accumulator.last;
        if (is_rate(metric))
        {
            out.total = accumulator.total;
            if (accumulator.covered_nano > 0)
            {
                out.rate = accumulator.total / (static_cast<double>(accumulator.covered_nano) / 1e9);
            }
        }
        const auto slot = aggregator.percentile_slot[metric];
        if (slot != no_slot && accumulator.percentile_used > 0)
        {
            auto *begin = window.percentile_values.data() + slot * window.percentile_capacity;
            const auto used = accumulator.percentile_used;
            /* Nearest-rank p95. */
            const auto rank = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(used)));
            auto *nth = begin + (rank == 0 ? 0 : rank - 1);
            std::nth_element(begin, nth, begin + used);
            out.p95 = static_cast<double>(*nth);
            out.flags |= ROBOTWEAX_SRT_TELEMETRY_METRIC_SUMMARY_PERCENTILE_AVAILABLE;
            if (accumulator.truncated)
            {
                out.flags |= ROBOTWEAX_SRT_TELEMETRY_METRIC_SUMMARY_PERCENTILE_TRUNCATED;
            }
        }
    }
    int result = 1;
    try
    {
        result = sink(user_data, &summary);
    }
    catch (...)
    {
        result = 1; /* a sink must not throw across the C boundary */
    }
    if (result == 0)
    {
        window.open = false; /* a failed window stays open for a retry */
    }
    return result;
}

void accumulate(robotweax_srt_telemetry_window_aggregator &aggregator, Window &window,
                const robotweax_srt_telemetry_metric_sample_v1 &sample)
{
    window.sample_count += 1;
    if (sample.interval_nano > 0 && (sample.available_metrics & rate_metrics) != 0)
    {
        window.covered_nano += sample.interval_nano;
    }
    if ((sample.quality_flags & (ROBOTWEAX_SRT_TELEMETRY_QUALITY_FIRST_SAMPLE |
                                 ROBOTWEAX_SRT_TELEMETRY_QUALITY_PROVIDER_CHANGED)) != 0)
    {
        window.flags |= ROBOTWEAX_SRT_TELEMETRY_WINDOW_BASELINE;
    }
    if ((sample.quality_flags & ROBOTWEAX_SRT_TELEMETRY_QUALITY_COUNTER_RESET) != 0)
    {
        window.flags |= ROBOTWEAX_SRT_TELEMETRY_WINDOW_COUNTER_RESET;
    }
    if ((sample.quality_flags & ROBOTWEAX_SRT_TELEMETRY_QUALITY_MONOTONIC_CLOCK_REGRESSION) != 0)
    {
        window.flags |= ROBOTWEAX_SRT_TELEMETRY_WINDOW_CLOCK_STEP;
    }
    if ((sample.quality_flags & ROBOTWEAX_SRT_TELEMETRY_METRIC_SAMPLE_SOURCE_ERROR) != 0)
    {
        window.flags |= ROBOTWEAX_SRT_TELEMETRY_WINDOW_SOURCE_ERROR;
    }
    if (sample.interval_nano > 2 * aggregator.config.expected_period_nano)
    {
        window.flags |= ROBOTWEAX_SRT_TELEMETRY_WINDOW_SAMPLING_GAP;
    }

    for (std::size_t metric = 0; metric < window.metrics.size(); ++metric)
    {
        if ((sample.available_metrics & (UINT64_C(1) << metric)) == 0)
        {
            continue;
        }
        const auto value = sample.values[metric];
        if (!std::isfinite(value))
        {
            continue; /* a non-finite host value is treated as unavailable */
        }
        auto &accumulator = window.metrics[metric];
        if (accumulator.count == 0)
        {
            accumulator.min = value;
            accumulator.max = value;
        }
        else
        {
            accumulator.min = std::min(accumulator.min, value);
            accumulator.max = std::max(accumulator.max, value);
        }
        accumulator.count += 1;
        accumulator.sum += value;
        accumulator.last = value;
        if (is_rate(metric))
        {
            accumulator.total += sample.rate_numerators[metric];
            accumulator.covered_nano += sample.interval_nano;
        }
        const auto slot = aggregator.percentile_slot[metric];
        if (slot != no_slot && window.percentile_capacity > 0)
        {
            auto *block = window.percentile_values.data() + slot * window.percentile_capacity;
            accumulator.percentile_seen += 1;
            if (accumulator.percentile_used < window.percentile_capacity)
            {
                block[accumulator.percentile_used] = static_cast<float>(value);
                accumulator.percentile_used += 1;
            }
            else
            {
                /* Reservoir sampling keeps an unbiased subset, so a burst late
                 * in an overfull window can still reach the p95. */
                accumulator.truncated = true;
                auto &state = aggregator.random_state;
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                const auto pick = state % accumulator.percentile_seen;
                if (pick < window.percentile_capacity)
                {
                    block[pick] = static_cast<float>(value);
                }
            }
        }
    }
}

} // namespace

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_window_aggregator_create(
    const robotweax_srt_telemetry_window_config_v1 *config,
    robotweax_srt_telemetry_window_aggregator **aggregator)
{
    if (aggregator == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *aggregator = nullptr;
    if (config == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (!valid_struct(config))
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    const auto period = config->expected_period_nano;
    if (period == 0 || config->window_count == 0 ||
        config->window_count > ROBOTWEAX_SRT_TELEMETRY_MAX_WINDOWS || config->reserved != 0 ||
        (config->percentile_metrics & ~defined_metrics) != 0)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    for (std::uint32_t index = 0; index < config->window_count; ++index)
    {
        const auto length = config->window_nano[index];
        if (length < period || length > day_nano || day_nano % length != 0)
        {
            return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
        }
    }

    auto *created = new (std::nothrow) robotweax_srt_telemetry_window_aggregator{};
    if (created == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    try
    {
        created->config = *config;
        created->percentile_slot.assign(ROBOTWEAX_SRT_TELEMETRY_METRIC_DEFINED_COUNT, no_slot);
        std::size_t slots = 0;
        for (std::size_t metric = 0; metric < ROBOTWEAX_SRT_TELEMETRY_METRIC_DEFINED_COUNT; ++metric)
        {
            if ((config->percentile_metrics & (UINT64_C(1) << metric)) != 0)
            {
                created->percentile_slot[metric] = slots++;
            }
        }
        std::vector<std::uint64_t> lengths(config->window_nano,
                                           config->window_nano + config->window_count);
        std::sort(lengths.begin(), lengths.end());
        lengths.erase(std::unique(lengths.begin(), lengths.end()), lengths.end());
        for (const auto length : lengths)
        {
            Window window;
            window.length_nano = length;
            const auto expected = (length + period - 1) / period;
            window.percentile_capacity = static_cast<std::size_t>(expected + expected / 4 + 4);
            window.percentile_values.assign(slots * window.percentile_capacity, 0.0F);
            created->windows.push_back(std::move(window));
        }
    }
    catch (const std::bad_alloc &)
    {
        delete created;
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    *aggregator = created;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" void robotweax_srt_telemetry_window_aggregator_destroy(
    robotweax_srt_telemetry_window_aggregator *aggregator)
{
    delete aggregator;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_window_aggregator_push(
    robotweax_srt_telemetry_window_aggregator *aggregator,
    const robotweax_srt_telemetry_metric_sample_v1 *sample,
    const robotweax_srt_telemetry_window_sink sink, void *user_data)
{
    if (aggregator == nullptr || sample == nullptr || sink == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (!valid_struct(sample))
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }

    const auto now = sample->observed_at_unix_nano;
    /* A wall-clock step: time went backwards, or moved by more than one
     * period beyond what the monotonic interval accounts for. */
    bool clock_step = false;
    if (aggregator->has_last_unix)
    {
        if (now < aggregator->last_unix)
        {
            clock_step = true;
        }
        else if (sample->interval_nano > 0)
        {
            const auto wall = now - aggregator->last_unix;
            const auto mono = sample->interval_nano;
            const auto difference = wall > mono ? wall - mono : mono - wall;
            clock_step = difference > aggregator->config.expected_period_nano;
        }
    }

    /* Phase 1: close every window the sample has left. Nothing is
     * accumulated until all of them were emitted, so after a sink failure
     * the caller can push the same sample again without double counting. */
    for (auto &window : aggregator->windows)
    {
        const auto start = now - now % window.length_nano;
        if (window.open && start != window.start)
        {
            const auto flags =
                start < window.start ? ROBOTWEAX_SRT_TELEMETRY_WINDOW_CLOCK_STEP : 0U;
            if (emit(*aggregator, window, flags, sink, user_data) != 0)
            {
                return ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE;
            }
        }
    }

    /* Phase 2: accumulate. A source-error sample carries no interval, so it
     * must not become the reference for the next sample's clock check. */
    if ((sample->quality_flags & ROBOTWEAX_SRT_TELEMETRY_METRIC_SAMPLE_SOURCE_ERROR) == 0)
    {
        aggregator->has_last_unix = true;
        aggregator->last_unix = now;
    }
    for (auto &window : aggregator->windows)
    {
        const auto start = now - now % window.length_nano;
        if (!window.open)
        {
            reset_window(window, start);
            if (window.flushed && window.flushed_start == start)
            {
                /* The rest of a window that flush already reported. */
                window.flags |= ROBOTWEAX_SRT_TELEMETRY_WINDOW_PARTIAL;
            }
            window.flushed = false;
        }
        if (clock_step)
        {
            window.flags |= ROBOTWEAX_SRT_TELEMETRY_WINDOW_CLOCK_STEP;
        }
        accumulate(*aggregator, window, *sample);
    }
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_window_aggregator_flush(
    robotweax_srt_telemetry_window_aggregator *aggregator,
    const robotweax_srt_telemetry_window_sink sink, void *user_data)
{
    if (aggregator == nullptr || sink == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    for (auto &window : aggregator->windows)
    {
        const auto was_open = window.open && window.sample_count > 0;
        const auto start = window.start;
        if (emit(*aggregator, window, ROBOTWEAX_SRT_TELEMETRY_WINDOW_PARTIAL, sink, user_data) != 0)
        {
            return ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE;
        }
        if (was_open)
        {
            window.flushed = true;
            window.flushed_start = start;
        }
    }
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}
