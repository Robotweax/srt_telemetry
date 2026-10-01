#ifndef ROBOTWEAX_SRT_TELEMETRY_METRICS_H
#define ROBOTWEAX_SRT_TELEMETRY_METRICS_H

/* Dashboard metrics and time windows.
 *
 * A metric is one number per sample that a dashboard plots: a rate derived
 * from counter deltas, a gauge, or a ratio of gauges. The window aggregator
 * turns a stream of samples into tumbling windows (by default 1 s, 10 s and
 * 60 s, aligned to wall-clock multiples) with min, max, mean, last, p95 and,
 * for rates, the exact total and window rate. Short bursts therefore stay
 * visible at every resolution, whatever the export interval. */

#include <robotweax/srt_telemetry.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum robotweax_srt_telemetry_metric
    {
        /* Rates from counter deltas (per second). */
        ROBOTWEAX_SRT_TELEMETRY_METRIC_SENT_BITS_PER_SECOND = 0,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND = 1,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_RETRANSMITTED_BITS_PER_SECOND = 2,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVED_BITS_PER_SECOND = 3,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_RECEIVED_BITS_PER_SECOND = 4,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_PACKETS_PER_SECOND = 5,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_RECEIVED_PACKETS_PER_SECOND = 6,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_SENDER_LOSS_PACKETS_PER_SECOND = 7,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_LOSS_PACKETS_PER_SECOND = 8,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_RETRANSMITTED_PACKETS_PER_SECOND = 9,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVED_RETRANSMITTED_PACKETS_PER_SECOND = 10,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_SENDER_DROP_PACKETS_PER_SECOND = 11,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_DROP_PACKETS_PER_SECOND = 12,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_UNDECRYPTABLE_PACKETS_PER_SECOND = 13,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_BELATED_PACKETS_PER_SECOND = 14,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_FILTER_RECOVERED_PACKETS_PER_SECOND = 15,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_FILTER_UNRECOVERED_PACKETS_PER_SECOND = 16,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_SENT_NAK_PER_SECOND = 17,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVED_NAK_PER_SECOND = 18,
        /* Gauges. */
        ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS = 19,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_SEND_BUFFER_SECONDS = 20,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVE_BUFFER_SECONDS = 21,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_TSBPD_DELAY_SECONDS = 22,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_SENDER_TSBPD_DELAY_SECONDS = 23,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_FLIGHT_SIZE_PACKETS = 24,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_FLOW_WINDOW_PACKETS = 25,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_AVAILABLE_RECEIVE_BUFFER_BYTES = 26,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_PACKET_SEND_PERIOD_SECONDS = 27,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_LINK_CAPACITY_BITS_PER_SECOND = 28,
        /* Ratios of gauges. */
        /* receive buffer span / receiver latency: about 1 when no packet is
         * missing, falling toward 0 while a gap waits for repair. */
        /* Only while data arrives or is buffered; otherwise unavailable, so a
         * send-only endpoint never shows a false zero. */
        ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO = 29,
        /* flight size / flow window: near 1 means the peer stopped reading. */
        ROBOTWEAX_SRT_TELEMETRY_METRIC_FLIGHT_TO_FLOW_WINDOW_RATIO = 30,
        /* send buffer span / peer latency: data ageing toward a too-late drop.
         * Only while data is sent or queued. */
        ROBOTWEAX_SRT_TELEMETRY_METRIC_SEND_BUFFER_TO_PEER_LATENCY_RATIO = 31,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_DEFINED_COUNT = 32
    } robotweax_srt_telemetry_metric;

/* Slots reserved in v1 structures; metrics added later use free slots. */
#define ROBOTWEAX_SRT_TELEMETRY_METRIC_SLOTS_V1 48u

#define ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(metric) (UINT64_C(1) << (metric))

/* Metrics whose p95 is computed by default: the ones whose bursts matter. */
#define ROBOTWEAX_SRT_TELEMETRY_DEFAULT_PERCENTILE_METRICS                                         \
    (ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND) | \
     ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(                                                           \
         ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_RECEIVED_BITS_PER_SECOND) |                         \
     ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(ROBOTWEAX_SRT_TELEMETRY_METRIC_RETRANSMITTED_BITS_PER_SECOND) | \
     ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS) |              \
     ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(ROBOTWEAX_SRT_TELEMETRY_METRIC_SEND_BUFFER_SECONDS) |      \
     ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVE_BUFFER_SECONDS) |   \
     ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO))

    typedef enum robotweax_srt_telemetry_metric_kind
    {
        ROBOTWEAX_SRT_TELEMETRY_METRIC_KIND_RATE = 1,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_KIND_GAUGE = 2,
        ROBOTWEAX_SRT_TELEMETRY_METRIC_KIND_RATIO = 3
    } robotweax_srt_telemetry_metric_kind;

    typedef struct robotweax_srt_telemetry_metric_info_v1
    {
        /* Stable snake_case name, e.g. "unique_sent_bits_per_second". */
        const char *name;
        /* UCUM unit: "bit/s", "{packet}/s", "s", "{packet}", "By", "1". */
        const char *unit;
        robotweax_srt_telemetry_metric_kind kind;
        robotweax_srt_telemetry_endpoint_role role;
    } robotweax_srt_telemetry_metric_info_v1;

    /* One value per defined metric for one sample. */
    typedef struct robotweax_srt_telemetry_metric_sample_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        uint64_t available_metrics;
        uint64_t observed_at_unix_nano;
        uint64_t sample_monotonic_nano;
        /* Interval behind rate metrics; 0 when no rate is available. */
        uint64_t interval_nano;
        /* QUALITY_* flags of the delta this sample was computed from. */
        uint64_t quality_flags;
        double values[ROBOTWEAX_SRT_TELEMETRY_METRIC_SLOTS_V1];
        /* Counter delta behind each rate metric (packets or bits); 0 for
         * gauges and ratios. Windows sum these for exact totals. */
        double rate_numerators[ROBOTWEAX_SRT_TELEMETRY_METRIC_SLOTS_V1];
    } robotweax_srt_telemetry_metric_sample_v1;

