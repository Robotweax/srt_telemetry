#ifndef ROBOTWEAX_SRT_TELEMETRY_OTLP_H
#define ROBOTWEAX_SRT_TELEMETRY_OTLP_H

/* Optional OTLP/HTTP push exporter (part of the HTTP library): every
 * interval, renders the sampler's state as OTLP JSON and POSTs it to an
 * OpenTelemetry collector (/v1/metrics). One thread, POSIX sockets, plain
 * HTTP: send to a collector on the same host or network, and let it forward
 * over TLS. See docs/architecture/otlp.md.
 *
 *   robotweax_srt_telemetry_otlp_exporter_config_v1 config;
 *   robotweax_srt_telemetry_otlp_exporter_config_default(sampler, &config);
 *   // config.endpoint defaults to http://127.0.0.1:4318/v1/metrics
 *   robotweax_srt_telemetry_otlp_exporter *exporter;
 *   robotweax_srt_telemetry_otlp_exporter_start(&config, &exporter);
 *   ...
 *   robotweax_srt_telemetry_otlp_exporter_stop(exporter); // before the sampler
 */

#include <robotweax/srt_telemetry_http.h>
#include <robotweax/srt_telemetry_sampler.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define ROBOTWEAX_SRT_TELEMETRY_OTLP_ENDPOINT_CAPACITY 256u
#define ROBOTWEAX_SRT_TELEMETRY_OTLP_HEADERS_CAPACITY 1024u

    typedef struct robotweax_srt_telemetry_otlp_exporter_config_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        /* Exported sampler; must outlive the exporter. */
        robotweax_srt_telemetry_sampler *sampler;
        /* "http://host[:port][/path]"; default
         * "http://127.0.0.1:4318/v1/metrics". A missing path means
         * /v1/metrics. https is not supported (no TLS in this library). */
        char endpoint[ROBOTWEAX_SRT_TELEMETRY_OTLP_ENDPOINT_CAPACITY];
        /* Extra request headers, "key=value,key=value" as in
         * OTEL_EXPORTER_OTLP_HEADERS, e.g. "authorization=Bearer%20abc";
         * values are percent-decoded.
         * Printable ASCII; keys are HTTP tokens; values without commas. */
        char headers[ROBOTWEAX_SRT_TELEMETRY_OTLP_HEADERS_CAPACITY];
        /* Export every interval, 100 ms to 1 h; default 10000 ms. */
        uint32_t interval_ms;
        /* Each attempt (name lookup, connect, send, response), 100 ms to
         * 60 s; default 5000 ms. An attempt that fails with a connection
         * error, a timeout, 429, 502, 503 or 504 is retried up to twice
         * while the interval lasts (retries end with the interval). */
        uint32_t timeout_ms;
        /* render_options_v1 flags (RENDER_NO_COUNTERS, RENDER_NO_INDICATORS). */
        uint32_t render_flags;
        uint32_t reserved;
        /* Window to export (0: the default window, 10 s). */
        uint64_t window_nano;
        /* Resource attributes, e.g. {"service.name", "encoder-7"}; copied at
         * start. At most 32. */
        const robotweax_srt_telemetry_label_v1 *resource_attributes;
        size_t resource_attribute_count;
    } robotweax_srt_telemetry_otlp_exporter_config_v1;

    ROBOTWEAX_SRT_TELEMETRY_HTTP_API void
    robotweax_srt_telemetry_otlp_exporter_config_default(robotweax_srt_telemetry_sampler *sampler,
                                                         robotweax_srt_telemetry_otlp_exporter_config_v1 *config);

    typedef struct robotweax_srt_telemetry_otlp_exporter robotweax_srt_telemetry_otlp_exporter;

    /* Validates the configuration and starts the export thread; the first
     * export follows after one interval. INVALID_ARGUMENT for a bad
     * configuration (including https endpoints). */
    ROBOTWEAX_SRT_TELEMETRY_HTTP_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_otlp_exporter_start(const robotweax_srt_telemetry_otlp_exporter_config_v1 *config,
                                                robotweax_srt_telemetry_otlp_exporter **exporter);

    typedef struct robotweax_srt_telemetry_otlp_exporter_stats_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        /* Exports the collector accepted (2xx), and exports given up. */
        uint64_t exports_succeeded;
        uint64_t exports_failed;
        /* Attempts retried after a retryable failure. */
        uint64_t retries;
        /* Request bytes sent (headers and body). */
        uint64_t bytes_sent;
        /* Of the latest attempt: HTTP status (0 when none was received) and
         * error (OK, IO_FAILURE for connection errors and timeouts,
         * SINK_FAILURE for a non-2xx status). */
        int32_t last_http_status;
        robotweax_srt_telemetry_error last_error;
        /* Wall-clock time of the latest accepted export (0: none yet). */
        uint64_t last_success_unix_nano;
    } robotweax_srt_telemetry_otlp_exporter_stats_v1;

    ROBOTWEAX_SRT_TELEMETRY_HTTP_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_otlp_exporter_stats(robotweax_srt_telemetry_otlp_exporter *exporter,
                                                robotweax_srt_telemetry_otlp_exporter_stats_v1 *stats);

    /* Stops the thread (an attempt in progress is abandoned within about
     * 100 ms) and frees the exporter. Stop it before destroying the sampler;
     * not from a sampler callback. */
    ROBOTWEAX_SRT_TELEMETRY_HTTP_API void
    robotweax_srt_telemetry_otlp_exporter_stop(robotweax_srt_telemetry_otlp_exporter *exporter);

#ifdef __cplusplus
}
#endif

#endif
