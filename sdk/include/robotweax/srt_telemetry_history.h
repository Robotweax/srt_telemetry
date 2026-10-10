#ifndef ROBOTWEAX_SRT_TELEMETRY_HISTORY_H
#define ROBOTWEAX_SRT_TELEMETRY_HISTORY_H

/* History and events.
 *
 * Compact record: one window (normally 1 s) of one socket in 112 bytes, enough
 * for every dashboard panel. 24 h at 1 s is 86,400 records, about 9.7 MB per
 * socket, so a product can keep a day of history without a backend.
 *
 * Event journal: a bounded, thread-safe ring of timestamped events (source
 * added or failed, connection changes, sampling gaps, host events) for the
 * dashboards' event timeline. */

#include <robotweax/srt_telemetry_metrics.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Availability bits of compact_record_v1 fields. */
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_UNIQUE_SENT_BPS UINT32_C(1)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_RETRANSMITTED_BPS (UINT32_C(1) << 1)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_UNIQUE_RECEIVED_BPS (UINT32_C(1) << 2)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_LINK_CAPACITY_BPS (UINT32_C(1) << 3)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_RTT (UINT32_C(1) << 4)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECOVERY_HEADROOM (UINT32_C(1) << 5)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_SEND_BUFFER (UINT32_C(1) << 6)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECEIVE_BUFFER (UINT32_C(1) << 7)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_SEND_BUFFER_RATIO (UINT32_C(1) << 8)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_FLIGHT_TO_FLOW (UINT32_C(1) << 9)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_SENDER_LOSS (UINT32_C(1) << 10)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECEIVER_LOSS (UINT32_C(1) << 11)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_RETRANSMITTED (UINT32_C(1) << 12)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECEIVED_RETRANSMITTED (UINT32_C(1) << 13)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_SENDER_DROP (UINT32_C(1) << 14)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECEIVER_DROP (UINT32_C(1) << 15)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_UNDECRYPTABLE (UINT32_C(1) << 16)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_BELATED (UINT32_C(1) << 17)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_FILTER_RECOVERED (UINT32_C(1) << 18)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_FILTER_UNRECOVERED (UINT32_C(1) << 19)

/* compact_record_v1.group: a connection group's own row, with its usable
 * and expected paths at the window's end (each saturating at 63). */
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_GROUP_ROW (UINT16_C(1) << 15)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_GROUP_USABLE_PATHS(group) ((unsigned)(group) & 63u)
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_GROUP_EXPECTED_PATHS(group) (((unsigned)(group) >> 6) & 63u)
/* A member path whose verdict was capped at Watch during the window. */
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_GROUP_CAPPED (UINT16_C(1) << 12)
/* A member path waiting as standby at the window's end. */
#define ROBOTWEAX_SRT_TELEMETRY_COMPACT_GROUP_STANDBY (UINT16_C(1) << 13)

    /* One window of one socket. Rates are window rates (exact totals over
     * covered time); packet counts are window totals. Floats keep about 7
     * significant digits. */
    typedef struct robotweax_srt_telemetry_compact_record_v1
    {
        uint64_t start_unix_nano;
        uint32_t window_milliseconds;
        /* WINDOW_* quality flags of the source window. */
        uint16_t quality_flags;
        uint16_t sample_count;
        uint32_t available;
        /* Worst health verdict and its reason during the window, when the
         * sampler runs the health engine (health_verdict / health_reason
         * values); 0 = unknown / none. */
        uint8_t health_verdict;
        uint8_t health_reason;
        /* COMPACT_GROUP_* bits for connection groups (0 otherwise). */
        uint16_t group;

        float unique_sent_bits_per_second;
        float retransmitted_bits_per_second;
        float unique_received_bits_per_second;
        float link_capacity_bits_per_second; /* mean */
        float rtt_mean_seconds;
        float rtt_max_seconds;
        float recovery_headroom_min;
        float recovery_headroom_mean;
        float send_buffer_max_seconds;
        float receive_buffer_min_seconds;
        float send_buffer_ratio_max;
        float flight_to_flow_max;

        uint32_t sender_loss_packets;
        uint32_t receiver_loss_packets;
        uint32_t retransmitted_packets;
        uint32_t received_retransmitted_packets;
        uint32_t sender_drop_packets;
        uint32_t receiver_drop_packets;
        uint32_t undecryptable_packets;
        uint32_t belated_packets;
        uint32_t filter_recovered_packets;
        uint32_t filter_unrecovered_packets;
    } robotweax_srt_telemetry_compact_record_v1;

    /* Converts a window summary. Counts above UINT32_MAX saturate. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error robotweax_srt_telemetry_compact_record_from_window(
        const robotweax_srt_telemetry_window_summary_v1 *window,
        robotweax_srt_telemetry_compact_record_v1 *record);

    /* ------------------------------------------------------------- events */

    typedef enum robotweax_srt_telemetry_event_kind ROBOTWEAX_SRT_TELEMETRY_ENUM_INT
    {
        ROBOTWEAX_SRT_TELEMETRY_EVENT_HOST = 0, /* host-defined; see code */
        ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_ADDED = 1,
        ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_REMOVED = 2,
        /* previous/current: error codes; native result in code */
        ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_FAILED = 3,
        ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_RECOVERED = 4,
        ROBOTWEAX_SRT_TELEMETRY_EVENT_SAMPLING_GAP = 5,
        ROBOTWEAX_SRT_TELEMETRY_EVENT_CLOCK_STEP = 6,
        ROBOTWEAX_SRT_TELEMETRY_EVENT_COUNTER_RESET = 7,
        /* Connection events (from the public SRT connection monitor);
         * previous/current: socket states, KM states, reject reasons or
         * versions as noted in the adapter. */
        ROBOTWEAX_SRT_TELEMETRY_EVENT_CONNECTED = 8,
        ROBOTWEAX_SRT_TELEMETRY_EVENT_DISCONNECTED = 9,
        ROBOTWEAX_SRT_TELEMETRY_EVENT_REJECTED = 10,
        ROBOTWEAX_SRT_TELEMETRY_EVENT_KM_STATE_CHANGED = 11,
        ROBOTWEAX_SRT_TELEMETRY_EVENT_SETTINGS_CHANGED = 12,
        ROBOTWEAX_SRT_TELEMETRY_EVENT_PEER_VERSION_CHANGED = 13,
        /* Reserved for the health engine. */
        ROBOTWEAX_SRT_TELEMETRY_EVENT_VERDICT_CHANGED = 14,
        /* Connection groups (public SRT group session), on the group's
         * source; code: member socket; detail: "path=<path>". */
        ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_ADDED = 15,
        ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_REMOVED = 16,
        /* On the member's source; previous/current: member states
         * (SRT_MEMBERSTATUS); code: member socket. */
        ROBOTWEAX_SRT_TELEMETRY_EVENT_MEMBER_STATE_CHANGED = 17,
        /* On the group's source; previous/current: member sockets (-1: none);
         * detail: "path=<previous> -> <current>". */
        ROBOTWEAX_SRT_TELEMETRY_EVENT_ACTIVE_MEMBER_CHANGED = 18,
        /* Observed local sender selection/revision change, including initial
         * selection and clear. Polling gaps do not reconstruct transitions. */
        ROBOTWEAX_SRT_TELEMETRY_EVENT_SEND_AUTHORITY_CHANGED = 19
    } robotweax_srt_telemetry_event_kind;

    typedef enum robotweax_srt_telemetry_event_severity ROBOTWEAX_SRT_TELEMETRY_ENUM_INT
    {
        ROBOTWEAX_SRT_TELEMETRY_SEVERITY_INFO = 1,
        ROBOTWEAX_SRT_TELEMETRY_SEVERITY_WARNING = 2,
        ROBOTWEAX_SRT_TELEMETRY_SEVERITY_ERROR = 3
    } robotweax_srt_telemetry_event_severity;

