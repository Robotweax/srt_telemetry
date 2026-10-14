#ifndef ROBOTWEAX_SRT_TELEMETRY_H
#define ROBOTWEAX_SRT_TELEMETRY_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(ROBOTWEAX_SRT_TELEMETRY_SHARED)
#if defined(ROBOTWEAX_SRT_TELEMETRY_BUILDING)
#define ROBOTWEAX_SRT_TELEMETRY_API __declspec(dllexport)
#else
#define ROBOTWEAX_SRT_TELEMETRY_API __declspec(dllimport)
#endif
#else
#define ROBOTWEAX_SRT_TELEMETRY_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define ROBOTWEAX_SRT_TELEMETRY_API __attribute__((visibility("default")))
#else
#define ROBOTWEAX_SRT_TELEMETRY_API
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/* Every enum of this API has int as its fixed underlying type in C++. A C
 * host may pass any int in an enum field or parameter; with a fixed type that
 * value is a valid C++ enum value, so the library can read and reject it
 * without undefined behaviour. The representation is the same as in C. */
#ifdef __cplusplus
#define ROBOTWEAX_SRT_TELEMETRY_ENUM_INT : int
#else
#define ROBOTWEAX_SRT_TELEMETRY_ENUM_INT
#endif

#define ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION 1u
#define ROBOTWEAX_SRT_TELEMETRY_VERSION "0.1.0-alpha.0"

    typedef enum robotweax_srt_telemetry_error ROBOTWEAX_SRT_TELEMETRY_ENUM_INT
    {
        ROBOTWEAX_SRT_TELEMETRY_OK = 0,
        ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT = 1,
        ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH = 2,
        ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE = 3,
        ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE = 4,
        ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE = 5,
        /* The call is not allowed in the object's current state, e.g. a manual
         * tick while the sampler thread runs, or a call from a callback. */
        ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE = 6,
        ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND = 7,
        /* The output does not fit; the required size is reported. */
        ROBOTWEAX_SRT_TELEMETRY_BUFFER_TOO_SMALL = 8,
        /* An operating-system I/O call failed, e.g. a socket could not be
         * bound. */
        ROBOTWEAX_SRT_TELEMETRY_IO_FAILURE = 9
    } robotweax_srt_telemetry_error;

#ifndef __cplusplus
    /* The C++ side sees int-sized enums. A C host built with short enums
     * (-fshort-enums, some ARM EABI targets) would disagree on every struct
     * layout, so it fails to compile here instead. */
    typedef char robotweax_srt_telemetry_enums_must_be_int_sized
        [sizeof(robotweax_srt_telemetry_error) == sizeof(int) ? 1 : -1];
#endif

    typedef enum robotweax_srt_telemetry_provider ROBOTWEAX_SRT_TELEMETRY_ENUM_INT
    {
        ROBOTWEAX_SRT_TELEMETRY_PROVIDER_UNKNOWN = 0,
        ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT = 1,
        ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT = 2
    } robotweax_srt_telemetry_provider;

/* Provider capabilities. Capabilities describe evidence-backed availability,
 * not values inferred from the provider name. */
#define ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1 UINT64_C(1)
#define ROBOTWEAX_SRT_TELEMETRY_CAP_FEC_STATS_V1 (UINT64_C(1) << 1)
#define ROBOTWEAX_SRT_TELEMETRY_CAP_GROUP_MEMBERS_V1 (UINT64_C(1) << 2)
#define ROBOTWEAX_SRT_TELEMETRY_CAP_ROBOTWEAX_EXTENSIONS_V1 (UINT64_C(1) << 3)
#define ROBOTWEAX_SRT_TELEMETRY_CAP_GROUP_PATH_IDENTIFIERS_V1 (UINT64_C(1) << 4)

/* Raw observation availability. A clear bit means unavailable; it never means
 * that a zero value was observed. Durations use seconds and byte values use
 * octets so the contract maps directly to OpenTelemetry UCUM units. */
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL UINT64_C(1)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_PACKET_TOTAL (UINT64_C(1) << 1)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL (UINT64_C(1) << 2)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL (UINT64_C(1) << 3)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_LOSS_PACKET_TOTAL (UINT64_C(1) << 4)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_LOSS_PACKET_TOTAL (UINT64_C(1) << 5)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_PACKET_TOTAL (UINT64_C(1) << 6)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_DROP_PACKET_TOTAL (UINT64_C(1) << 7)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_DROP_PACKET_TOTAL (UINT64_C(1) << 8)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_UNDECRYPTABLE_PACKET_TOTAL (UINT64_C(1) << 9)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL (UINT64_C(1) << 10)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_WIRE_BYTE_TOTAL (UINT64_C(1) << 11)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL (UINT64_C(1) << 12)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL (UINT64_C(1) << 13)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_WIRE_BYTE_TOTAL (UINT64_C(1) << 14)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS (UINT64_C(1) << 15)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_FLIGHT_SIZE_PACKETS (UINT64_C(1) << 16)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_FLOW_WINDOW_PACKETS (UINT64_C(1) << 17)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_CONGESTION_WINDOW_PACKETS (UINT64_C(1) << 18)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_AVAILABLE_SEND_BUFFER_BYTES (UINT64_C(1) << 19)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_AVAILABLE_RECEIVE_BUFFER_BYTES (UINT64_C(1) << 20)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_BYTES (UINT64_C(1) << 21)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_SECONDS (UINT64_C(1) << 22)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_BYTES (UINT64_C(1) << 23)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_SECONDS (UINT64_C(1) << 24)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_TSBPD_DELAY_SECONDS (UINT64_C(1) << 25)
#define ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_TSBPD_DELAY_SECONDS (UINT64_C(1) << 26)

#define ROBOTWEAX_SRT_TELEMETRY_RATE_SENT_WIRE_BITS_PER_SECOND UINT64_C(1)
#define ROBOTWEAX_SRT_TELEMETRY_RATE_RECEIVED_WIRE_BITS_PER_SECOND (UINT64_C(1) << 1)
#define ROBOTWEAX_SRT_TELEMETRY_RATE_UNIQUE_SENT_WIRE_BITS_PER_SECOND (UINT64_C(1) << 2)
#define ROBOTWEAX_SRT_TELEMETRY_RATE_UNIQUE_RECEIVED_WIRE_BITS_PER_SECOND (UINT64_C(1) << 3)

#define ROBOTWEAX_SRT_TELEMETRY_QUALITY_FIRST_SAMPLE UINT64_C(1)
#define ROBOTWEAX_SRT_TELEMETRY_QUALITY_MONOTONIC_CLOCK_REGRESSION (UINT64_C(1) << 1)
#define ROBOTWEAX_SRT_TELEMETRY_QUALITY_PROVIDER_CHANGED (UINT64_C(1) << 2)
#define ROBOTWEAX_SRT_TELEMETRY_QUALITY_COUNTER_RESET (UINT64_C(1) << 3)

    typedef struct robotweax_srt_telemetry_raw_sample_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        robotweax_srt_telemetry_provider provider;
        uint32_t reserved;
        uint64_t capabilities;
        uint64_t available_fields;
        uint64_t observed_at_unix_nano;
        uint64_t sample_monotonic_nano;

        uint64_t sent_packet_total;
        uint64_t received_packet_total;
        uint64_t unique_sent_packet_total;
        uint64_t unique_received_packet_total;
        uint64_t sender_loss_packet_total;
        uint64_t receiver_loss_packet_total;
        uint64_t retransmitted_packet_total;
        /* Local send/retransmission-buffer removals due to TTL or TLPKTDROP.
         * Does not by itself establish network loss or application payload loss. */
        uint64_t sender_drop_packet_total;
        uint64_t receiver_drop_packet_total;
        uint64_t receiver_undecryptable_packet_total;

        uint64_t sent_wire_byte_total;
        uint64_t received_wire_byte_total;
        uint64_t unique_sent_wire_byte_total;
        uint64_t unique_received_wire_byte_total;
        uint64_t retransmitted_wire_byte_total;

        double rtt_seconds;
        uint64_t flight_size_packets;
        uint64_t flow_window_packets;
        uint64_t congestion_window_packets;
        uint64_t available_send_buffer_bytes;
        uint64_t available_receive_buffer_bytes;
        uint64_t send_buffer_bytes;
        double send_buffer_seconds;
        uint64_t receive_buffer_bytes;
        double receive_buffer_seconds;
        double sender_tsbpd_delay_seconds;
        double receiver_tsbpd_delay_seconds;
    } robotweax_srt_telemetry_raw_sample_v1;

    typedef struct robotweax_srt_telemetry_delta_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        uint64_t quality_flags;
        uint64_t interval_nano;
        uint64_t available_delta_fields;
        uint64_t counter_reset_fields;
        uint64_t available_rates;

        uint64_t sent_packet_delta;
        uint64_t received_packet_delta;
        uint64_t unique_sent_packet_delta;
        uint64_t unique_received_packet_delta;
        uint64_t sender_loss_packet_delta;
        uint64_t receiver_loss_packet_delta;
        uint64_t retransmitted_packet_delta;
        /* Interval change in sender_drop_packet_total, with the same semantics. */
        uint64_t sender_drop_packet_delta;
        uint64_t receiver_drop_packet_delta;
        uint64_t receiver_undecryptable_packet_delta;

        uint64_t sent_wire_byte_delta;
        uint64_t received_wire_byte_delta;
        uint64_t unique_sent_wire_byte_delta;
        uint64_t unique_received_wire_byte_delta;
        uint64_t retransmitted_wire_byte_delta;

        double sent_wire_bits_per_second;
        double received_wire_bits_per_second;
        double unique_sent_wire_bits_per_second;
        double unique_received_wire_bits_per_second;
    } robotweax_srt_telemetry_delta_v1;

    typedef struct robotweax_srt_telemetry_record_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        robotweax_srt_telemetry_raw_sample_v1 raw;
        robotweax_srt_telemetry_delta_v1 delta;
    } robotweax_srt_telemetry_record_v1;

/* Extended statistics (supplement to raw_sample_v1). Together with the v1 raw
 * sample these cover every distinct observation of the public SRT_TRACEBSTATS
 * surface except the two interval rates (mbpsSendRate, mbpsRecvRate), which
 * the delta engine derives from lifetime byte counters instead. A clear bit
 * means unavailable, never an observed zero. See the field map for sources. */
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_CONNECTION_AGE_SECONDS UINT64_C(1)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENT_ACK_PACKET_TOTAL (UINT64_C(1) << 1)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_ACK_PACKET_TOTAL (UINT64_C(1) << 2)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENT_NAK_PACKET_TOTAL (UINT64_C(1) << 3)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_NAK_PACKET_TOTAL (UINT64_C(1) << 4)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENDER_ACTIVE_NANO_TOTAL (UINT64_C(1) << 5)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVER_LOSS_WIRE_BYTE_TOTAL (UINT64_C(1) << 6)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENDER_DROP_WIRE_BYTE_TOTAL (UINT64_C(1) << 7)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVER_DROP_WIRE_BYTE_TOTAL (UINT64_C(1) << 8)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVER_UNDECRYPTABLE_PAYLOAD_BYTE_TOTAL (UINT64_C(1) << 9)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENT_FILTER_EXTRA_PACKET_TOTAL (UINT64_C(1) << 10)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_FILTER_EXTRA_PACKET_TOTAL (UINT64_C(1) << 11)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_FILTER_RECOVERED_PACKET_TOTAL (UINT64_C(1) << 12)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_FILTER_UNRECOVERED_PACKET_TOTAL (UINT64_C(1) << 13)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_RETRANSMITTED_PACKET_SINCE_CLEAR (UINT64_C(1) << 14)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_BELATED_PACKET_SINCE_CLEAR (UINT64_C(1) << 15)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_REORDER_DISTANCE_PACKETS (UINT64_C(1) << 16)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_PACKET_SEND_PERIOD_SECONDS (UINT64_C(1) << 17)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_ESTIMATED_LINK_CAPACITY_BITS_PER_SECOND (UINT64_C(1) << 18)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_DATA_ARRIVAL_RATE_BITS_PER_SECOND (UINT64_C(1) << 19)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SEND_BANDWIDTH_LIMIT_BITS_PER_SECOND (UINT64_C(1) << 20)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_MSS_BYTES (UINT64_C(1) << 21)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SEND_BUFFER_PACKETS (UINT64_C(1) << 22)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVE_BUFFER_PACKETS (UINT64_C(1) << 23)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_REORDER_TOLERANCE_PACKETS (UINT64_C(1) << 24)
#define ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_AVERAGE_BELATED_SECONDS (UINT64_C(1) << 25)

/* Ext fields that are monotonic counters and therefore produce deltas. The
 * *_SINCE_CLEAR counters accumulate since connection start unless the host
 * itself clears SRT interval statistics. A host clear is detected only when
 * the counter is lower than at the previous sample (best effort): an ext reset
 * sets ext_delta.counter_reset_fields but not the shared v1 quality flag. */
#define ROBOTWEAX_SRT_TELEMETRY_EXT_COUNTER_FIELDS                                                 \
    (ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENT_ACK_PACKET_TOTAL |                                     \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_ACK_PACKET_TOTAL |                                 \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENT_NAK_PACKET_TOTAL |                                     \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_NAK_PACKET_TOTAL |                                 \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENDER_ACTIVE_NANO_TOTAL |                                  \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVER_LOSS_WIRE_BYTE_TOTAL |                             \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENDER_DROP_WIRE_BYTE_TOTAL |                               \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVER_DROP_WIRE_BYTE_TOTAL |                             \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVER_UNDECRYPTABLE_PAYLOAD_BYTE_TOTAL |                 \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENT_FILTER_EXTRA_PACKET_TOTAL |                            \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_FILTER_EXTRA_PACKET_TOTAL |                        \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_FILTER_RECOVERED_PACKET_TOTAL |                             \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_FILTER_UNRECOVERED_PACKET_TOTAL |                           \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_RETRANSMITTED_PACKET_SINCE_CLEAR |                 \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_BELATED_PACKET_SINCE_CLEAR)

    typedef struct robotweax_srt_telemetry_ext_sample_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        uint64_t available_fields;

        /* Time since the SRT socket (or group) connected; transport-local. */
        double connection_age_seconds;

        uint64_t sent_ack_packet_total;
        uint64_t received_ack_packet_total;
        uint64_t sent_nak_packet_total;
        uint64_t received_nak_packet_total;
        /* Time the sender had unacknowledged or queued DATA. */
        uint64_t sender_active_nano_total;
        /* Estimated from the average packet size by the provider. */
        uint64_t receiver_loss_wire_byte_total;
        uint64_t sender_drop_wire_byte_total;
        /* Receiver drop bytes are estimated from the average packet size. */
        uint64_t receiver_drop_wire_byte_total;
        /* Protected payload only, without IP/UDP/SRT headers. */
        uint64_t receiver_undecryptable_payload_byte_total;
        /* Packet-filter (FEC) counters; zero when no filter is negotiated. */
        uint64_t sent_filter_extra_packet_total;
        uint64_t received_filter_extra_packet_total;
        uint64_t filter_recovered_packet_total;
        uint64_t filter_unrecovered_packet_total;
        uint64_t received_retransmitted_packet_since_clear;
        uint64_t belated_packet_since_clear;
        /* Recent reorder distance; decays with the tolerance estimator and is
         * not reset by an interval clear. */
        uint64_t reorder_distance_packets;

        double packet_send_period_seconds;
        /* Haivision SRT: link capacity estimated from probe packet pairs. */
        double estimated_link_capacity_bits_per_second;
        /* Robotweax SRT: measured local DATA arrival throughput. */
        double data_arrival_rate_bits_per_second;
        /* Effective sender limit (SRTO_MAXBW or congestion control); 0 = none reported. */
        double send_bandwidth_limit_bits_per_second;
        uint64_t mss_bytes;
        uint64_t send_buffer_packets;
        uint64_t receive_buffer_packets;
        uint64_t reorder_tolerance_packets;
        double average_belated_seconds;
    } robotweax_srt_telemetry_ext_sample_v1;

    typedef struct robotweax_srt_telemetry_ext_delta_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        /* Bits use the EXT_FIELD_* values of the source counter. */
        uint64_t available_delta_fields;
        uint64_t counter_reset_fields;

        uint64_t sent_ack_packet_delta;
        uint64_t received_ack_packet_delta;
        uint64_t sent_nak_packet_delta;
        uint64_t received_nak_packet_delta;
        uint64_t sender_active_nano_delta;
        uint64_t receiver_loss_wire_byte_delta;
        uint64_t sender_drop_wire_byte_delta;
        uint64_t receiver_drop_wire_byte_delta;
        uint64_t receiver_undecryptable_payload_byte_delta;
        uint64_t sent_filter_extra_packet_delta;
        uint64_t received_filter_extra_packet_delta;
        uint64_t filter_recovered_packet_delta;
        uint64_t filter_unrecovered_packet_delta;
        uint64_t received_retransmitted_packet_delta;
        uint64_t belated_packet_delta;
    } robotweax_srt_telemetry_ext_delta_v1;

    typedef enum robotweax_srt_telemetry_endpoint_role ROBOTWEAX_SRT_TELEMETRY_ENUM_INT
    {
        ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_UNKNOWN = 0,
        ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER = 1,
        ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER = 2
    } robotweax_srt_telemetry_endpoint_role;

    /* JSONL export takes a stable snapshot of the recorder and sends one
     * complete UTF-8 JSON line per callback invocation. Metadata strings are
     * required printable ASCII and are copied into the snapshot output. */
    typedef struct robotweax_srt_telemetry_jsonl_export_config_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        const char *stream_id;
        const char *flow_id;
        const char *provider_version;
        const char *provider_revision;
        const char *build_profile;
        robotweax_srt_telemetry_endpoint_role role;
        robotweax_srt_telemetry_provider configured_provider;
        uint64_t reserved;
    } robotweax_srt_telemetry_jsonl_export_config_v1;

    /* Return zero to accept a line. A non-zero result stops export and is
     * reported as ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE. The line includes its
     * trailing newline and is not null-terminated for the callback contract. */
    typedef int (*robotweax_srt_telemetry_jsonl_sink)(void *user_data, const char *line,
                                                      size_t line_size);

    typedef struct robotweax_srt_telemetry_delta_engine robotweax_srt_telemetry_delta_engine;
    typedef struct robotweax_srt_telemetry_recorder robotweax_srt_telemetry_recorder;

    ROBOTWEAX_SRT_TELEMETRY_API const char *robotweax_srt_telemetry_version(void);

    ROBOTWEAX_SRT_TELEMETRY_API const char *
    robotweax_srt_telemetry_error_string(robotweax_srt_telemetry_error error);

    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_delta_engine_create(robotweax_srt_telemetry_delta_engine **engine);

    ROBOTWEAX_SRT_TELEMETRY_API void
    robotweax_srt_telemetry_delta_engine_destroy(robotweax_srt_telemetry_delta_engine *engine);

    ROBOTWEAX_SRT_TELEMETRY_API void
    robotweax_srt_telemetry_delta_engine_reset(robotweax_srt_telemetry_delta_engine *engine);

    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_delta_engine_update(robotweax_srt_telemetry_delta_engine *engine,
                                                const robotweax_srt_telemetry_raw_sample_v1 *sample,
                                                robotweax_srt_telemetry_delta_v1 *delta);

    /* Same as delta_engine_update, plus deltas for the extended counters. The
     * interval, quality flags and baselines are shared: ext deltas exist only
     * when the v1 delta has a valid interval and both samples carry the field.
     * Mixing update and update_ext on one engine is allowed; ext deltas then
     * start after the next update_ext call that follows another one. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_delta_engine_update_ext(
        robotweax_srt_telemetry_delta_engine *engine,
        const robotweax_srt_telemetry_raw_sample_v1 *sample,
        const robotweax_srt_telemetry_ext_sample_v1 *ext_sample,
        robotweax_srt_telemetry_delta_v1 *delta, robotweax_srt_telemetry_ext_delta_v1 *ext_delta);

    /* The complete ring is allocated at creation. Push, read, and clear do not
     * grow it. A full ring overwrites its oldest record. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_recorder_create(size_t capacity,
                                            robotweax_srt_telemetry_recorder **recorder);

    ROBOTWEAX_SRT_TELEMETRY_API void
    robotweax_srt_telemetry_recorder_destroy(robotweax_srt_telemetry_recorder *recorder);

    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_recorder_push(robotweax_srt_telemetry_recorder *recorder,
                                          const robotweax_srt_telemetry_raw_sample_v1 *raw,
                                          const robotweax_srt_telemetry_delta_v1 *delta);

    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error robotweax_srt_telemetry_recorder_read(
        const robotweax_srt_telemetry_recorder *recorder, size_t oldest_first_index,
        robotweax_srt_telemetry_record_v1 *record);

    ROBOTWEAX_SRT_TELEMETRY_API size_t
    robotweax_srt_telemetry_recorder_size(const robotweax_srt_telemetry_recorder *recorder);

    ROBOTWEAX_SRT_TELEMETRY_API size_t
    robotweax_srt_telemetry_recorder_capacity(const robotweax_srt_telemetry_recorder *recorder);

    ROBOTWEAX_SRT_TELEMETRY_API uint64_t robotweax_srt_telemetry_recorder_overwritten_count(
        const robotweax_srt_telemetry_recorder *recorder);

    ROBOTWEAX_SRT_TELEMETRY_API void
    robotweax_srt_telemetry_recorder_clear(robotweax_srt_telemetry_recorder *recorder);

    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_recorder_export_jsonl(
        const robotweax_srt_telemetry_recorder *recorder,
        const robotweax_srt_telemetry_jsonl_export_config_v1 *config,
        robotweax_srt_telemetry_jsonl_sink sink, void *user_data);

    /* ------------------------------------------------ recording schema 1alpha2
     * Group context, member state per sample and health transitions
     * (docs/architecture/bounded-recorder.md#schema-1alpha2). A recorder fed
     * with these still exports 1alpha1 through recorder_export_jsonl, which
     * leaves them out. */

/* member_v1.state: SRT_MEMBERSTATUS as reported by srt_group_data. */
#define ROBOTWEAX_SRT_TELEMETRY_RECORDER_MEMBER_PENDING 0u
#define ROBOTWEAX_SRT_TELEMETRY_RECORDER_MEMBER_IDLE 1u
#define ROBOTWEAX_SRT_TELEMETRY_RECORDER_MEMBER_RUNNING 2u
#define ROBOTWEAX_SRT_TELEMETRY_RECORDER_MEMBER_BROKEN 3u
#define ROBOTWEAX_SRT_TELEMETRY_RECORDER_MEMBER_UNKNOWN 4u
/* member_v1.authority: the local Backup sender's selection (sender rows). */
#define ROBOTWEAX_SRT_TELEMETRY_RECORDER_AUTHORITY_UNKNOWN 0u
#define ROBOTWEAX_SRT_TELEMETRY_RECORDER_AUTHORITY_ACTIVE 1u
#define ROBOTWEAX_SRT_TELEMETRY_RECORDER_AUTHORITY_PROBE 2u
#define ROBOTWEAX_SRT_TELEMETRY_RECORDER_AUTHORITY_NONE 3u

    /* A group member's state at one sample. */
    typedef struct robotweax_srt_telemetry_recorder_member_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        uint32_t state;
        /* 1: the host graded the member as a waiting Backup standby. */
        uint32_t standby;
        /* UNKNOWN on receivers or without a sender authority source. */
        uint32_t authority;
        uint32_t reserved;
    } robotweax_srt_telemetry_recorder_member_v1;

    /* Push with the member state of the sample (member may be NULL: none). */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_recorder_push_member_v1(
        robotweax_srt_telemetry_recorder *recorder, const robotweax_srt_telemetry_raw_sample_v1 *raw,
        const robotweax_srt_telemetry_delta_v1 *delta,
        const robotweax_srt_telemetry_recorder_member_v1 *member);