#define ROBOTWEAX_SRT_TELEMETRY_METRIC_SUMMARY_PERCENTILE_AVAILABLE UINT32_C(1)
/* More samples than the percentile buffer holds: p95 uses the first ones. */
#define ROBOTWEAX_SRT_TELEMETRY_METRIC_SUMMARY_PERCENTILE_TRUNCATED (UINT32_C(1) << 1)

    typedef struct robotweax_srt_telemetry_metric_summary_v1
    {
        uint32_t sample_count;
        uint32_t flags;
        double min;
        double max;
        double mean;
        double last;
        /* Nearest-rank 95th percentile; stored with 32-bit float precision
         * (about 7 significant digits) to keep the buffers small. */
        double p95;
        /* Rates only: sum of counter deltas (packets or bits) in the window and
         * that sum divided by the covered time. */
        double total;
        double rate;
    } robotweax_srt_telemetry_metric_summary_v1;

/* Window quality flags. */
#define ROBOTWEAX_SRT_TELEMETRY_WINDOW_PARTIAL UINT32_C(1)
#define ROBOTWEAX_SRT_TELEMETRY_WINDOW_SAMPLING_GAP (UINT32_C(1) << 1)
#define ROBOTWEAX_SRT_TELEMETRY_WINDOW_CLOCK_STEP (UINT32_C(1) << 2)
#define ROBOTWEAX_SRT_TELEMETRY_WINDOW_COUNTER_RESET (UINT32_C(1) << 3)
#define ROBOTWEAX_SRT_TELEMETRY_WINDOW_BASELINE (UINT32_C(1) << 4)

    typedef struct robotweax_srt_telemetry_window_summary_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        uint64_t window_nano;
        /* Aligned wall-clock bounds [start, end). */
        uint64_t start_unix_nano;
        uint64_t end_unix_nano;
        /* Sum of the sample intervals that contributed rates. */
        uint64_t covered_nano;
        uint32_t sample_count;
        uint32_t quality_flags;
        uint64_t available_metrics;
        robotweax_srt_telemetry_metric_summary_v1 metrics[ROBOTWEAX_SRT_TELEMETRY_METRIC_SLOTS_V1];
    } robotweax_srt_telemetry_window_summary_v1;

#define ROBOTWEAX_SRT_TELEMETRY_MAX_WINDOWS 4u

    typedef struct robotweax_srt_telemetry_window_config_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        /* Expected sampling period; sizes the p95 buffers and defines a gap
         * (an interval above twice the period). */
        uint64_t expected_period_nano;
        uint32_t window_count;
        uint32_t reserved;
        /* Each window must divide 24 h evenly and be at least the period. */
        uint64_t window_nano[ROBOTWEAX_SRT_TELEMETRY_MAX_WINDOWS];
        /* Metrics that get a p95; memory is about 4 bytes x metrics x
         * (window / period) per window. */
        uint64_t percentile_metrics;
    } robotweax_srt_telemetry_window_config_v1;

    /* Return zero to continue. A non-zero result stops the push or flush and
     * is reported as ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE. */
    typedef int (*robotweax_srt_telemetry_window_sink)(
        void *user_data, const robotweax_srt_telemetry_window_summary_v1 *summary);

    typedef struct robotweax_srt_telemetry_window_aggregator
        robotweax_srt_telemetry_window_aggregator;

    ROBOTWEAX_SRT_TELEMETRY_API const robotweax_srt_telemetry_metric_info_v1 *
    robotweax_srt_telemetry_metric_info(robotweax_srt_telemetry_metric metric);

    /* Fills the default configuration: 100 ms period, 1 s / 10 s / 60 s
     * windows, default percentile metrics. */
    ROBOTWEAX_SRT_TELEMETRY_API void
    robotweax_srt_telemetry_window_config_default(robotweax_srt_telemetry_window_config_v1 *config);

    /* Derives metric values from one sample. ext and ext_delta may be NULL. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error robotweax_srt_telemetry_metrics_compute(
        const robotweax_srt_telemetry_raw_sample_v1 *raw,
        const robotweax_srt_telemetry_ext_sample_v1 *ext,
        const robotweax_srt_telemetry_delta_v1 *delta,
        const robotweax_srt_telemetry_ext_delta_v1 *ext_delta,
        robotweax_srt_telemetry_metric_sample_v1 *metrics);

    /* All buffers are allocated at creation; push and flush never allocate. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_window_aggregator_create(
        const robotweax_srt_telemetry_window_config_v1 *config,
        robotweax_srt_telemetry_window_aggregator **aggregator);

    ROBOTWEAX_SRT_TELEMETRY_API void robotweax_srt_telemetry_window_aggregator_destroy(
        robotweax_srt_telemetry_window_aggregator *aggregator);

    /* Adds one metric sample. Windows whose end has passed are emitted to the
     * sink first, shortest window first. Empty windows are not emitted. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_window_aggregator_push(
        robotweax_srt_telemetry_window_aggregator *aggregator,
        const robotweax_srt_telemetry_metric_sample_v1 *sample,
        robotweax_srt_telemetry_window_sink sink, void *user_data);

    /* Emits every open window, marked PARTIAL, and starts empty windows. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_window_aggregator_flush(
        robotweax_srt_telemetry_window_aggregator *aggregator,
        robotweax_srt_telemetry_window_sink sink, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
