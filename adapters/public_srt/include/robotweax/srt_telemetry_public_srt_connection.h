#ifndef ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_CONNECTION_H
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_CONNECTION_H

/* Connection monitor: socket state, reject reason, negotiated options and
 * encryption state of one SRT socket, read through the public SRT API that
 * the host injects. Poll it at a low rate (about 1 Hz) or on demand; each poll
 * makes one srt_getsockstate, one srt_getrejectreason and about 25
 * srt_getsockflag calls. */

#include <robotweax/srt_telemetry_history.h>
#include <robotweax/srt_telemetry_public_srt.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Host-side SRT functions with plain integer types, so this header does
     * not depend on srt.h. <robotweax/srt_telemetry_public_srt_host.h>
     * provides inline wrappers around the host's own SRT library. */
    typedef int (*robotweax_srt_telemetry_public_srt_getsockstate_fn)(int32_t socket);
    typedef int (*robotweax_srt_telemetry_public_srt_getrejectreason_fn)(int32_t socket);
    typedef int (*robotweax_srt_telemetry_public_srt_getsockflag_fn)(int32_t socket, int option,
                                                                     void *value, int *length);

/* Values of state follow SRT_SOCKSTATUS: 1 INIT, 2 OPENED, 3 LISTENING,
 * 4 CONNECTING, 5 CONNECTED, 6 BROKEN, 7 CLOSING, 8 CLOSED, 9 NONEXIST.
 * Providers differ after failures (e.g. after a connect timeout Haivision
 * reports OPENED, Robotweax BROKEN); the monitor reports what the provider
 * says. */
#define ROBOTWEAX_SRT_TELEMETRY_CONNECTION_STATE_CONNECTED 5

/* km_state values follow SRT_KM_STATE: 0 UNSECURED, 1 SECURING, 2 SECURED,
 * 3 NOSECRET, 4 BADSECRET, 5 BADCRYPTOMODE. */

#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_STATE UINT64_C(1)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_REJECT_REASON (UINT64_C(1) << 1)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_RECEIVER_LATENCY_SECONDS (UINT64_C(1) << 2)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PEER_LATENCY_SECONDS (UINT64_C(1) << 3)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_OVERHEAD_PERCENT (UINT64_C(1) << 4)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_MAX_BANDWIDTH (UINT64_C(1) << 5)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_INPUT_BANDWIDTH (UINT64_C(1) << 6)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_MIN_INPUT_BANDWIDTH (UINT64_C(1) << 7)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_LOSS_MAX_TTL_PACKETS (UINT64_C(1) << 8)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_SENDER_DROP_DELAY (UINT64_C(1) << 9)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_TOO_LATE_PACKET_DROP (UINT64_C(1) << 10)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_TSBPD_MODE (UINT64_C(1) << 11)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PERIODIC_NAK_REPORT (UINT64_C(1) << 12)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PAYLOAD_SIZE_BYTES (UINT64_C(1) << 13)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_FLOW_CONTROL_PACKETS (UINT64_C(1) << 14)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PEER_SRT_VERSION (UINT64_C(1) << 15)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_LOCAL_SRT_VERSION (UINT64_C(1) << 16)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_KM_STATE (UINT64_C(1) << 17)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_SEND_KM_STATE (UINT64_C(1) << 18)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_RECEIVE_KM_STATE (UINT64_C(1) << 19)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_KEY_LENGTH_BYTES (UINT64_C(1) << 20)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_ENFORCED_ENCRYPTION (UINT64_C(1) << 21)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_KM_REFRESH_RATE_PACKETS (UINT64_C(1) << 22)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_KM_PREANNOUNCE_PACKETS (UINT64_C(1) << 23)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PACKET_FILTER (UINT64_C(1) << 24)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_STREAM_ID (UINT64_C(1) << 25)

