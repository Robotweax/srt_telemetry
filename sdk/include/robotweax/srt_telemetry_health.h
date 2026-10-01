#ifndef ROBOTWEAX_SRT_TELEMETRY_HEALTH_H
#define ROBOTWEAX_SRT_TELEMETRY_HEALTH_H

/* Health engine: turns one endpoint's metric samples into a verdict an
 * operator can act on.
 *
 * Eight indicators, each computed from this endpoint's own data, are graded
 * against thresholds. Hysteresis keeps them steady: an indicator enters a
 * worse state after the condition has held for 2 s, and returns to a better
 * one after 10 s without it. The verdict is the worst indicator, with a
 * reason code and a suggested knob (setting, direction, value).
 *
 * The verdict is descriptive and one-sided. It says what this endpoint sees
 * and which setting would help; it makes no causal claim about the far side.
 * Missing data yields UNKNOWN, never OK. Default thresholds are defaults
 * until field data calibrates them; products can override every one. */

#include <robotweax/srt_telemetry_metrics.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Ordered by severity, so verdicts compare numerically, except that
     * UNKNOWN (0) means "no grade", not "better than OK". */
    typedef enum robotweax_srt_telemetry_health_verdict
    {
        ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN = 0,
        ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK = 1,
        ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH = 2,
        ROBOTWEAX_SRT_TELEMETRY_HEALTH_AT_RISK = 3,
        ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING = 4
    } robotweax_srt_telemetry_health_verdict;

    typedef enum robotweax_srt_telemetry_health_indicator
    {
        /* Receiver: receive buffer span / latency, minimum over the gauge
         * window. Time left to repair a gap before it becomes a skip. */
        ROBOTWEAX_SRT_TELEMETRY_INDICATOR_RECOVERY_HEADROOM = 0,
        /* Both: latency / RTT (maximum over the gauge window). Whether the
         * latency allows enough retransmission rounds. */
        ROBOTWEAX_SRT_TELEMETRY_INDICATOR_LATENCY_SUFFICIENCY = 1,
        /* Receiver: dropped / (unique received + dropped) over the count
         * window. What the viewer actually loses. */
        ROBOTWEAX_SRT_TELEMETRY_INDICATOR_UNRECOVERED_RATIO = 2,
        /* Receiver: 1 - (dropped - undecryptable) / loss detected over the
         * count window. Whether repair keeps up with the loss. */
        ROBOTWEAX_SRT_TELEMETRY_INDICATOR_RECOVERY_EFFICIENCY = 3,
        /* Sender: retransmitted rate / allowed overhead (input rate x
         * OHEADBW, or MAXBW minus input rate) over the count window. */
        ROBOTWEAX_SRT_TELEMETRY_INDICATOR_OVERHEAD_BUDGET = 4,
        /* Sender: sending rate / estimated link capacity over the gauge
         * window. Needs a provider that estimates capacity. */
        ROBOTWEAX_SRT_TELEMETRY_INDICATOR_PATH_HEADROOM = 5,
        /* Sender: send buffer span / too-late drop threshold (maximum over
         * the gauge window); sender drops in the count window escalate it. */
        ROBOTWEAX_SRT_TELEMETRY_INDICATOR_SENDER_BACKLOG = 6,
        /* Sender: flight size / flow window. Receiver: receive buffer span /
         * latency above 1 (data waiting for the application). Maximum over
         * the gauge window. The receiving application is not reading. */
        ROBOTWEAX_SRT_TELEMETRY_INDICATOR_PEER_READ_STALL = 7,
        ROBOTWEAX_SRT_TELEMETRY_INDICATOR_COUNT = 8
    } robotweax_srt_telemetry_health_indicator;

