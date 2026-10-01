#include <robotweax/srt_telemetry_public_srt.h>

#if __has_include(<srt/srt.h>)
#include <srt/srt.h>
#else
#include <srt.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <type_traits>

namespace
{

static_assert(
    std::is_same_v<decltype(&srt_bistats), robotweax_srt_telemetry_public_srt_bistats_fn>);

int failures = 0;
int sample_calls = 0;
int observed_clear = -1;
int observed_instantaneous = -1;

void check(const bool condition, const std::string_view message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

int fake_bistats(const SRTSOCKET, SRT_TRACEBSTATS *statistics, const int clear,
                 const int instantaneous)
{
    observed_clear = clear;
    observed_instantaneous = instantaneous;
    const auto increment = sample_calls++;

    *statistics = {};
    statistics->pktSentTotal = 100 + increment * 10;
    statistics->pktRecvTotal = 80 + increment * 8;
    statistics->pktSndLossTotal = -1;
    statistics->pktRcvLossTotal = increment;
    statistics->pktRetransTotal = 4 + increment;
    statistics->pktSndDropTotal = increment;
    statistics->pktRcvDropTotal = increment * 2;
    statistics->pktRcvUndecryptTotal = 0;
    statistics->byteSentTotal = 10'000U + static_cast<std::uint64_t>(increment) * 1'000U;
    statistics->byteRecvTotal = 8'000U + static_cast<std::uint64_t>(increment) * 800U;
    statistics->pktSentUniqueTotal = 96 + increment * 9;
    statistics->pktRecvUniqueTotal = 79 + increment * 7;
    statistics->byteSentUniqueTotal = 9'000U + static_cast<std::uint64_t>(increment) * 900U;
    statistics->byteRecvUniqueTotal = 7'500U + static_cast<std::uint64_t>(increment) * 700U;
    statistics->byteRetransTotal = 1'000U + static_cast<std::uint64_t>(increment) * 100U;
    statistics->msRTT = 12.5;
    statistics->pktFlightSize = 7;
    statistics->pktFlowWindow = 8'192;
    statistics->pktCongestionWindow = 1'024;
    statistics->byteAvailSndBuf = 64'000;
    statistics->byteAvailRcvBuf = 63'000;
    statistics->byteSndBuf = 2'000;
    statistics->msSndBuf = 20;
    statistics->byteRcvBuf = 3'000;
    statistics->msRcvBuf = 30;
    statistics->msSndTsbPdDelay = 120;
    statistics->msRcvTsbPdDelay = 130;
    return 0;
}

int failing_bistats(const SRTSOCKET, SRT_TRACEBSTATS *, const int, const int)
{
    return SRT_ERROR;
}

robotweax_srt_telemetry_public_srt_config_v1
config_for(const robotweax_srt_telemetry_public_srt_bistats_fn bistats)
{
    robotweax_srt_telemetry_public_srt_config_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config.socket = 42;
    config.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    config.capabilities = ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1;
    config.qualified_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_V1_5_5_CANDIDATE_FIELDS;
    config.bistats = bistats;
    return config;
}

robotweax_srt_telemetry_public_srt_result_v1 empty_result()
{
    robotweax_srt_telemetry_public_srt_result_v1 result{};
    result.struct_size = sizeof(result);
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    return result;
}

void test_non_destructive_mapping_and_delta()
{
    sample_calls = 0;
    auto config = config_for(&fake_bistats);
    robotweax_srt_telemetry_public_srt_session *session = nullptr;
    check(robotweax_srt_telemetry_public_srt_session_create(&config, &session) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "session creation");

    auto first = empty_result();
    check(robotweax_srt_telemetry_public_srt_session_sample(session, 1'800'000'000'000'000'000ULL,
                                                            1'000'000'000ULL,
                                                            &first) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "first public SRT sample");
    check(observed_clear == 0, "adapter never clears host interval counters");
    check(observed_instantaneous == 1, "adapter requests instantaneous queue gauges");
    check(first.raw.sent_packet_total == 100, "physical packet counter mapping");
    check(first.raw.unique_sent_packet_total == 96, "unique packet counter mapping");
    check(std::abs(first.raw.rtt_seconds - 0.0125) < 0.000001, "RTT unit conversion");
    check(std::abs(first.raw.receive_buffer_seconds - 0.030) < 0.000001,
          "buffer duration conversion");
    check((first.raw.available_fields & ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_LOSS_PACKET_TOTAL) ==
              0,
          "negative provider value is unavailable rather than a large unsigned value");

    auto second = empty_result();
    check(robotweax_srt_telemetry_public_srt_session_sample(session, 1'800'000'001'000'000'000ULL,
                                                            2'000'000'000ULL,
                                                            &second) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "second public SRT sample");
    check(second.delta.sent_packet_delta == 10, "lifetime packet delta");
    check(std::abs(second.delta.sent_wire_bits_per_second - 8'000.0) < 0.001,
          "physical wire rate from lifetime byte delta");
    check(std::abs(second.delta.unique_sent_wire_bits_per_second - 7'200.0) < 0.001,
          "unique wire rate remains separate");

    robotweax_srt_telemetry_public_srt_session_destroy(session);
}

void test_provider_failure_is_explicit()
{
    auto config = config_for(&failing_bistats);
    robotweax_srt_telemetry_public_srt_session *session = nullptr;
    check(robotweax_srt_telemetry_public_srt_session_create(&config, &session) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "failure-session creation");

    auto result = empty_result();
    check(robotweax_srt_telemetry_public_srt_session_sample(session, 1, 1, &result) ==
              ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE,
          "provider failure is distinct from unavailable telemetry");
    check(result.native_result == SRT_ERROR, "native provider result is retained");
    check(result.raw.available_fields == 0, "failed sample emits no observed values");

    robotweax_srt_telemetry_public_srt_session_destroy(session);
}

void test_capability_and_provider_validation()
{
    robotweax_srt_telemetry_public_srt_session *session = nullptr;
    auto config = config_for(&fake_bistats);
    config.capabilities = 0;
    check(robotweax_srt_telemetry_public_srt_session_create(&config, &session) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "qualified common fields require the common-stats capability");

    config = config_for(&fake_bistats);
    config.provider = static_cast<robotweax_srt_telemetry_provider>(99);
    check(robotweax_srt_telemetry_public_srt_session_create(&config, &session) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "only qualified Robotweax and Haivision providers are accepted");
}

int full_bistats(const SRTSOCKET socket, SRT_TRACEBSTATS *statistics, const int clear,
                 const int instantaneous)
{
    const auto step = sample_calls;
    const auto result = fake_bistats(socket, statistics, clear, instantaneous);
    statistics->msTimeStamp = 2'500 + step * 1'000;
    statistics->pktSentACKTotal = 40 + step * 4;
    statistics->pktRecvACKTotal = 41 + step * 5;
    statistics->pktSentNAKTotal = 2 + step;
    statistics->pktRecvNAKTotal = 3 + step;
    statistics->usSndDurationTotal = 900'000 + step * 1'000'000;
    statistics->byteRcvLossTotal = 1'316U * static_cast<std::uint64_t>(step);
    statistics->byteSndDropTotal = 1'360U * static_cast<std::uint64_t>(step);
    statistics->byteRcvDropTotal = 2'720U * static_cast<std::uint64_t>(step);
    statistics->byteRcvUndecryptTotal = 0;
    statistics->pktSndFilterExtraTotal = 5 + step;
    statistics->pktRcvFilterExtraTotal = 4 + step;
    statistics->pktRcvFilterSupplyTotal = 1 + step;
    statistics->pktRcvFilterLossTotal = 0;
    statistics->pktRcvRetrans = 6 + step * 2;
    statistics->pktRcvBelated = step;
    statistics->pktReorderDistance = 3;
    statistics->usPktSndPeriod = 10.5;
    statistics->mbpsBandwidth = 940.5;
    statistics->mbpsMaxBW = 12.5;
    statistics->byteMSS = 1'500;
    statistics->pktSndBuf = 11;
    statistics->pktRcvBuf = 12;
    statistics->pktReorderTolerance = 4;
    statistics->pktRcvAvgBelatedTime = 7.5;
    statistics->mbpsSendRate = 99.0;
    return result;
}

robotweax_srt_telemetry_public_srt_config_v2
config_v2_for(const robotweax_srt_telemetry_public_srt_bistats_fn bistats,
              const robotweax_srt_telemetry_provider provider)
{
    robotweax_srt_telemetry_public_srt_config_v2 config{};
    config.struct_size = sizeof(config);
    config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config.socket = 42;
    config.provider = provider;
    config.capabilities =
        ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1 | ROBOTWEAX_SRT_TELEMETRY_CAP_FEC_STATS_V1;
    config.qualified_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_V1_5_5_CANDIDATE_FIELDS;
    config.qualified_ext_fields =
        provider == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT
            ? ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_HAIVISION_V1_5_5_EXT_CANDIDATE_FIELDS
            : ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_ROBOTWEAX_EXT_CANDIDATE_FIELDS;
    config.bistats = bistats;
    return config;
}

robotweax_srt_telemetry_public_srt_ext_result_v1 empty_ext_result()
{
    robotweax_srt_telemetry_public_srt_ext_result_v1 result{};
    result.struct_size = sizeof(result);
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    return result;
}

bool near(const double actual, const double expected)
{
    return std::abs(actual - expected) <= 1e-9 * std::max(1.0, std::abs(expected));
}

void test_extended_mapping_and_delta()
{
    sample_calls = 0;
    auto config = config_v2_for(&full_bistats, ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT);
    robotweax_srt_telemetry_public_srt_session *session = nullptr;
    check(robotweax_srt_telemetry_public_srt_session_create_v2(&config, &session) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "v2 session creation");

    auto first = empty_result();
    auto first_ext = empty_ext_result();
    check(robotweax_srt_telemetry_public_srt_session_sample_ext(session, 1, 1'000'000'000ULL, &first,
                                                                &first_ext) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "first extended sample");
    check(observed_clear == 0 && observed_instantaneous == 1,
          "extended sampling keeps clear=0 and instantaneous buffers");
    const auto &ext = first_ext.ext;
    check(ext.available_fields == config.qualified_ext_fields,
          "every qualified extended field is available");
    check(near(ext.connection_age_seconds, 2.5), "connection age in seconds");
    check(ext.sent_ack_packet_total == 40 && ext.received_ack_packet_total == 41,
          "ACK counters keep their side");
    check(ext.sent_nak_packet_total == 2 && ext.received_nak_packet_total == 3,
          "NAK counters keep their side");
    check(ext.sender_active_nano_total == 900'000'000ULL, "sender-active microseconds to ns");
    check(ext.sent_filter_extra_packet_total == 5 && ext.filter_recovered_packet_total == 1,
          "filter counters");
    check(ext.received_retransmitted_packet_since_clear == 6 &&
              ext.reorder_distance_packets == 3,
          "interval-accumulated receiver fields");
    check(near(ext.packet_send_period_seconds, 10.5e-6), "send period in seconds");
    check(near(ext.estimated_link_capacity_bits_per_second, 940.5e6),
          "Haivision mbpsBandwidth maps to link capacity");
    check((ext.available_fields &
           ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_DATA_ARRIVAL_RATE_BITS_PER_SECOND) == 0 &&
              ext.data_arrival_rate_bits_per_second == 0.0,
          "the other mbpsBandwidth interpretation stays unavailable");
    check(near(ext.send_bandwidth_limit_bits_per_second, 12.5e6), "MaxBW in bit/s");
    check(ext.mss_bytes == 1'500 && ext.send_buffer_packets == 11 &&
              ext.receive_buffer_packets == 12 && ext.reorder_tolerance_packets == 4,
          "packet gauges");
    check(near(ext.average_belated_seconds, 0.0075), "average belated time in seconds");
    check(first.raw.available_fields ==
              (config.qualified_fields & ~ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_LOSS_PACKET_TOTAL),
          "the same call fills every qualified v1 field (the fake's negative loss stays unavailable)");

    auto second = empty_result();
    auto second_ext = empty_ext_result();
    check(robotweax_srt_telemetry_public_srt_session_sample_ext(session, 2, 2'000'000'000ULL,
                                                                &second, &second_ext) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "second extended sample");
    const auto &d = second_ext.ext_delta;
    check(d.available_delta_fields == (config.qualified_ext_fields &
                                       ROBOTWEAX_SRT_TELEMETRY_EXT_COUNTER_FIELDS),
          "every qualified ext counter has a delta");
    check(d.sent_ack_packet_delta == 4 && d.received_ack_packet_delta == 5, "ACK deltas");
    check(d.sender_active_nano_delta == 1'000'000'000ULL, "sender-active delta");
    check(d.received_retransmitted_packet_delta == 2 && d.belated_packet_delta == 1,
          "since-clear counter deltas");
    check(second.delta.sent_packet_delta == 10, "v1 deltas are unchanged by the ext path");
    robotweax_srt_telemetry_public_srt_session_destroy(session);

    sample_calls = 0;
    config = config_v2_for(&full_bistats, ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT);
    check(robotweax_srt_telemetry_public_srt_session_create_v2(&config, &session) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "Robotweax v2 session creation");
    auto robotweax = empty_result();
    auto robotweax_ext = empty_ext_result();
    static_cast<void>(robotweax_srt_telemetry_public_srt_session_sample_ext(
        session, 1, 1'000'000'000ULL, &robotweax, &robotweax_ext));
    check(near(robotweax_ext.ext.data_arrival_rate_bits_per_second, 940.5e6) &&
              (robotweax_ext.ext.available_fields &
               ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_ESTIMATED_LINK_CAPACITY_BITS_PER_SECOND) == 0,
          "Robotweax mbpsBandwidth maps to measured arrival rate only");
    robotweax_srt_telemetry_public_srt_session_destroy(session);
}

void test_v1_session_reports_no_ext_fields()
{
    sample_calls = 0;
    auto config = config_for(&full_bistats);
    robotweax_srt_telemetry_public_srt_session *session = nullptr;
    check(robotweax_srt_telemetry_public_srt_session_create(&config, &session) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "v1 session creation");
    auto result = empty_result();
    auto ext = empty_ext_result();
    check(robotweax_srt_telemetry_public_srt_session_sample_ext(session, 1, 1, &result, &ext) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "ext sampling on a v1 session");
    check(ext.ext.available_fields == 0, "a v1 session qualifies no extended fields");
    auto short_ext = empty_ext_result();
    short_ext.struct_size = 8;
    check(robotweax_srt_telemetry_public_srt_session_sample_ext(session, 2, 2, &result,
                                                                &short_ext) ==
              ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH,
          "truncated ext result is an ABI mismatch");
    robotweax_srt_telemetry_public_srt_session_destroy(session);
}

void test_v2_validation()
{
    robotweax_srt_telemetry_public_srt_session *session = nullptr;
    auto config = config_v2_for(&full_bistats, ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT);
    config.qualified_ext_fields |=
        ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_DATA_ARRIVAL_RATE_BITS_PER_SECOND;
    check(robotweax_srt_telemetry_public_srt_session_create_v2(&config, &session) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "both mbpsBandwidth interpretations cannot be qualified together");

    config = config_v2_for(&full_bistats, ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT);
    config.qualified_ext_fields =
        ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_DATA_ARRIVAL_RATE_BITS_PER_SECOND;
    check(robotweax_srt_telemetry_public_srt_session_create_v2(&config, &session) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "Haivision cannot qualify Robotweax's arrival-rate meaning of mbpsBandwidth");

    config = config_v2_for(&full_bistats, ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT);
    config.qualified_ext_fields =
        ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_ESTIMATED_LINK_CAPACITY_BITS_PER_SECOND;
    check(robotweax_srt_telemetry_public_srt_session_create_v2(&config, &session) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "Robotweax cannot qualify Haivision's link-capacity meaning of mbpsBandwidth");

    config = config_v2_for(&full_bistats, ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT);
    config.capabilities = ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1;
    check(robotweax_srt_telemetry_public_srt_session_create_v2(&config, &session) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "filter fields require the FEC capability");

    config = config_v2_for(&full_bistats, ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT);
    config.qualified_ext_fields |= UINT64_C(1) << 40;
    check(robotweax_srt_telemetry_public_srt_session_create_v2(&config, &session) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "unknown extended fields are rejected");

    config = config_v2_for(&full_bistats, ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT);
    config.struct_size = sizeof(robotweax_srt_telemetry_public_srt_config_v1);
    check(robotweax_srt_telemetry_public_srt_session_create_v2(&config, &session) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "a v1-sized config is not accepted as v2");
    check(session == nullptr, "failed creation leaves no session");
}

void test_group_candidates_are_a_subset()
{
    static_assert((ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_CANDIDATE_FIELDS &
                   ~ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_V1_5_5_CANDIDATE_FIELDS) == 0);
    static_assert((ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_EXT_CANDIDATE_FIELDS &
                   ~ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_EXT_COMMON_CANDIDATE_FIELDS) == 0);
    sample_calls = 0;
    auto config = config_v2_for(&full_bistats, ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT);
    config.socket = 0x40000001; /* SRT group IDs carry the group bit */
    config.qualified_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_CANDIDATE_FIELDS;
    config.qualified_ext_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_EXT_CANDIDATE_FIELDS;
    robotweax_srt_telemetry_public_srt_session *session = nullptr;
    check(robotweax_srt_telemetry_public_srt_session_create_v2(&config, &session) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "group-ID session creation");
    auto result = empty_result();
    auto ext = empty_ext_result();
    static_cast<void>(
        robotweax_srt_telemetry_public_srt_session_sample_ext(session, 1, 1, &result, &ext));
    check(result.raw.available_fields == config.qualified_fields &&
              ext.ext.available_fields == config.qualified_ext_fields,
          "a group session exposes only the group field set");
    robotweax_srt_telemetry_public_srt_session_destroy(session);
}

} // namespace

int main()
{
    test_non_destructive_mapping_and_delta();
    test_provider_failure_is_explicit();
    test_capability_and_provider_validation();
    test_extended_mapping_and_delta();
    test_v1_session_reports_no_ext_fields();
    test_v2_validation();
    test_group_candidates_are_a_subset();
    if (failures != 0)
    {
        std::cerr << failures << " public SRT adapter checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all public SRT adapter checks passed\n";
    return EXIT_SUCCESS;
}
