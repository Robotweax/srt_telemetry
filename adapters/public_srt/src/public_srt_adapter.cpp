#include "public_srt_internal.hpp"

#if defined(ROBOTWEAX_SRT_TELEMETRY_USE_FLAT_SRT_HEADER)
#include <srt.h>
#else
#include <srt/srt.h>
#endif

#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

/* Layout of the explicitly selected Robotweax extension contract, not of
 * SRT_TRACEBSTATS or an inferred provider feature. Old public headers can
 * compile the reader without importing or linking the optional symbol. */
#ifndef ROBOTWEAX_SRT_RTTDATA_VERSION
struct ROBOTWEAX_SRT_RttDataV1_
{
    std::uint32_t struct_size, abi_version, flags, last_update_source;
    std::uint32_t smoothed_microseconds, variation_microseconds;
    std::uint64_t local_ackack_age_microseconds, peer_estimate_age_microseconds;
    std::uint32_t reserved[2];
};
#endif
static_assert(sizeof(ROBOTWEAX_SRT_RttDataV1_) == 48);
#ifndef ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION
struct ROBOTWEAX_SRT_GroupSendStateV1_
{
    std::uint32_t struct_size, abi_version, state, reserved0;
    std::int32_t active_member, probe_member;
    std::uint64_t group_generation, active_generation, probe_generation;
    std::uint64_t authority_revision, active_age_microseconds, probe_age_microseconds;
    std::uint32_t reserved[2];
};
#endif
static_assert(sizeof(ROBOTWEAX_SRT_GroupSendStateV1_) == 80);

struct robotweax_srt_telemetry_public_srt_session
{
    robotweax_srt_telemetry_public_srt_config_v2 config{};
    robotweax_srt_telemetry_delta_engine *delta_engine = nullptr;
};

