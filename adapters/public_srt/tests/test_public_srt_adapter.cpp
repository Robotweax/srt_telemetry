#include <robotweax/srt_telemetry_public_srt.h>

#if __has_include(<srt/srt.h>)
#include <srt/srt.h>
#else
#include <srt.h>
#endif

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

} // namespace

int main()
{
    test_non_destructive_mapping_and_delta();
    test_provider_failure_is_explicit();
    test_capability_and_provider_validation();
    if (failures != 0)
    {
        std::cerr << failures << " public SRT adapter checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all public SRT adapter checks passed\n";
    return EXIT_SUCCESS;
}