#define ROBOTWEAX_SRT_TELEMETRY_EVENT_DETAIL_CAPACITY 96u

    typedef struct robotweax_srt_telemetry_event_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        uint64_t observed_at_unix_nano;
        uint64_t sample_monotonic_nano;
        /* Sampler source ID; 0 when the event is not about one source. */
        uint64_t source_id;
        /* Assigned by the journal: increases by one per accepted event. */
        uint64_t sequence;
        robotweax_srt_telemetry_event_kind kind;
        robotweax_srt_telemetry_event_severity severity;
        int64_t previous_value;
        int64_t current_value;
        /* Kind-specific code, e.g. a changed-field mask or native result. */
        int64_t code;
        /* Printable ASCII, NUL-terminated; longer text is truncated. */
        char detail[ROBOTWEAX_SRT_TELEMETRY_EVENT_DETAIL_CAPACITY];
    } robotweax_srt_telemetry_event_v1;

    typedef struct robotweax_srt_telemetry_event_journal robotweax_srt_telemetry_event_journal;

    /* Bounded ring, allocated at creation; a full journal overwrites the
     * oldest event. All functions are thread-safe. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_event_journal_create(size_t capacity,
                                                 robotweax_srt_telemetry_event_journal **journal);

    ROBOTWEAX_SRT_TELEMETRY_API void
    robotweax_srt_telemetry_event_journal_destroy(robotweax_srt_telemetry_event_journal *journal);

    /* Copies the event, assigns its sequence and replaces non-printable
     * detail characters. sequence (optional) receives the assigned value. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_event_journal_push(robotweax_srt_telemetry_event_journal *journal,
                                               const robotweax_srt_telemetry_event_v1 *event,
                                               uint64_t *sequence);

    /* Copies up to capacity events with a sequence greater than after_sequence,
     * oldest first. count receives the number copied. Pass 0 to read from the
     * oldest retained event. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_event_journal_read(const robotweax_srt_telemetry_event_journal *journal,
                                               uint64_t after_sequence,
                                               robotweax_srt_telemetry_event_v1 *events,
                                               size_t capacity, size_t *count);

    ROBOTWEAX_SRT_TELEMETRY_API size_t
    robotweax_srt_telemetry_event_journal_size(const robotweax_srt_telemetry_event_journal *journal);

    ROBOTWEAX_SRT_TELEMETRY_API uint64_t robotweax_srt_telemetry_event_journal_overwritten_count(
        const robotweax_srt_telemetry_event_journal *journal);

    ROBOTWEAX_SRT_TELEMETRY_API const char *
    robotweax_srt_telemetry_event_kind_name(robotweax_srt_telemetry_event_kind kind);

#ifdef __cplusplus
}
#endif

#endif
