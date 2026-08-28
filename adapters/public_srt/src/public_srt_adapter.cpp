#include <robotweax/srt_telemetry_public_srt.h>

#if defined(ROBOTWEAX_SRT_TELEMETRY_USE_FLAT_SRT_HEADER)
#include <srt.h>
#else
#include <srt/srt.h>
#endif

#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

struct robotweax_srt_telemetry_public_srt_session
{
    robotweax_srt_telemetry_public_srt_config_v1 config{};
    robotweax_srt_telemetry_delta_engine *delta_engine = nullptr;
};

namespace
{

constexpr std::uint64_t candidate_fields =
    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_V1_5_5_CANDIDATE_FIELDS;

bool valid_config(const robotweax_srt_telemetry_public_srt_config_v1 *config)
{
    const auto known_provider =
        config != nullptr && (config->provider == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT ||
                              config->provider == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT);
    const auto common_capability_matches_fields =
        config != nullptr &&
        (config->qualified_fields == 0 ||
         (config->capabilities & ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1) != 0);
    return config != nullptr &&
           config->struct_size >= sizeof(robotweax_srt_telemetry_public_srt_config_v1) &&
           config->abi_version == ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION && config->socket >= 0 &&
           known_provider && common_capability_matches_fields && config->bistats != nullptr &&
           (config->qualified_fields & ~candidate_fields) == 0;
}

bool valid_result(const robotweax_srt_telemetry_public_srt_result_v1 *result)
{
    return result != nullptr &&
           result->struct_size >= sizeof(robotweax_srt_telemetry_public_srt_result_v1) &&
           result->abi_version == ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
}

void initialize_result(robotweax_srt_telemetry_public_srt_result_v1 &result)
{
    std::memset(&result, 0, sizeof(result));
    result.struct_size = sizeof(result);
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    result.raw.struct_size = sizeof(result.raw);
    result.raw.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    result.delta.struct_size = sizeof(result.delta);
    result.delta.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
}

void map_signed_counter(const std::uint64_t field, const std::uint64_t qualified_fields,
                        const std::int64_t source, std::uint64_t &destination,
                        std::uint64_t &available_fields)
{
    if ((qualified_fields & field) == 0 || source < 0)
    {
        return;
    }
    destination = static_cast<std::uint64_t>(source);
    available_fields |= field;
}

void map_unsigned_counter(const std::uint64_t field, const std::uint64_t qualified_fields,
                          const std::uint64_t source, std::uint64_t &destination,
                          std::uint64_t &available_fields)
{
    if ((qualified_fields & field) == 0)
    {
        return;
    }
    destination = source;
    available_fields |= field;
}

void map_signed_gauge(const std::uint64_t field, const std::uint64_t qualified_fields,
                      const std::int64_t source, std::uint64_t &destination,
                      std::uint64_t &available_fields)
{
    map_signed_counter(field, qualified_fields, source, destination, available_fields);
}

void map_milliseconds(const std::uint64_t field, const std::uint64_t qualified_fields,
                      const double source, double &destination, std::uint64_t &available_fields)
{
    if ((qualified_fields & field) == 0 || !std::isfinite(source) || source < 0.0)
    {
        return;
    }
    destination = source / 1'000.0;
    available_fields |= field;
}

void populate_raw(const SRT_TRACEBSTATS &statistics,
                  const robotweax_srt_telemetry_public_srt_config_v1 &config,
                  const std::uint64_t observed_at_unix_nano,
                  const std::uint64_t sample_monotonic_nano,
                  robotweax_srt_telemetry_raw_sample_v1 &raw)
{
    raw.provider = config.provider;
    raw.capabilities = config.capabilities;
    raw.observed_at_unix_nano = observed_at_unix_nano;
    raw.sample_monotonic_nano = sample_monotonic_nano;

#define ROBOTWEAX_MAP_SIGNED(field_name, source_name, destination_name)                            \
    map_signed_counter(field_name, config.qualified_fields, statistics.source_name,                \
                       raw.destination_name, raw.available_fields)

    ROBOTWEAX_MAP_SIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL, pktSentTotal,
                         sent_packet_total);
    ROBOTWEAX_MAP_SIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_PACKET_TOTAL, pktRecvTotal,
                         received_packet_total);
    ROBOTWEAX_MAP_SIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL, pktSentUniqueTotal,
                         unique_sent_packet_total);
    ROBOTWEAX_MAP_SIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL,
                         pktRecvUniqueTotal, unique_received_packet_total);
    ROBOTWEAX_MAP_SIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_LOSS_PACKET_TOTAL, pktSndLossTotal,
                         sender_loss_packet_total);
    ROBOTWEAX_MAP_SIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_LOSS_PACKET_TOTAL, pktRcvLossTotal,
                         receiver_loss_packet_total);
    ROBOTWEAX_MAP_SIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_PACKET_TOTAL, pktRetransTotal,
                         retransmitted_packet_total);
    ROBOTWEAX_MAP_SIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_DROP_PACKET_TOTAL, pktSndDropTotal,
                         sender_drop_packet_total);
    ROBOTWEAX_MAP_SIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_DROP_PACKET_TOTAL, pktRcvDropTotal,
                         receiver_drop_packet_total);
    ROBOTWEAX_MAP_SIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_UNDECRYPTABLE_PACKET_TOTAL,
                         pktRcvUndecryptTotal, receiver_undecryptable_packet_total);

#undef ROBOTWEAX_MAP_SIGNED

