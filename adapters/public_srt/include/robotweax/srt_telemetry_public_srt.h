#ifndef ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_H
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_H

#include <robotweax/srt_telemetry.h>

#include <stdint.h>

#if defined(_WIN32)
#if defined(ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_SHARED)
#if defined(ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_BUILDING)
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API __declspec(dllexport)
#else
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API __declspec(dllimport)
#endif
#else
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API __attribute__((visibility("default")))
#else
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    struct CBytePerfMon;

    typedef int (*robotweax_srt_telemetry_public_srt_bistats_fn)(int32_t socket,
                                                                 struct CBytePerfMon *statistics,
                                                                 int clear, int instantaneous);

#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_V1_5_5_CANDIDATE_FIELDS                                 \
    (ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL |                                             \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_PACKET_TOTAL |                                         \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL |                                      \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL |                                  \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_LOSS_PACKET_TOTAL |                                      \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_LOSS_PACKET_TOTAL |                                    \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_PACKET_TOTAL |                                    \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_DROP_PACKET_TOTAL |                                      \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_DROP_PACKET_TOTAL |                                    \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_UNDECRYPTABLE_PACKET_TOTAL |                           \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL |                                          \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_WIRE_BYTE_TOTAL |                                      \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL |                                   \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL |                               \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_WIRE_BYTE_TOTAL |                                 \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS |                                                   \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_FLIGHT_SIZE_PACKETS |                                           \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_FLOW_WINDOW_PACKETS |                                           \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_CONGESTION_WINDOW_PACKETS |                                     \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_AVAILABLE_SEND_BUFFER_BYTES |                                   \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_AVAILABLE_RECEIVE_BUFFER_BYTES |                                \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_BYTES |                                             \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_SECONDS |                                           \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_BYTES |                                          \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_SECONDS |                                        \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_TSBPD_DELAY_SECONDS |                                    \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_TSBPD_DELAY_SECONDS)

    typedef struct robotweax_srt_telemetry_public_srt_config_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        int32_t socket;
        robotweax_srt_telemetry_provider provider;
        uint64_t capabilities;
        uint64_t qualified_fields;
        robotweax_srt_telemetry_public_srt_bistats_fn bistats;
    } robotweax_srt_telemetry_public_srt_config_v1;

    typedef struct robotweax_srt_telemetry_public_srt_result_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        int32_t native_result;
        uint32_t reserved;
        robotweax_srt_telemetry_raw_sample_v1 raw;
        robotweax_srt_telemetry_delta_v1 delta;
    } robotweax_srt_telemetry_public_srt_result_v1;

    typedef struct robotweax_srt_telemetry_public_srt_session
        robotweax_srt_telemetry_public_srt_session;

    /* The host owns the socket and injected srt_bistats function. They must
     * remain valid until session destruction. Calls on one session are
     * externally synchronized; sampling always uses clear=0 and
     * instantaneous=1. */
    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_public_srt_session_create(
        const robotweax_srt_telemetry_public_srt_config_v1 *config,
        robotweax_srt_telemetry_public_srt_session **session);

    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API void robotweax_srt_telemetry_public_srt_session_destroy(
        robotweax_srt_telemetry_public_srt_session *session);

    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API void robotweax_srt_telemetry_public_srt_session_reset(
        robotweax_srt_telemetry_public_srt_session *session);

    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_public_srt_session_sample(
        robotweax_srt_telemetry_public_srt_session *session, uint64_t observed_at_unix_nano,
        uint64_t sample_monotonic_nano, robotweax_srt_telemetry_public_srt_result_v1 *result);

#ifdef __cplusplus
}
#endif

#endif