/* Fields whose change means the transport configuration changed. */
#define ROBOTWEAX_SRT_TELEMETRY_CONN_SETTINGS_FIELDS                                               \
    (ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_RECEIVER_LATENCY_SECONDS |                                 \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PEER_LATENCY_SECONDS |                                     \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_OVERHEAD_PERCENT |                                         \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_MAX_BANDWIDTH |                                            \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_INPUT_BANDWIDTH |                                          \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_MIN_INPUT_BANDWIDTH |                                      \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_LOSS_MAX_TTL_PACKETS |                                     \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_SENDER_DROP_DELAY |                                        \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_TOO_LATE_PACKET_DROP |                                     \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_TSBPD_MODE |                                               \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PERIODIC_NAK_REPORT |                                      \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PAYLOAD_SIZE_BYTES |                                       \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_FLOW_CONTROL_PACKETS |                                     \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_KEY_LENGTH_BYTES |                                         \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_ENFORCED_ENCRYPTION |                                      \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_KM_REFRESH_RATE_PACKETS |                                  \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_KM_PREANNOUNCE_PACKETS |                                   \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PACKET_FILTER)

#define ROBOTWEAX_SRT_TELEMETRY_CONN_KM_FIELDS                                                     \
    (ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_KM_STATE | ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_SEND_KM_STATE | \
     ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_RECEIVE_KM_STATE)

/* Events derived from consecutive snapshots. KM_STATE_CHANGED,
 * SETTINGS_CHANGED and PEER_VERSION_CHANGED come only from value changes of
 * fields readable in both polls; a closing socket that makes options
 * unreadable shows up in changed_fields and as DISCONNECTED, not as a
 * renegotiation. REJECTED means the reject reason changed to a non-zero
 * value (SRT_REJ_* includes timeouts and closes, not only refusals); a
 * repeated rejection with the same code is not reported again. Monitor
 * caller or accepted sockets; on a listening socket SRT stores the latest
 * incoming rejection, which this event does not track per caller. */
#define ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_FIRST_SNAPSHOT UINT64_C(1)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_STATE_CHANGED (UINT64_C(1) << 1)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_CONNECTED (UINT64_C(1) << 2)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_DISCONNECTED (UINT64_C(1) << 3)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_REJECTED (UINT64_C(1) << 4)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_KM_STATE_CHANGED (UINT64_C(1) << 5)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_SETTINGS_CHANGED (UINT64_C(1) << 6)
#define ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_PEER_VERSION_CHANGED (UINT64_C(1) << 7)