#define ROBOTWEAX_MAP_UNSIGNED(field_name, source_name, destination_name)                          \
    map_unsigned_counter(field_name, config.qualified_fields, statistics.source_name,              \
                         raw.destination_name, raw.available_fields)

    ROBOTWEAX_MAP_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL, byteSentTotal,
                           sent_wire_byte_total);
    ROBOTWEAX_MAP_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_WIRE_BYTE_TOTAL, byteRecvTotal,
                           received_wire_byte_total);
    ROBOTWEAX_MAP_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL,
                           byteSentUniqueTotal, unique_sent_wire_byte_total);
    ROBOTWEAX_MAP_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL,
                           byteRecvUniqueTotal, unique_received_wire_byte_total);
    ROBOTWEAX_MAP_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_WIRE_BYTE_TOTAL,
                           byteRetransTotal, retransmitted_wire_byte_total);

#undef ROBOTWEAX_MAP_UNSIGNED

    map_milliseconds(ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS, config.qualified_fields,
                     statistics.msRTT, raw.rtt_seconds, raw.available_fields);

#define ROBOTWEAX_MAP_GAUGE(field_name, source_name, destination_name)                             \
    map_signed_gauge(field_name, config.qualified_fields, statistics.source_name,                  \
                     raw.destination_name, raw.available_fields)

    ROBOTWEAX_MAP_GAUGE(ROBOTWEAX_SRT_TELEMETRY_FIELD_FLIGHT_SIZE_PACKETS, pktFlightSize,
                        flight_size_packets);
    ROBOTWEAX_MAP_GAUGE(ROBOTWEAX_SRT_TELEMETRY_FIELD_FLOW_WINDOW_PACKETS, pktFlowWindow,
                        flow_window_packets);
    ROBOTWEAX_MAP_GAUGE(ROBOTWEAX_SRT_TELEMETRY_FIELD_CONGESTION_WINDOW_PACKETS,
                        pktCongestionWindow, congestion_window_packets);
    ROBOTWEAX_MAP_GAUGE(ROBOTWEAX_SRT_TELEMETRY_FIELD_AVAILABLE_SEND_BUFFER_BYTES, byteAvailSndBuf,
                        available_send_buffer_bytes);
    ROBOTWEAX_MAP_GAUGE(ROBOTWEAX_SRT_TELEMETRY_FIELD_AVAILABLE_RECEIVE_BUFFER_BYTES,
                        byteAvailRcvBuf, available_receive_buffer_bytes);
    ROBOTWEAX_MAP_GAUGE(ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_BYTES, byteSndBuf,
                        send_buffer_bytes);
    ROBOTWEAX_MAP_GAUGE(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_BYTES, byteRcvBuf,
                        receive_buffer_bytes);

#undef ROBOTWEAX_MAP_GAUGE

    map_milliseconds(ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_SECONDS, config.qualified_fields,
                     statistics.msSndBuf, raw.send_buffer_seconds, raw.available_fields);
    map_milliseconds(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_SECONDS, config.qualified_fields,
                     statistics.msRcvBuf, raw.receive_buffer_seconds, raw.available_fields);
    map_milliseconds(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_TSBPD_DELAY_SECONDS,
                     config.qualified_fields, statistics.msSndTsbPdDelay,
                     raw.sender_tsbpd_delay_seconds, raw.available_fields);
    map_milliseconds(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_TSBPD_DELAY_SECONDS,
                     config.qualified_fields, statistics.msRcvTsbPdDelay,
                     raw.receiver_tsbpd_delay_seconds, raw.available_fields);
}

} // namespace

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_public_srt_session_create(
    const robotweax_srt_telemetry_public_srt_config_v1 *config,
    robotweax_srt_telemetry_public_srt_session **session)
{
    if (session == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *session = nullptr;
    if (!valid_config(config))
    {
        return config != nullptr && config->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION
                   ? ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH
                   : ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }

    auto *created = new (std::nothrow) robotweax_srt_telemetry_public_srt_session{};
    if (created == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    created->config = *config;
    const auto error = robotweax_srt_telemetry_delta_engine_create(&created->delta_engine);
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        delete created;
        return error;
    }
    *session = created;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" void robotweax_srt_telemetry_public_srt_session_destroy(
    robotweax_srt_telemetry_public_srt_session *session)
{
    if (session != nullptr)
    {
        robotweax_srt_telemetry_delta_engine_destroy(session->delta_engine);
    }
    delete session;
}

extern "C" void robotweax_srt_telemetry_public_srt_session_reset(
    robotweax_srt_telemetry_public_srt_session *session)
{
    if (session != nullptr)
    {
        robotweax_srt_telemetry_delta_engine_reset(session->delta_engine);
    }
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_public_srt_session_sample(
    robotweax_srt_telemetry_public_srt_session *session, const std::uint64_t observed_at_unix_nano,
    const std::uint64_t sample_monotonic_nano, robotweax_srt_telemetry_public_srt_result_v1 *result)
{
    if (session == nullptr || result == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (!valid_result(result))
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }

    initialize_result(*result);
    SRT_TRACEBSTATS statistics{};
    result->native_result = session->config.bistats(session->config.socket, &statistics, 0, 1);
    if (result->native_result != 0)
    {
        return ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE;
    }

    populate_raw(statistics, session->config, observed_at_unix_nano, sample_monotonic_nano,
                 result->raw);
    return robotweax_srt_telemetry_delta_engine_update(session->delta_engine, &result->raw,
                                                       &result->delta);
}