/* Indicator slots in v1 structures; indicators added later use free slots. */
#define ROBOTWEAX_SRT_TELEMETRY_INDICATOR_SLOTS_V1 16u

    typedef enum robotweax_srt_telemetry_health_reason
    {
        ROBOTWEAX_SRT_TELEMETRY_REASON_NONE = 0,
        /* No packets in the window (stream idle or not started). */
        ROBOTWEAX_SRT_TELEMETRY_REASON_NO_DATA = 1,
        /* A needed metric is unavailable; see missing_metrics. */
        ROBOTWEAX_SRT_TELEMETRY_REASON_MISSING_METRIC = 2,
        /* A needed setting is not in the context (e.g. OHEADBW). */
        ROBOTWEAX_SRT_TELEMETRY_REASON_MISSING_SETTING = 3,
        ROBOTWEAX_SRT_TELEMETRY_REASON_SOURCE_ERROR = 4,
        ROBOTWEAX_SRT_TELEMETRY_REASON_CONNECTING = 5,
        ROBOTWEAX_SRT_TELEMETRY_REASON_CONNECTION_LOST = 6,
        /* The endpoint role is unknown, so no indicator applies. */
        ROBOTWEAX_SRT_TELEMETRY_REASON_ROLE_UNKNOWN = 7,

        ROBOTWEAX_SRT_TELEMETRY_REASON_RECOVERY_HEADROOM_LOW = 16,
        ROBOTWEAX_SRT_TELEMETRY_REASON_LATENCY_LOW_FOR_RTT = 17,
        ROBOTWEAX_SRT_TELEMETRY_REASON_UNRECOVERED_LOSS = 18,
        /* Drops include packets that could not be decrypted. */
        ROBOTWEAX_SRT_TELEMETRY_REASON_UNDECRYPTABLE = 19,
        /* Drops with repairs arriving too late (belated packets). */
        ROBOTWEAX_SRT_TELEMETRY_REASON_LATE_REPAIRS = 20,
        ROBOTWEAX_SRT_TELEMETRY_REASON_RECOVERY_INEFFICIENT = 21,
        ROBOTWEAX_SRT_TELEMETRY_REASON_OVERHEAD_BUDGET_HIGH = 22,
        ROBOTWEAX_SRT_TELEMETRY_REASON_PATH_CAPACITY_EXCEEDED = 23,
        ROBOTWEAX_SRT_TELEMETRY_REASON_SENDER_BACKLOG = 24,
        ROBOTWEAX_SRT_TELEMETRY_REASON_SENDER_DROPS = 25,
        ROBOTWEAX_SRT_TELEMETRY_REASON_PEER_READ_STALL = 26,
        ROBOTWEAX_SRT_TELEMETRY_REASON_APPLICATION_READ_STALL = 27,
        /* Flight fills the flow window while the window is unchanged: no
         * acknowledgements arrive (path or peer unresponsive). No knob. */
        ROBOTWEAX_SRT_TELEMETRY_REASON_ACKNOWLEDGEMENTS_STALLED = 28,
        /* RTT well above its baseline with sustained loss: the path queues
         * and drops. Advice: lower the bitrate to 80 % of what it delivers. */
        ROBOTWEAX_SRT_TELEMETRY_REASON_PATH_CONGESTED = 29
    } robotweax_srt_telemetry_health_reason;

    typedef enum robotweax_srt_telemetry_health_knob
    {
        ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE = 0,
        /* SRTO_LATENCY (receiver) or SRTO_PEERLATENCY (sender); seconds. */
        ROBOTWEAX_SRT_TELEMETRY_KNOB_LATENCY = 1,
        /* The stream's (encoder) bitrate; bit/s. */
        ROBOTWEAX_SRT_TELEMETRY_KNOB_BITRATE = 2,
        /* SRTO_OHEADBW; percent. */
        ROBOTWEAX_SRT_TELEMETRY_KNOB_OVERHEAD_BANDWIDTH = 3,
        /* SRTO_MAXBW; bytes/s, as the option takes it. */
        ROBOTWEAX_SRT_TELEMETRY_KNOB_MAX_BANDWIDTH = 4,
        /* SRTO_PASSPHRASE / SRTO_PBKEYLEN on both ends; no value. */
        ROBOTWEAX_SRT_TELEMETRY_KNOB_PASSPHRASE = 5,
        /* The application reading from the receiving socket; no value. */
        ROBOTWEAX_SRT_TELEMETRY_KNOB_RECEIVING_APPLICATION = 6
    } robotweax_srt_telemetry_health_knob;

    typedef enum robotweax_srt_telemetry_knob_direction
    {
        ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_NONE = 0,
        ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_INCREASE = 1,
        ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_DECREASE = 2,
        /* Check the setting or component; no direction implied. */
        ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_CHECK = 3
    } robotweax_srt_telemetry_knob_direction;

    typedef enum robotweax_srt_telemetry_connection_condition
    {
        /* The host does not report connection state; indicators decide. */
        ROBOTWEAX_SRT_TELEMETRY_CONNECTION_UNREPORTED = 0,
        ROBOTWEAX_SRT_TELEMETRY_CONNECTION_CONNECTING = 1,
        ROBOTWEAX_SRT_TELEMETRY_CONNECTION_CONNECTED = 2,
        /* Broken, closed or rejected: FAILING at once. */
        ROBOTWEAX_SRT_TELEMETRY_CONNECTION_LOST = 3
    } robotweax_srt_telemetry_connection_condition;

    typedef struct robotweax_srt_telemetry_health_config_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        robotweax_srt_telemetry_endpoint_role role;
        uint32_t reserved;
        /* Sliding window for packet counts (loss, drops, retransmissions);
         * default 10 s. A single drop stays visible this long. */
        uint64_t count_window_nano;
        /* Sliding window for gauges (buffers, RTT, ratios); default 1 s. */
        uint64_t gauge_window_nano;
        /* Hysteresis: a worse state must hold this long; default 2 s. */
        uint64_t enter_hold_nano;
        /* A better state must hold this long; default 10 s. */
        uint64_t leave_hold_nano;
    } robotweax_srt_telemetry_health_config_v1;

    ROBOTWEAX_SRT_TELEMETRY_API void
    robotweax_srt_telemetry_health_config_default(robotweax_srt_telemetry_endpoint_role role,
                                                  robotweax_srt_telemetry_health_config_v1 *config);

