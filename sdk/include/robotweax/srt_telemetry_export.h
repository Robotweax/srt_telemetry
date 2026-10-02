#ifndef ROBOTWEAX_SRT_TELEMETRY_EXPORT_H
#define ROBOTWEAX_SRT_TELEMETRY_EXPORT_H

/* Export model: one versioned naming scheme for every output, and renderers
 * that turn the sampler's current state into Prometheus text exposition or a
 * JSON snapshot. Every output formats the same records, so a Grafana panel,
 * the built-in UI and a JSONL file cannot disagree.
 *
 * Names follow OpenTelemetry style (srt.receiver.recovery_headroom) and
 * Prometheus conventions (srt_receiver_recovery_headroom_ratio). The scheme
 * is versioned: names in a released version never change meaning; a change
 * gets a new name and a new schema version. See
 * docs/architecture/metric-names.md. */

#include <robotweax/srt_telemetry_metrics.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define ROBOTWEAX_SRT_TELEMETRY_METRIC_SCHEMA_VERSION 1u

    /* OpenTelemetry-style name, e.g. "srt.receiver.recovery_headroom". NULL
     * for an undefined metric. */
    ROBOTWEAX_SRT_TELEMETRY_API const char *
    robotweax_srt_telemetry_metric_otel_name(robotweax_srt_telemetry_metric metric);

    /* Prometheus metric family name, e.g. "srt_receiver_recovery_headroom_ratio". */
    ROBOTWEAX_SRT_TELEMETRY_API const char *
    robotweax_srt_telemetry_metric_prometheus_name(robotweax_srt_telemetry_metric metric);

    /* "sender", "receiver" or "connection" (both roles). */
    ROBOTWEAX_SRT_TELEMETRY_API const char *
    robotweax_srt_telemetry_metric_scope(robotweax_srt_telemetry_metric metric);

    typedef enum robotweax_srt_telemetry_render_format
    {
        /* Prometheus text exposition format 0.0.4. */
        ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS = 1,
        /* JSON snapshot, schema "srt-telemetry.snapshot.v1". */
        ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON = 2,
        /* OTLP/HTTP JSON: an ExportMetricsServiceRequest for POST /v1/metrics
         * (OpenTelemetry names, window statistics in attribute "stat",
         * lifetime counters as cumulative sums). See docs/architecture/otlp.md. */
        ROBOTWEAX_SRT_TELEMETRY_RENDER_OTLP_JSON = 3
    } robotweax_srt_telemetry_render_format;

/* render_options_v1.flags */
/* Leave out the per-indicator health series. */
#define ROBOTWEAX_SRT_TELEMETRY_RENDER_NO_INDICATORS UINT32_C(1)
/* Leave out the lifetime counters. */
#define ROBOTWEAX_SRT_TELEMETRY_RENDER_NO_COUNTERS (UINT32_C(1) << 1)

    typedef struct robotweax_srt_telemetry_render_options_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        robotweax_srt_telemetry_render_format format;
        uint32_t flags;
        /* Which configured window to export (its latest closed instance).
         * 0: 10 s if configured, otherwise the shortest window. */
        uint64_t window_nano;
    } robotweax_srt_telemetry_render_options_v1;

    ROBOTWEAX_SRT_TELEMETRY_API void
    robotweax_srt_telemetry_render_options_default(robotweax_srt_telemetry_render_format format,
                                                   robotweax_srt_telemetry_render_options_v1 *options);

    /* Receives the rendered output in one or more chunks. Return zero to
     * continue; non-zero stops rendering with SINK_FAILURE. */
    typedef int (*robotweax_srt_telemetry_render_writer)(void *user_data, const char *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif
