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

#define ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION 1u
#define ROBOTWEAX_SRT_TELEMETRY_VERSION "0.1.0-alpha.0"

    typedef enum robotweax_srt_telemetry_error
    {
        ROBOTWEAX_SRT_TELEMETRY_OK = 0,
        ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT = 1,
        ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH = 2,
        ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE = 3,
        ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE = 4,
        ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE = 5
    } robotweax_srt_telemetry_error;

    typedef enum robotweax_srt_telemetry_provider
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

    typedef enum robotweax_srt_telemetry_endpoint_role
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

#ifdef __cplusplus
}
#endif

#endif