/* health_context_v1.flags: the bandwidth settings below are known. */
#define ROBOTWEAX_SRT_TELEMETRY_HEALTH_CONTEXT_BANDWIDTH_SETTINGS UINT32_C(1)

    /* What the statistics cannot tell: connection state and settings, as the
     * connection monitor reads them. Optional; set it when it changes. */
    typedef struct robotweax_srt_telemetry_health_context_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        robotweax_srt_telemetry_connection_condition connection;
        uint32_t flags;
        /* SRTO_OHEADBW, percent. */
        int64_t overhead_percent;
        /* SRTO_MAXBW: -1 unlimited, 0 relative to the input rate. */
        int64_t max_bandwidth_bytes_per_second;
    } robotweax_srt_telemetry_health_context_v1;

    ROBOTWEAX_SRT_TELEMETRY_API void
    robotweax_srt_telemetry_health_context_init(robotweax_srt_telemetry_health_context_v1 *context);

/* health_indicator_status_v1.flags */
/* The indicator applies to this endpoint's role and settings. */
#define ROBOTWEAX_SRT_TELEMETRY_INDICATOR_APPLICABLE UINT32_C(1)
/* value was computed from this sample's windows. */
#define ROBOTWEAX_SRT_TELEMETRY_INDICATOR_AVAILABLE (UINT32_C(1) << 1)
/* Required for an OK verdict: unavailable means the verdict is UNKNOWN. */
#define ROBOTWEAX_SRT_TELEMETRY_INDICATOR_REQUIRED (UINT32_C(1) << 2)

    typedef struct robotweax_srt_telemetry_health_indicator_status_v1
    {
        double value;
        uint32_t flags;
        /* After hysteresis; what dashboards show. */
        robotweax_srt_telemetry_health_verdict state;
        /* This sample's grade before hysteresis. */
        robotweax_srt_telemetry_health_verdict raw_state;
        /* Why the indicator is in its state (or unavailable). */
        robotweax_srt_telemetry_health_reason reason;
    } robotweax_srt_telemetry_health_indicator_status_v1;

    typedef struct robotweax_srt_telemetry_health_status_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        uint64_t observed_at_unix_nano;
        uint64_t sample_monotonic_nano;
        /* Monotonic time the verdict last changed. */
        uint64_t verdict_since_monotonic_nano;
        robotweax_srt_telemetry_health_verdict verdict;
        robotweax_srt_telemetry_health_reason reason;
        /* Indicator behind the verdict, or -1 (connection, role, source). */
        int32_t primary_indicator;
        robotweax_srt_telemetry_health_knob knob;
        robotweax_srt_telemetry_knob_direction knob_direction;
        uint32_t reserved;
        /* Suggested knob value in the knob's unit, or 0 when none. */
        double suggested_value;
        /* METRIC_BIT()s an applicable indicator needed but did not get. */
        uint64_t missing_metrics;
        robotweax_srt_telemetry_health_indicator_status_v1
            indicators[ROBOTWEAX_SRT_TELEMETRY_INDICATOR_SLOTS_V1];
    } robotweax_srt_telemetry_health_status_v1;

    /* Not thread-safe: one engine per endpoint, driven from one thread. The
     * sampler runs one per source when health is enabled. */
    typedef struct robotweax_srt_telemetry_health_engine robotweax_srt_telemetry_health_engine;

    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_health_engine_create(const robotweax_srt_telemetry_health_config_v1 *config,
                                                 robotweax_srt_telemetry_health_engine **engine);

    ROBOTWEAX_SRT_TELEMETRY_API void
    robotweax_srt_telemetry_health_engine_destroy(robotweax_srt_telemetry_health_engine *engine);

    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error robotweax_srt_telemetry_health_engine_set_context(
        robotweax_srt_telemetry_health_engine *engine,
        const robotweax_srt_telemetry_health_context_v1 *context);

    /* Grades one indicator. For indicators where higher is worse the state
     * is the worst level whose threshold the value exceeds; where lower is
     * worse, the worst level the value falls below. Thresholds must be
     * ordered accordingly. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error robotweax_srt_telemetry_health_engine_set_thresholds(
        robotweax_srt_telemetry_health_engine *engine, robotweax_srt_telemetry_health_indicator indicator,
        double watch, double at_risk, double failing);

    /* The shipped defaults for a role; see docs/architecture/health-engine.md. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error robotweax_srt_telemetry_health_default_thresholds(
        robotweax_srt_telemetry_endpoint_role role, robotweax_srt_telemetry_health_indicator indicator,
        double *watch, double *at_risk, double *failing);

    /* Feeds one metric sample (samples must arrive in monotonic order) and
     * returns the status after it. A sample flagged METRIC_SAMPLE_SOURCE_ERROR
     * keeps the verdict (reason SOURCE_ERROR) for up to enter_hold after the
     * last good sample, then yields UNKNOWN / SOURCE_ERROR. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_health_engine_push(robotweax_srt_telemetry_health_engine *engine,
                                               const robotweax_srt_telemetry_metric_sample_v1 *sample,
                                               robotweax_srt_telemetry_health_status_v1 *status);

    ROBOTWEAX_SRT_TELEMETRY_API const char *
    robotweax_srt_telemetry_health_verdict_name(robotweax_srt_telemetry_health_verdict verdict);
    ROBOTWEAX_SRT_TELEMETRY_API const char *
    robotweax_srt_telemetry_health_indicator_name(robotweax_srt_telemetry_health_indicator indicator);
    ROBOTWEAX_SRT_TELEMETRY_API const char *
    robotweax_srt_telemetry_health_reason_name(robotweax_srt_telemetry_health_reason reason);
    ROBOTWEAX_SRT_TELEMETRY_API const char *
    robotweax_srt_telemetry_health_knob_name(robotweax_srt_telemetry_health_knob knob);

#ifdef __cplusplus
}
#endif

#endif
