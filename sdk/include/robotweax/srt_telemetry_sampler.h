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
     * after since_start_unix_nano, oldest first. Pass 0 for everything. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_sampler_history_read(robotweax_srt_telemetry_sampler *sampler,
                                                 uint64_t source_id,
                                                 uint64_t since_start_unix_nano,
                                                 robotweax_srt_telemetry_compact_record_v1 *records,
                                                 size_t capacity, size_t *count);

    /* Creates the event journal (capacity events). The sampler then records
     * SOURCE_ADDED/REMOVED/FAILED/RECOVERED and, from the shortest window,
     * SAMPLING_GAP, CLOCK_STEP and COUNTER_RESET. Calling it again replaces
     * the journal; 0 turns it off. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_sampler_enable_journal(robotweax_srt_telemetry_sampler *sampler,
                                                   size_t capacity);

    /* The sampler's journal, or NULL when disabled. Owned by the sampler and
     * valid until enable_journal is called again or the sampler is destroyed.
     * Reading it is thread-safe and does not take the sampler lock. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_event_journal *
    robotweax_srt_telemetry_sampler_journal(robotweax_srt_telemetry_sampler *sampler);

    /* Adds a host event, e.g. converted from the connection monitor. Safe
     * from any thread, including callbacks. NOT_FOUND without a journal. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_sampler_record_event(robotweax_srt_telemetry_sampler *sampler,
                                                 const robotweax_srt_telemetry_event_v1 *event);

#ifdef __cplusplus
}
#endif

#endif