namespace
{

constexpr std::uint64_t candidate_fields =
    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_V1_5_5_CANDIDATE_FIELDS;

/* mbpsBandwidth has a provider-specific meaning, so each provider may only
 * qualify its own interpretation. */
std::uint64_t ext_candidate_fields(const robotweax_srt_telemetry_provider provider)
{
    switch (provider)
    {
    case ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT:
        return ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_HAIVISION_V1_5_5_EXT_CANDIDATE_FIELDS;
    case ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT:
        return ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_ROBOTWEAX_EXT_CANDIDATE_FIELDS;
    default:
        return 0;
    }
}

robotweax_srt_telemetry_public_srt_config_v2
widen_config(const robotweax_srt_telemetry_public_srt_config_v1 &config)
{
    robotweax_srt_telemetry_public_srt_config_v2 widened{};
    widened.struct_size = sizeof(widened);
    widened.abi_version = config.abi_version;
    widened.socket = config.socket;
    widened.provider = config.provider;
    widened.capabilities = config.capabilities;
    widened.qualified_fields = config.qualified_fields;
    widened.qualified_ext_fields = 0;
    widened.bistats = config.bistats;
    return widened;
}

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

bool valid_config_v2(const robotweax_srt_telemetry_public_srt_config_v2 *config)
{
    if (config == nullptr ||
        config->struct_size < sizeof(robotweax_srt_telemetry_public_srt_config_v2))
    {
        return false;
    }
    robotweax_srt_telemetry_public_srt_config_v1 common{};
    common.struct_size = sizeof(common);
    common.abi_version = config->abi_version;
    common.socket = config->socket;
    common.provider = config->provider;
    common.capabilities = config->capabilities;
    common.qualified_fields = config->qualified_fields;
    common.bistats = config->bistats;
    const auto ext = config->qualified_ext_fields;
    const auto filter_ok =
        (ext & ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_EXT_FILTER_CANDIDATE_FIELDS) == 0 ||
        (config->capabilities & ROBOTWEAX_SRT_TELEMETRY_CAP_FEC_STATS_V1) != 0;
    const auto common_ok =
        ext == 0 || (config->capabilities & ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1) != 0;
    return valid_config(&common) && (ext & ~ext_candidate_fields(config->provider)) == 0 &&
           filter_ok && common_ok;
}

bool valid_ext_result(const robotweax_srt_telemetry_public_srt_ext_result_v1 *result)
{
    return result != nullptr &&
           result->struct_size >= sizeof(robotweax_srt_telemetry_public_srt_ext_result_v1) &&
           result->abi_version == ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
}

void initialize_ext_result(robotweax_srt_telemetry_public_srt_ext_result_v1 &result)
{
    std::memset(&result, 0, sizeof(result));
    result.struct_size = sizeof(result);
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    result.ext.struct_size = sizeof(result.ext);
    result.ext.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    result.ext_delta.struct_size = sizeof(result.ext_delta);
    result.ext_delta.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
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
                  const robotweax_srt_telemetry_public_srt_config_v2 &config,
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

void map_scaled(const std::uint64_t field, const std::uint64_t qualified_fields, const double source,
                const double scale, double &destination, std::uint64_t &available_fields)
{
    if ((qualified_fields & field) == 0 || !std::isfinite(source) || source < 0.0)
    {
        return;
    }
    destination = source * scale;
    available_fields |= field;
}

void map_microseconds_to_nano(const std::uint64_t field, const std::uint64_t qualified_fields,
                              const std::int64_t source, std::uint64_t &destination,
                              std::uint64_t &available_fields)
{
    constexpr std::int64_t limit = INT64_MAX / 1'000;
    if ((qualified_fields & field) == 0 || source < 0 || source > limit)
    {
        return;
    }
    destination = static_cast<std::uint64_t>(source) * 1'000U;
    available_fields |= field;
}

void populate_ext(const SRT_TRACEBSTATS &statistics,
                  const robotweax_srt_telemetry_public_srt_config_v2 &config,
                  robotweax_srt_telemetry_ext_sample_v1 &ext)
{
    const auto qualified = config.qualified_ext_fields;
    auto &available = ext.available_fields;

    map_scaled(ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_CONNECTION_AGE_SECONDS, qualified,
               static_cast<double>(statistics.msTimeStamp), 1e-3, ext.connection_age_seconds,
               available);

#define ROBOTWEAX_MAP_EXT_SIGNED(field_suffix, source_name, destination_name)                      \
    map_signed_counter(ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_##field_suffix, qualified,                \
                       statistics.source_name, ext.destination_name, available)

    ROBOTWEAX_MAP_EXT_SIGNED(SENT_ACK_PACKET_TOTAL, pktSentACKTotal, sent_ack_packet_total);
    ROBOTWEAX_MAP_EXT_SIGNED(RECEIVED_ACK_PACKET_TOTAL, pktRecvACKTotal, received_ack_packet_total);
    ROBOTWEAX_MAP_EXT_SIGNED(SENT_NAK_PACKET_TOTAL, pktSentNAKTotal, sent_nak_packet_total);
    ROBOTWEAX_MAP_EXT_SIGNED(RECEIVED_NAK_PACKET_TOTAL, pktRecvNAKTotal, received_nak_packet_total);
    ROBOTWEAX_MAP_EXT_SIGNED(SENT_FILTER_EXTRA_PACKET_TOTAL, pktSndFilterExtraTotal,
                             sent_filter_extra_packet_total);
    ROBOTWEAX_MAP_EXT_SIGNED(RECEIVED_FILTER_EXTRA_PACKET_TOTAL, pktRcvFilterExtraTotal,
                             received_filter_extra_packet_total);
    ROBOTWEAX_MAP_EXT_SIGNED(FILTER_RECOVERED_PACKET_TOTAL, pktRcvFilterSupplyTotal,
                             filter_recovered_packet_total);
    ROBOTWEAX_MAP_EXT_SIGNED(FILTER_UNRECOVERED_PACKET_TOTAL, pktRcvFilterLossTotal,
                             filter_unrecovered_packet_total);
    ROBOTWEAX_MAP_EXT_SIGNED(RECEIVED_RETRANSMITTED_PACKET_SINCE_CLEAR, pktRcvRetrans,
                             received_retransmitted_packet_since_clear);
    ROBOTWEAX_MAP_EXT_SIGNED(BELATED_PACKET_SINCE_CLEAR, pktRcvBelated, belated_packet_since_clear);
    ROBOTWEAX_MAP_EXT_SIGNED(REORDER_DISTANCE_PACKETS, pktReorderDistance,
                             reorder_distance_packets);
    ROBOTWEAX_MAP_EXT_SIGNED(MSS_BYTES, byteMSS, mss_bytes);
    ROBOTWEAX_MAP_EXT_SIGNED(SEND_BUFFER_PACKETS, pktSndBuf, send_buffer_packets);
    ROBOTWEAX_MAP_EXT_SIGNED(RECEIVE_BUFFER_PACKETS, pktRcvBuf, receive_buffer_packets);
    ROBOTWEAX_MAP_EXT_SIGNED(REORDER_TOLERANCE_PACKETS, pktReorderTolerance,
                             reorder_tolerance_packets);

#undef ROBOTWEAX_MAP_EXT_SIGNED

#define ROBOTWEAX_MAP_EXT_UNSIGNED(field_suffix, source_name, destination_name)                    \
    map_unsigned_counter(ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_##field_suffix, qualified,              \
                         statistics.source_name, ext.destination_name, available)

    ROBOTWEAX_MAP_EXT_UNSIGNED(RECEIVER_LOSS_WIRE_BYTE_TOTAL, byteRcvLossTotal,
                               receiver_loss_wire_byte_total);
    ROBOTWEAX_MAP_EXT_UNSIGNED(SENDER_DROP_WIRE_BYTE_TOTAL, byteSndDropTotal,
                               sender_drop_wire_byte_total);
    ROBOTWEAX_MAP_EXT_UNSIGNED(RECEIVER_DROP_WIRE_BYTE_TOTAL, byteRcvDropTotal,
                               receiver_drop_wire_byte_total);
    ROBOTWEAX_MAP_EXT_UNSIGNED(RECEIVER_UNDECRYPTABLE_PAYLOAD_BYTE_TOTAL, byteRcvUndecryptTotal,
                               receiver_undecryptable_payload_byte_total);

#undef ROBOTWEAX_MAP_EXT_UNSIGNED

    map_microseconds_to_nano(ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENDER_ACTIVE_NANO_TOTAL, qualified,
                             statistics.usSndDurationTotal, ext.sender_active_nano_total,
                             available);

    constexpr double megabit = 1'000'000.0;
    map_scaled(ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_PACKET_SEND_PERIOD_SECONDS, qualified,
               statistics.usPktSndPeriod, 1e-6, ext.packet_send_period_seconds, available);
    map_scaled(ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_ESTIMATED_LINK_CAPACITY_BITS_PER_SECOND, qualified,
               statistics.mbpsBandwidth, megabit, ext.estimated_link_capacity_bits_per_second,
               available);
    map_scaled(ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_DATA_ARRIVAL_RATE_BITS_PER_SECOND, qualified,
               statistics.mbpsBandwidth, megabit, ext.data_arrival_rate_bits_per_second, available);
    map_scaled(ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SEND_BANDWIDTH_LIMIT_BITS_PER_SECOND, qualified,
               statistics.mbpsMaxBW, megabit, ext.send_bandwidth_limit_bits_per_second, available);
    map_scaled(ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_AVERAGE_BELATED_SECONDS, qualified,
               statistics.pktRcvAvgBelatedTime, 1e-3, ext.average_belated_seconds, available);
}

robotweax_srt_telemetry_error
create_session(const robotweax_srt_telemetry_public_srt_config_v2 &config,
               robotweax_srt_telemetry_public_srt_session **session)
{
    auto *created = new (std::nothrow) robotweax_srt_telemetry_public_srt_session{};
    if (created == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    created->config = config;
    const auto error = robotweax_srt_telemetry_delta_engine_create(&created->delta_engine);
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        delete created;
        return error;
    }
    *session = created;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

} // namespace

// Shared with the group session (public_srt_internal.hpp).
namespace robotweax_srt_telemetry_detail
{

std::uint64_t provider_ext_candidates(const robotweax_srt_telemetry_provider provider)
{
    return ext_candidate_fields(provider);
}

void map_raw(const SRT_TRACEBSTATS &statistics,
             const robotweax_srt_telemetry_public_srt_config_v2 &config,
             const std::uint64_t observed_at_unix_nano, const std::uint64_t sample_monotonic_nano,
             robotweax_srt_telemetry_raw_sample_v1 &raw)
{
    populate_raw(statistics, config, observed_at_unix_nano, sample_monotonic_nano, raw);
}

void map_ext(const SRT_TRACEBSTATS &statistics,
             const robotweax_srt_telemetry_public_srt_config_v2 &config,
             robotweax_srt_telemetry_ext_sample_v1 &ext)
{
    populate_ext(statistics, config, ext);
}

} // namespace robotweax_srt_telemetry_detail

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

    return create_session(widen_config(*config), session);
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_public_srt_session_create_v2(
    const robotweax_srt_telemetry_public_srt_config_v2 *config,
    robotweax_srt_telemetry_public_srt_session **session)
{
    if (session == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *session = nullptr;
    if (!valid_config_v2(config))
    {
        return config != nullptr && config->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION
                   ? ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH
                   : ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    return create_session(*config, session);
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

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_public_srt_session_sample_ext(
    robotweax_srt_telemetry_public_srt_session *session, const std::uint64_t observed_at_unix_nano,
    const std::uint64_t sample_monotonic_nano, robotweax_srt_telemetry_public_srt_result_v1 *result,
    robotweax_srt_telemetry_public_srt_ext_result_v1 *ext_result)
{
    if (session == nullptr || result == nullptr || ext_result == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (!valid_result(result) || !valid_ext_result(ext_result))
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }

    initialize_result(*result);
    initialize_ext_result(*ext_result);
    SRT_TRACEBSTATS statistics{};
    result->native_result = session->config.bistats(session->config.socket, &statistics, 0, 1);
    if (result->native_result != 0)
    {
        return ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE;
    }

    populate_raw(statistics, session->config, observed_at_unix_nano, sample_monotonic_nano,
                 result->raw);
    populate_ext(statistics, session->config, ext_result->ext);
    return robotweax_srt_telemetry_delta_engine_update_ext(session->delta_engine, &result->raw,
                                                           &ext_result->ext, &result->delta,
                                                           &ext_result->ext_delta);
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_public_srt_source_sample(
    void *session, const std::uint64_t observed_at_unix_nano,
    const std::uint64_t sample_monotonic_nano, robotweax_srt_telemetry_source_output_v1 *output)
{
    if (session == nullptr || output == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (output->struct_size < sizeof(*output) ||
        output->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    robotweax_srt_telemetry_public_srt_result_v1 result{};
    result.struct_size = sizeof(result);
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    robotweax_srt_telemetry_public_srt_ext_result_v1 ext{};
    ext.struct_size = sizeof(ext);
    ext.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    const auto error = robotweax_srt_telemetry_public_srt_session_sample_ext(
        static_cast<robotweax_srt_telemetry_public_srt_session *>(session), observed_at_unix_nano,
        sample_monotonic_nano, &result, &ext);
    output->native_result = result.native_result;
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return error;
    }
    output->raw = result.raw;
    output->delta = result.delta;
    output->ext = ext.ext;
    output->ext_delta = ext.ext_delta;
    output->has_ext = 1;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" void robotweax_srt_telemetry_public_srt_source_config(
    robotweax_srt_telemetry_public_srt_session *session,
    const robotweax_srt_telemetry_endpoint_role role, const robotweax_srt_telemetry_label_v1 *labels,
    const std::uint32_t label_count, robotweax_srt_telemetry_source_config_v1 *config)
{
    if (config == nullptr)
    {
        return;
    }
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config->sample = &robotweax_srt_telemetry_public_srt_source_sample;
    config->source_data = session;
    config->role = role;
    config->labels = labels;
    config->label_count = label_count;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_public_srt_read_rtt_metadata_v1(
    const std::int32_t socket, robotweax_srt_telemetry_public_srt_rtt_data_v1_fn getter,
    const std::uint64_t mono, robotweax_srt_telemetry_rtt_metadata_v1 *output)
{
    if (output == nullptr || output->struct_size < sizeof(*output) ||
        output->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION || socket < 0)
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    *output = {};
    output->struct_size = sizeof(*output);
    output->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    output->sampled_monotonic_nano = mono;
    output->local_ackack_age_microseconds = UINT64_MAX;
    output->peer_estimate_age_microseconds = UINT64_MAX;
    if (getter == nullptr)
        return ROBOTWEAX_SRT_TELEMETRY_OK;
    ROBOTWEAX_SRT_RttDataV1_ data{};
    data.struct_size = sizeof(data);
    data.abi_version = 1;
    output->availability = 1;
    output->native_result = getter(socket, &data, sizeof(data));
    if (output->native_result != 0 || data.struct_size != sizeof(data) || data.abi_version != 1 ||
        data.flags > 7 || data.last_update_source > 2 || data.reserved[0] || data.reserved[1] ||
        ((data.flags & 1) == 0 && (data.flags != 0 || data.last_update_source != 0)) ||
        ((data.flags & 1) != 0 && data.last_update_source == 0) ||
        (((data.flags & 2) != 0) != (data.local_ackack_age_microseconds != UINT64_MAX)) ||
        (((data.flags & 4) != 0) != (data.peer_estimate_age_microseconds != UINT64_MAX)) ||
        (data.last_update_source == 1 && (data.flags & 2) == 0) ||
        (data.last_update_source == 2 && (data.flags & 4) == 0))
        return ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE;
    output->availability = 2;
    output->flags = data.flags;
    output->last_update_source = data.last_update_source;
    output->smoothed_microseconds = data.smoothed_microseconds;
    output->variation_microseconds = data.variation_microseconds;
    output->local_ackack_age_microseconds = data.local_ackack_age_microseconds;
    output->peer_estimate_age_microseconds = data.peer_estimate_age_microseconds;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_public_srt_read_group_send_metadata_v1(
    const std::int32_t group, robotweax_srt_telemetry_public_srt_group_send_state_v1_fn getter,
    const std::uint64_t mono, robotweax_srt_telemetry_group_send_metadata_v1 *output)
{
    if (output == nullptr || output->struct_size < sizeof(*output) ||
        output->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION || group < 0)
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    *output = {};
    output->struct_size = sizeof(*output);
    output->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    output->sampled_monotonic_nano = mono;
    output->active_member = output->probe_member = -1;
    output->active_age_microseconds = output->probe_age_microseconds = UINT64_MAX;
    if (getter == nullptr)
        return ROBOTWEAX_SRT_TELEMETRY_OK;
    ROBOTWEAX_SRT_GroupSendStateV1_ data{};
    data.struct_size = sizeof(data);
    data.abi_version = 1;
    output->availability = 1;
    output->native_result = getter(group, &data, sizeof(data));
    if (output->native_result != 0 || data.struct_size != sizeof(data) || data.abi_version != 1 ||
        data.reserved0 || data.reserved[0] || data.reserved[1] || data.state > 1 ||
        data.group_generation == 0 || data.active_member < -1 || data.probe_member < -1 ||
        (data.state == 1) != (data.active_member >= 0) ||
        (data.active_member >= 0) != (data.active_generation != 0) ||
        (data.probe_member >= 0) != (data.probe_generation != 0) ||
        (data.active_member >= 0) != (data.active_age_microseconds != UINT64_MAX) ||
        (data.probe_member >= 0) != (data.probe_age_microseconds != UINT64_MAX))
        return ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE;
    output->availability = 2;
    output->state = data.state;
    output->group_generation = data.group_generation;
    output->active_member = data.active_member;
    output->active_generation = data.active_generation;
    output->probe_member = data.probe_member;
    output->probe_generation = data.probe_generation;
    output->authority_revision = data.authority_revision;
    output->active_age_microseconds = data.active_age_microseconds;
    output->probe_age_microseconds = data.probe_age_microseconds;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}