/* jsonl_export_v2_options.group_type */
#define ROBOTWEAX_SRT_TELEMETRY_RECORDING_GROUP_NONE 0u
#define ROBOTWEAX_SRT_TELEMETRY_RECORDING_GROUP_BROADCAST 1u
#define ROBOTWEAX_SRT_TELEMETRY_RECORDING_GROUP_BACKUP 2u
#define ROBOTWEAX_SRT_TELEMETRY_RECORDING_GROUP_UNKNOWN 3u
/* jsonl_export_v2_options.group_row */
#define ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_NONE 0u
#define ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_GROUP 1u
#define ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_MEMBER 2u

    /* What the 1alpha2 header says beyond config_v1. group_type and
     * group_row are NONE together (a plain socket) or set together. */
    typedef struct robotweax_srt_telemetry_jsonl_export_v2_options
    {
        uint32_t struct_size;
        uint32_t abi_version;
        uint32_t group_type;
        uint32_t group_row;
        /* 1: health was graded; the recording carries its transitions. */
        uint32_t health_lines;
        uint32_t reserved;
    } robotweax_srt_telemetry_jsonl_export_v2_options;

    /* Exports schema 1alpha2: the 1alpha1 fields, the header's group context
     * and health counts, member on member rows (unknown state for samples
     * pushed without one), and health lines after the sample each was graded
     * on. When the oldest transitions are gone with the samples they were
     * graded on, the one in force at the first kept sample comes first,
     * marked carried. */
    ROBOTWEAX_SRT_TELEMETRY_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_recorder_export_jsonl_v2(
        const robotweax_srt_telemetry_recorder *recorder,
        const robotweax_srt_telemetry_jsonl_export_config_v1 *config,
        const robotweax_srt_telemetry_jsonl_export_v2_options *options,
        robotweax_srt_telemetry_jsonl_sink sink, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
