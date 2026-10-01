#ifndef ROBOTWEAX_SRT_TELEMETRY_SAMPLER_H
#define ROBOTWEAX_SRT_TELEMETRY_SAMPLER_H

/* Sampler: samples every registered source at a fixed period (100 ms by
 * default), derives metrics, keeps per-source time windows and delivers
 * samples and closed windows through callbacks. It can run its own thread or
 * be driven by the host's loop with sampler_tick.
 *
 * A source is any callback that fills one sample; the core never links an SRT
 * library. The public SRT adapter provides a ready source for its sessions
 * (robotweax_srt_telemetry_public_srt_source_config). */

#include <robotweax/srt_telemetry_export.h>
#include <robotweax/srt_telemetry_health.h>
#include <robotweax/srt_telemetry_history.h>
#include <robotweax/srt_telemetry_metrics.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define ROBOTWEAX_SRT_TELEMETRY_MAX_LABELS 8u
#define ROBOTWEAX_SRT_TELEMETRY_LABEL_KEY_CAPACITY 64u
#define ROBOTWEAX_SRT_TELEMETRY_LABEL_VALUE_CAPACITY 256u

    /* Host label such as {"site", "studio-2"}. Keys: [A-Za-z_][A-Za-z0-9_]*,
     * at most 63 bytes. Values: printable ASCII, at most 255 bytes. Labels are
     * copied when the source is added. */
    typedef struct robotweax_srt_telemetry_label_v1
    {
        const char *key;
        const char *value;
    } robotweax_srt_telemetry_label_v1;

    /* What a source fills on each tick. The sampler zero-initializes it and
     * sets every struct_size/abi_version before the call. */
    typedef struct robotweax_srt_telemetry_source_output_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        robotweax_srt_telemetry_raw_sample_v1 raw;
        robotweax_srt_telemetry_delta_v1 delta;
        /* Set has_ext to 1 when ext and ext_delta were filled. */
        uint32_t has_ext;
        /* Provider return code for diagnostics; 0 on success. */
        int32_t native_result;
        robotweax_srt_telemetry_ext_sample_v1 ext;
        robotweax_srt_telemetry_ext_delta_v1 ext_delta;
    } robotweax_srt_telemetry_source_output_v1;

    /* Fills one sample for the given timestamps. Any error other than OK is
     * reported through on_error and the tick continues with other sources.
     * The source's windows still advance, flagged WINDOW_SOURCE_ERROR, so an
     * outage shows up as flagged windows rather than as silence. on_error
     * fires at once for a new error code and at most once per second while
     * the same error repeats. */
    typedef robotweax_srt_telemetry_error (*robotweax_srt_telemetry_source_fn)(
        void *source_data, uint64_t observed_at_unix_nano, uint64_t sample_monotonic_nano,
        robotweax_srt_telemetry_source_output_v1 *output);

    typedef struct robotweax_srt_telemetry_source_config_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        robotweax_srt_telemetry_source_fn sample;
        void *source_data;
        robotweax_srt_telemetry_endpoint_role role;
        uint32_t label_count;
        const robotweax_srt_telemetry_label_v1 *labels;
    } robotweax_srt_telemetry_source_config_v1;

    /* Passed to callbacks; pointers are valid only during the callback. */
    typedef struct robotweax_srt_telemetry_source_info_v1
    {
        uint64_t source_id;
        robotweax_srt_telemetry_endpoint_role role;
        uint32_t label_count;
        const robotweax_srt_telemetry_label_v1 *labels;
    } robotweax_srt_telemetry_source_info_v1;

    /* Callbacks run on the sampling thread (or inside sampler_tick) while the
     * sampler holds its internal lock. They must not call sampler functions
     * (such a call returns INVALID_STATE) and must not throw. Do not hold a
     * lock that a callback takes while calling remove_source, stop, tick or
     * destroy, or the two can deadlock. Any callback may be NULL. */
    typedef struct robotweax_srt_telemetry_sampler_callbacks_v1
    {
        void *user_data;
        void (*on_sample)(void *user_data, const robotweax_srt_telemetry_source_info_v1 *source,
                          const robotweax_srt_telemetry_source_output_v1 *output,
                          const robotweax_srt_telemetry_metric_sample_v1 *metrics);
        void (*on_window)(void *user_data, const robotweax_srt_telemetry_source_info_v1 *source,
                          const robotweax_srt_telemetry_window_summary_v1 *window);
        void (*on_error)(void *user_data, const robotweax_srt_telemetry_source_info_v1 *source,
                         robotweax_srt_telemetry_error error, int32_t native_result);
    } robotweax_srt_telemetry_sampler_callbacks_v1;

    typedef struct robotweax_srt_telemetry_sampler_config_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        /* Period of the internal thread; also the windows' expected period. */
        uint64_t period_nano;
        robotweax_srt_telemetry_window_config_v1 windows;
        robotweax_srt_telemetry_sampler_callbacks_v1 callbacks;
    } robotweax_srt_telemetry_sampler_config_v1;

    typedef struct robotweax_srt_telemetry_sampler robotweax_srt_telemetry_sampler;

    /* 100 ms period, default windows, no callbacks. */
    ROBOTWEAX_SRT_TELEMETRY_API void
    robotweax_srt_telemetry_sampler_config_default(robotweax_srt_telemetry_sampler_config_v1 *config);

    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_create(
        const robotweax_srt_telemetry_sampler_config_v1 *config,
        robotweax_srt_telemetry_sampler **sampler);

    /* Stops the thread if running, flushes every source's windows (PARTIAL)
     * and frees the sampler. Must not run concurrently with any other call on
     * the same sampler. A call from one of its own callbacks is ignored (the
     * sampler stays alive) because it would deadlock. */
    ROBOTWEAX_SRT_TELEMETRY_API void
    robotweax_srt_telemetry_sampler_destroy(robotweax_srt_telemetry_sampler *sampler);

    /* Thread-safe. The source is sampled from the next tick on. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_sampler_add_source(robotweax_srt_telemetry_sampler *sampler,
                                               const robotweax_srt_telemetry_source_config_v1 *source,
                                               uint64_t *source_id);

    /* Thread-safe. Waits for a running tick to finish, flushes the source's
     * windows (PARTIAL) and forgets it. After it returns the source callback
     * is never called again, so the host may destroy its SRT session and close
     * the socket. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_sampler_remove_source(robotweax_srt_telemetry_sampler *sampler,
                                                  uint64_t source_id);

    ROBOTWEAX_SRT_TELEMETRY_API size_t
    robotweax_srt_telemetry_sampler_source_count(robotweax_srt_telemetry_sampler *sampler);

    /* Samples every source once with the given timestamps. For hosts with
     * their own loop and for tests. INVALID_STATE while the thread runs. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_sampler_tick(robotweax_srt_telemetry_sampler *sampler,
                                         uint64_t observed_at_unix_nano,
                                         uint64_t sample_monotonic_nano);

    /* Starts the sampling thread. Ticks are scheduled on the steady clock at
     * multiples of the period; when a tick overruns, missed ticks are skipped
     * (the windows then flag a sampling gap) rather than bunched together. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_sampler_start(robotweax_srt_telemetry_sampler *sampler);

    /* Stops and joins the thread. Windows stay open; a later start continues
     * them. Safe to call from several threads; only one joins, the others
     * return at once. start is refused (INVALID_STATE) until the join is
     * complete. Must not be called from a callback. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_sampler_stop(robotweax_srt_telemetry_sampler *sampler);

    /* ---------------------------------------------------- history, events */

    /* Keeps a ring of compact records per source, one per closed window of
     * the shortest configured window (1 s by default). 86,400 records hold
     * 24 h at 1 s, about 9.7 MB per source. Applies to current and future
     * sources; calling it again resizes and clears every history. 0 turns
     * history off. A source's history is discarded when it is removed. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_sampler_enable_history(robotweax_srt_telemetry_sampler *sampler,
                                                   size_t records_per_source);

    /* Copies up to capacity records of one source whose window starts at or
     * after since_start_unix_nano, in the order the windows closed (time
     * order unless the wall clock stepped back). Pass 0 for everything.
     * Returns INVALID_STATE from the sampler's own callbacks. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_sampler_history_read(robotweax_srt_telemetry_sampler *sampler,
                                                 uint64_t source_id,
                                                 uint64_t since_start_unix_nano,
                                                 robotweax_srt_telemetry_compact_record_v1 *records,
                                                 size_t capacity, size_t *count);

    /* Creates the event journal (capacity events). The sampler then records
     * SOURCE_ADDED/REMOVED/FAILED/RECOVERED and, from the shortest window,
     * SAMPLING_GAP, CLOCK_STEP and COUNTER_RESET (once per episode). Calling
     * it again replaces the journal and drops its events; 0 turns it off.
     * Sequence numbers continue across replacement, so readers' cursors stay
     * valid. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_sampler_enable_journal(robotweax_srt_telemetry_sampler *sampler,
                                                   size_t capacity);

    /* Reads the sampler's journal like event_journal_read: events with a
     * sequence greater than after_sequence, oldest first. overwritten
     * (optional) receives how many events the full journal has dropped.
     * Safe from any thread, including callbacks; it does not take the
     * sampler lock. NOT_FOUND without a journal. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_sampler_journal_read(robotweax_srt_telemetry_sampler *sampler,
                                                 uint64_t after_sequence,
                                                 robotweax_srt_telemetry_event_v1 *events,
                                                 size_t capacity, size_t *count,
                                                 uint64_t *overwritten);

    /* Adds a host event, e.g. converted from the connection monitor. Safe
     * from any thread, including callbacks. NOT_FOUND without a journal. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_sampler_record_event(robotweax_srt_telemetry_sampler *sampler,
                                                 const robotweax_srt_telemetry_event_v1 *event);

    /* ------------------------------------------------------------- health */

    /* Called after a sample when a source's verdict, reason or advice changed.
     * Same rules as the other callbacks. */
    typedef void (*robotweax_srt_telemetry_health_callback)(
        void *user_data, const robotweax_srt_telemetry_source_info_v1 *source,
        const robotweax_srt_telemetry_health_status_v1 *status);

    /* Runs a health engine per source (current and future), with the default
     * health configuration for the source's role. Non-zero enables; zero
     * disables and discards the engines. With health on, verdict changes are
     * journaled (VERDICT_CHANGED) and the compact history records each
     * window's worst verdict. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_sampler_enable_health(robotweax_srt_telemetry_sampler *sampler,
                                                  int enabled);

    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_set_health_callback(
        robotweax_srt_telemetry_sampler *sampler, robotweax_srt_telemetry_health_callback callback,
        void *user_data);

    /* Overrides thresholds for every source of a role, current and future. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_set_health_thresholds(
        robotweax_srt_telemetry_sampler *sampler, robotweax_srt_telemetry_endpoint_role role,
        robotweax_srt_telemetry_health_indicator indicator, double watch, double at_risk, double failing);

    /* Connection state and settings for one source, e.g. from the public SRT
     * adapter's robotweax_srt_telemetry_public_srt_health_context. Kept
     * while health is disabled and applied when it is enabled. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_set_health_context(
        robotweax_srt_telemetry_sampler *sampler, uint64_t source_id,
        const robotweax_srt_telemetry_health_context_v1 *context);

    /* The status after the source's latest sample. NOT_FOUND for an unknown
     * source; INVALID_STATE when health is off or from a callback. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_sampler_health_read(robotweax_srt_telemetry_sampler *sampler,
                                                uint64_t source_id,
                                                robotweax_srt_telemetry_health_status_v1 *status);

    /* --------------------------------------------------- snapshots, export */

    /* Copies up to capacity source IDs in ascending order; count receives
     * the number copied. BUFFER_TOO_SMALL when more sources exist. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_source_ids(
        robotweax_srt_telemetry_sampler *sampler, uint64_t *ids, size_t capacity, size_t *count);

    /* The latest closed window of a configured length (0: 10 s if
     * configured, otherwise the shortest). NOT_FOUND before the first one
     * closes; INVALID_ARGUMENT for a length that is not configured. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_window_read(
        robotweax_srt_telemetry_sampler *sampler, uint64_t source_id, uint64_t window_nano,
        robotweax_srt_telemetry_window_summary_v1 *summary);

    /* The metrics of the latest successful sample. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_latest_metrics(
        robotweax_srt_telemetry_sampler *sampler, uint64_t source_id, robotweax_srt_telemetry_metric_sample_v1 *metrics);

    /* Renders every source (labels, latest window, lifetime counters,
     * health) in one format; see srt_telemetry_export.h. State is copied
     * under the sampler lock; the writer runs after it is released, once,
     * and may call the sampler. Not from the sampler's own callbacks. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_render_to(
        robotweax_srt_telemetry_sampler *sampler, const robotweax_srt_telemetry_render_options_v1 *options,
        robotweax_srt_telemetry_render_writer writer, void *user_data);

    /* As render_to, into a buffer, NUL-terminated. length receives the
     * output length without the terminator; BUFFER_TOO_SMALL when it does
     * not fit (render again with at least length + 1 bytes; the size can
     * change between calls). */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_render(
        robotweax_srt_telemetry_sampler *sampler, const robotweax_srt_telemetry_render_options_v1 *options,
        char *buffer, size_t capacity, size_t *length);

#ifdef __cplusplus
}
#endif

#endif
