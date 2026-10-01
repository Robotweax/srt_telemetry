#ifndef ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_H
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_H

#include <robotweax/srt_telemetry.h>
#include <robotweax/srt_telemetry_sampler.h>

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

/* Extended fields with the same meaning on both providers. */
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_EXT_COMMON_CANDIDATE_FIELDS                             \
    (ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_CONNECTION_AGE_SECONDS |                                    \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENT_ACK_PACKET_TOTAL |                                     \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_ACK_PACKET_TOTAL |                                 \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENT_NAK_PACKET_TOTAL |                                     \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_NAK_PACKET_TOTAL |                                 \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENDER_ACTIVE_NANO_TOTAL |                                  \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVER_LOSS_WIRE_BYTE_TOTAL |                             \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENDER_DROP_WIRE_BYTE_TOTAL |                               \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVER_DROP_WIRE_BYTE_TOTAL |                             \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVER_UNDECRYPTABLE_PAYLOAD_BYTE_TOTAL |                 \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_RETRANSMITTED_PACKET_SINCE_CLEAR |                 \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_BELATED_PACKET_SINCE_CLEAR |                                \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_REORDER_DISTANCE_PACKETS |                  \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_PACKET_SEND_PERIOD_SECONDS |                                \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SEND_BANDWIDTH_LIMIT_BITS_PER_SECOND |                      \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_MSS_BYTES |                                                 \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SEND_BUFFER_PACKETS |                                       \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVE_BUFFER_PACKETS |                                    \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_REORDER_TOLERANCE_PACKETS |                                 \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_AVERAGE_BELATED_SECONDS)

/* Packet-filter (FEC) counters; qualifying them requires CAP_FEC_STATS_V1. */
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_EXT_FILTER_CANDIDATE_FIELDS                             \
    (ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENT_FILTER_EXTRA_PACKET_TOTAL |                            \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_FILTER_EXTRA_PACKET_TOTAL |                        \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_FILTER_RECOVERED_PACKET_TOTAL |                             \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_FILTER_UNRECOVERED_PACKET_TOTAL)

/* mbpsBandwidth means different things per provider, so it maps to two
 * distinct contract fields. Each provider may qualify only its own. */
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_HAIVISION_V1_5_5_EXT_CANDIDATE_FIELDS                    \
    (ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_EXT_COMMON_CANDIDATE_FIELDS |                              \
     ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_EXT_FILTER_CANDIDATE_FIELDS |                              \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_ESTIMATED_LINK_CAPACITY_BITS_PER_SECOND)
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_ROBOTWEAX_EXT_CANDIDATE_FIELDS                          \
    (ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_EXT_COMMON_CANDIDATE_FIELDS |                              \
     ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_EXT_FILTER_CANDIDATE_FIELDS |                              \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_DATA_ARRIVAL_RATE_BITS_PER_SECOND)

/* Group statistics: srt_bistats on a group ID fills only these fields. Use a
 * separate session whose socket is the group ID. Live bonding qualification is
 * still open; see the field map. */
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_CANDIDATE_FIELDS                                  \
    (ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL |                                      \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL |                                  \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_DROP_PACKET_TOTAL |                                    \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL |                                   \
     ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL)
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_EXT_CANDIDATE_FIELDS                              \
    (ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_CONNECTION_AGE_SECONDS |                                    \
     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVER_DROP_WIRE_BYTE_TOTAL)

    /* v2 adds the qualified extended fields. v1 sessions report no ext fields. */
    typedef struct robotweax_srt_telemetry_public_srt_config_v2
    {
        uint32_t struct_size;
        uint32_t abi_version;
        int32_t socket;
        robotweax_srt_telemetry_provider provider;
        uint64_t capabilities;
        uint64_t qualified_fields;
        uint64_t qualified_ext_fields;
        robotweax_srt_telemetry_public_srt_bistats_fn bistats;
    } robotweax_srt_telemetry_public_srt_config_v2;

    typedef struct robotweax_srt_telemetry_public_srt_ext_result_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        robotweax_srt_telemetry_ext_sample_v1 ext;
        robotweax_srt_telemetry_ext_delta_v1 ext_delta;
    } robotweax_srt_telemetry_public_srt_ext_result_v1;

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

    /* Like session_create, with extended-field qualification. Ext fields must
     * be in the configured provider's candidate mask (so each provider can only
     * qualify its own mbpsBandwidth interpretation); filter fields also require
     * CAP_FEC_STATS_V1. */
    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_public_srt_session_create_v2(
        const robotweax_srt_telemetry_public_srt_config_v2 *config,
        robotweax_srt_telemetry_public_srt_session **session);

    /* One srt_bistats call fills the v1 result and the extended result. Calls
     * may be mixed with session_sample; ext deltas then restart after two
     * consecutive session_sample_ext calls. */
    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_public_srt_session_sample_ext(
        robotweax_srt_telemetry_public_srt_session *session, uint64_t observed_at_unix_nano,
        uint64_t sample_monotonic_nano, robotweax_srt_telemetry_public_srt_result_v1 *result,
        robotweax_srt_telemetry_public_srt_ext_result_v1 *ext_result);

    /* Sampler source for a session: source_data is the session. One
     * srt_bistats call per tick fills the v1 and extended samples. Remove the
     * source from the sampler before destroying the session. */
    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_public_srt_source_sample(
        void *session, uint64_t observed_at_unix_nano, uint64_t sample_monotonic_nano,
        robotweax_srt_telemetry_source_output_v1 *output);

    /* Fills a sampler source config for a session. labels are copied by
     * sampler_add_source, so they only need to live until that call. */
    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API void robotweax_srt_telemetry_public_srt_source_config(
        robotweax_srt_telemetry_public_srt_session *session,
        robotweax_srt_telemetry_endpoint_role role, const robotweax_srt_telemetry_label_v1 *labels,
        uint32_t label_count, robotweax_srt_telemetry_source_config_v1 *config);

#ifdef __cplusplus
}
#endif

#endif