#define ROBOTWEAX_SRT_TELEMETRY_CONN_TEXT_CAPACITY 520u

    typedef struct robotweax_srt_telemetry_public_srt_connection_config_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        int32_t socket;
        robotweax_srt_telemetry_provider provider;
        robotweax_srt_telemetry_public_srt_getsockstate_fn getsockstate;
        robotweax_srt_telemetry_public_srt_getrejectreason_fn getrejectreason;
        robotweax_srt_telemetry_public_srt_getsockflag_fn getsockflag;
        /* Stream IDs can identify customers or channels. Off by default; when
         * zero the stream ID is never read and stays unavailable. */
        uint32_t capture_stream_id;
        /* Must be zero. */
        uint32_t reserved;
    } robotweax_srt_telemetry_public_srt_connection_config_v1;

    typedef struct robotweax_srt_telemetry_connection_snapshot_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        uint64_t available_fields;
        uint64_t observed_at_unix_nano;
        uint64_t sample_monotonic_nano;

        int32_t state;
        /* SRT_REJ_* value; 0 means no reject reason recorded. */
        int32_t reject_reason;

        double receiver_latency_seconds;
        double peer_latency_seconds;
        /* Retransmission overhead allowance over the input rate, percent. */
        int64_t overhead_percent;
        /* SRTO_MAXBW in bytes/s: -1 unlimited (default), 0 relative to input. */
        int64_t max_bandwidth_bytes_per_second;
        /* SRTO_INPUTBW in bytes/s; 0 means estimated by SRT. */
        int64_t input_bandwidth_bytes_per_second;
        int64_t min_input_bandwidth_bytes_per_second;
        int64_t loss_max_ttl_packets;
        /* Extra delay before sender too-late drop; negative = sender drop off. */
        double sender_drop_delay_seconds;
        uint32_t too_late_packet_drop;
        uint32_t tsbpd_mode;
        uint32_t periodic_nak_report;
        uint32_t enforced_encryption;
        int64_t payload_size_bytes;
        int64_t flow_control_packets;
        /* SRT versions as 0xXXYYZZ, e.g. 0x010505 for 1.5.5. */
        uint32_t peer_srt_version;
        uint32_t local_srt_version;

        int32_t km_state;
        int32_t send_km_state;
        int32_t receive_km_state;
        /* AES key length in bytes; 0 when unencrypted. */
        int32_t key_length_bytes;
        int64_t km_refresh_rate_packets;
        int64_t km_preannounce_packets;

        /* Negotiated packet-filter configuration, e.g.
         * "fec,arq:onreq,cols:4,layout:staircase,rows:2"; empty without FEC. */
        char packet_filter[ROBOTWEAX_SRT_TELEMETRY_CONN_TEXT_CAPACITY];
        char stream_id[ROBOTWEAX_SRT_TELEMETRY_CONN_TEXT_CAPACITY];
    } robotweax_srt_telemetry_connection_snapshot_v1;

    typedef struct robotweax_srt_telemetry_public_srt_connection_result_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        robotweax_srt_telemetry_connection_snapshot_v1 snapshot;
        /* CONN_FIELD_* bits whose value or availability differs from the
         * previous poll; zero on the first poll. */
        uint64_t changed_fields;
        /* CONN_EVENT_* bits derived from the change. */
        uint64_t events;
        int32_t previous_state;
        int32_t previous_km_state;
    } robotweax_srt_telemetry_public_srt_connection_result_v1;

    typedef struct robotweax_srt_telemetry_public_srt_connection_monitor
        robotweax_srt_telemetry_public_srt_connection_monitor;

    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_public_srt_connection_monitor_create(
        const robotweax_srt_telemetry_public_srt_connection_config_v1 *config,
        robotweax_srt_telemetry_public_srt_connection_monitor **monitor);

    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API void
    robotweax_srt_telemetry_public_srt_connection_monitor_destroy(
        robotweax_srt_telemetry_public_srt_connection_monitor *monitor);

    /* Reads a snapshot and compares it with the previous one. A socket that
     * no longer exists still yields a snapshot (state NONEXIST, options
     * unavailable) so the disconnect is reported, not swallowed. Failed option
     * reads set the calling thread's srt_getlasterror in both providers. Calls
     * on one monitor are externally synchronized. */
    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_public_srt_connection_monitor_poll(
        robotweax_srt_telemetry_public_srt_connection_monitor *monitor,
        uint64_t observed_at_unix_nano, uint64_t sample_monotonic_nano,
        robotweax_srt_telemetry_public_srt_connection_result_v1 *result);

    /* Converts a poll result into journal events for source_id (at most 5):
     * CONNECTED and DISCONNECTED (previous/current socket state), REJECTED
     * (current = reject reason), KM_STATE_CHANGED (previous/current KM state;
     * WARNING for NOSECRET, BADSECRET or BADCRYPTOMODE), SETTINGS_CHANGED
     * (code = changed settings fields) and PEER_VERSION_CHANGED (current =
     * peer version). A first poll yields only CONNECTED or REJECTED. */
    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_public_srt_connection_events(
        const robotweax_srt_telemetry_public_srt_connection_result_v1 *result, uint64_t source_id,
        robotweax_srt_telemetry_event_v1 *events, size_t capacity, size_t *count);

#ifdef __cplusplus
}
#endif

#endif
