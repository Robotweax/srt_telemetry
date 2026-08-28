#include <robotweax/srt_telemetry_public_srt.h>

#if defined(ROBOTWEAX_SRT_TELEMETRY_RUNTIME_USE_FLAT_SRT_HEADER)
#include <srt.h>
#else
#include <srt/srt.h>
#endif

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

constexpr auto runtime_provider =
#if defined(ROBOTWEAX_SRT_TELEMETRY_RUNTIME_PROVIDER_ROBOTWEAX)
    ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
constexpr std::string_view runtime_provider_name = "robotweax-srt";
#else
    ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
constexpr std::string_view runtime_provider_name = "haivision-srt";
#endif

constexpr std::uint64_t runtime_fields =
    ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_PACKET_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_WIRE_BYTE_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS;

std::atomic<std::uint64_t> benchmark_sequence{0};

class Runtime final
{
  public:
    Runtime() : active_(srt_startup() == 0) {}

    ~Runtime()
    {
        if (active_)
        {
            static_cast<void>(srt_cleanup());
        }
    }

    [[nodiscard]] bool active() const noexcept
    {
        return active_;
    }

  private:
    bool active_;
};

class Socket final
{
  public:
    Socket() = default;
    explicit Socket(const SRTSOCKET socket) : socket_(socket) {}

    ~Socket()
    {
        static_cast<void>(close());
    }

    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;

    [[nodiscard]] SRTSOCKET get() const noexcept
    {
        return socket_;
    }

    int close() noexcept
    {
        if (socket_ == SRT_INVALID_SOCK)
        {
            return 0;
        }
        const auto socket = socket_;
        socket_ = SRT_INVALID_SOCK;
        return srt_close(socket);
    }

  private:
    SRTSOCKET socket_ = SRT_INVALID_SOCK;
};

struct Session final
{
    ~Session()
    {
        robotweax_srt_telemetry_public_srt_session_destroy(value);
    }

    robotweax_srt_telemetry_public_srt_session *value = nullptr;
};

bool srt_ok(const int result, const std::string_view operation)
{
    if (result != SRT_ERROR)
    {
        return true;
    }
    std::cerr << "FAIL: " << operation << ": " << srt_getlasterror_str() << '\n';
    return false;
}

bool bind_ephemeral_udp(const SRTSOCKET listener, sockaddr_in &address)
{
    const auto native_socket = ::socket(AF_INET, SOCK_DGRAM, 0);
#if defined(_WIN32)
    if (native_socket == INVALID_SOCKET)
#else
    if (native_socket < 0)
#endif
    {
        std::cerr << "FAIL: native UDP socket creation\n";
        return false;
    }

    const auto close_native = [&]()
    {
#if defined(_WIN32)
        static_cast<void>(closesocket(native_socket));
#else
        static_cast<void>(close(native_socket));
#endif
    };

    address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(native_socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
    {
        std::cerr << "FAIL: native UDP bind\n";
        close_native();
        return false;
    }

#if defined(_WIN32)
    int address_size = sizeof(address);
#else
    socklen_t address_size = sizeof(address);
#endif
    if (::getsockname(native_socket, reinterpret_cast<sockaddr *>(&address), &address_size) != 0)
    {
        std::cerr << "FAIL: native UDP getsockname\n";
        close_native();
        return false;
    }
    if (srt_bind_acquire(listener, native_socket) == SRT_ERROR)
    {
        std::cerr << "FAIL: srt_bind_acquire: " << srt_getlasterror_str() << '\n';
        close_native();
        return false;
    }
    return true;
}

bool create_session(const SRTSOCKET socket, Session &session,
                    const robotweax_srt_telemetry_public_srt_bistats_fn bistats = &srt_bistats)
{
    robotweax_srt_telemetry_public_srt_config_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config.socket = socket;
    config.provider = runtime_provider;
    config.capabilities = ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1;
    config.qualified_fields = runtime_fields;
    config.bistats = bistats;
    return robotweax_srt_telemetry_public_srt_session_create(&config, &session.value) ==
           ROBOTWEAX_SRT_TELEMETRY_OK;
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
int benchmark_bistats(const SRTSOCKET, SRT_TRACEBSTATS *statistics, const int clear,
                      const int instantaneous)
{
    if (statistics == nullptr || clear != 0 || instantaneous != 1)
    {
        return SRT_ERROR;
    }
    const auto sequence = benchmark_sequence.fetch_add(1, std::memory_order_relaxed);
    *statistics = {};
    statistics->pktSentTotal = static_cast<std::int64_t>(100 + sequence);
    statistics->pktRecvTotal = static_cast<std::int64_t>(100 + sequence);
    statistics->pktSentUniqueTotal = static_cast<std::int64_t>(100 + sequence);
    statistics->pktRecvUniqueTotal = static_cast<std::int64_t>(100 + sequence);
    statistics->byteSentTotal = 136'000 + sequence;
    statistics->byteRecvTotal = 136'000 + sequence;
    statistics->byteSentUniqueTotal = 136'000 + sequence;
    statistics->byteRecvUniqueTotal = 136'000 + sequence;
    statistics->msRTT = 2.0;
    return 0;
}

bool sample(Session &session, robotweax_srt_telemetry_public_srt_result_v1 &result)
{
    const auto unix_nano =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
    const auto monotonic_nano =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
    result = {};
    result.struct_size = sizeof(result);
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    if (robotweax_srt_telemetry_public_srt_session_sample(session.value, unix_nano, monotonic_nano,
                                                          &result) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        std::cerr << "FAIL: live adapter sample returned " << result.native_result << '\n';
        return false;
    }
    return true;
}

bool send_payload(const SRTSOCKET sender, const char fill)
{
    std::array<char, 1'316> outbound{};
    outbound.fill(fill);
    const auto sent =
        srt_sendmsg(sender, outbound.data(), static_cast<int>(outbound.size()), -1, 1);
    if (sent != static_cast<int>(outbound.size()))
    {
        return srt_ok(sent, "srt_sendmsg") && false;
    }
    return true;
}

bool receive_payload(const SRTSOCKET receiver, const char fill)
{
    std::array<char, 1'316> expected{};
    std::array<char, 1'316> inbound{};
    expected.fill(fill);
    const auto received = srt_recvmsg(receiver, inbound.data(), static_cast<int>(inbound.size()));
    if (received != static_cast<int>(inbound.size()))
    {
        return srt_ok(received, "srt_recvmsg") && false;
    }
    return inbound == expected;
}

bool has_bidirectional_deltas(const robotweax_srt_telemetry_public_srt_result_v1 &result)
{
    constexpr auto counter_fields = runtime_fields & ~ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS;
    return (result.raw.available_fields & runtime_fields) == runtime_fields &&
           (result.delta.available_delta_fields & counter_fields) == counter_fields &&
           result.delta.sent_packet_delta > 0 && result.delta.received_packet_delta > 0 &&
           result.delta.unique_sent_packet_delta > 0 &&
           result.delta.unique_received_packet_delta > 0 && result.delta.sent_wire_byte_delta > 0 &&
           result.delta.received_wire_byte_delta > 0 &&
           result.delta.unique_sent_wire_byte_delta > 0 &&
           result.delta.unique_received_wire_byte_delta > 0;
}

bool configure_caller(const SRTSOCKET socket)
{
    const int timeout_milliseconds = 5'000;
    return srt_ok(srt_setsockflag(socket, SRTO_CONNTIMEO, &timeout_milliseconds,
                                  sizeof(timeout_milliseconds)),
                  "caller connect timeout") &&
           srt_ok(srt_setsockflag(socket, SRTO_RCVTIMEO, &timeout_milliseconds,
                                  sizeof(timeout_milliseconds)),
                  "caller receive timeout");
}

bool configure_receiver(const SRTSOCKET socket)
{
    const int timeout_milliseconds = 5'000;
    return srt_ok(
        srt_setsockflag(socket, SRTO_RCVTIMEO, &timeout_milliseconds, sizeof(timeout_milliseconds)),
        "receive timeout");
}

bool wait_for_command(const std::string_view expected)
{
    std::string command;
    if (!std::getline(std::cin, command) || command != expected)
    {
        std::cerr << "FAIL: expected coordinator command " << expected << '\n';
        return false;
    }
    return true;
}

void emit_result(const std::string_view role,
                 const robotweax_srt_telemetry_public_srt_result_v1 &result)
{
    std::cout << std::setprecision(17) << "RESULT {\"provider\":\"" << runtime_provider_name
              << "\",\"role\":\"" << role
              << "\",\"availableFields\":" << result.raw.available_fields
              << ",\"intervalNano\":" << result.delta.interval_nano
              << ",\"sentPacketDelta\":" << result.delta.sent_packet_delta
              << ",\"receivedPacketDelta\":" << result.delta.received_packet_delta
              << ",\"uniqueSentPacketDelta\":" << result.delta.unique_sent_packet_delta
              << ",\"uniqueReceivedPacketDelta\":" << result.delta.unique_received_packet_delta
              << ",\"sentWireByteDelta\":" << result.delta.sent_wire_byte_delta
              << ",\"receivedWireByteDelta\":" << result.delta.received_wire_byte_delta
              << ",\"uniqueSentWireByteDelta\":" << result.delta.unique_sent_wire_byte_delta
              << ",\"uniqueReceivedWireByteDelta\":" << result.delta.unique_received_wire_byte_delta
              << ",\"rttSeconds\":" << result.raw.rtt_seconds << "}\n"
              << std::flush;
}

int run_self_loopback()
{
    Runtime runtime;
    if (!runtime.active())
    {
        std::cerr << "FAIL: srt_startup\n";
        return EXIT_FAILURE;
    }

    Socket listener(srt_create_socket());
    Socket caller(srt_create_socket());
    if (listener.get() == SRT_INVALID_SOCK || caller.get() == SRT_INVALID_SOCK)
    {
        std::cerr << "FAIL: socket creation\n";
        return EXIT_FAILURE;
    }

    if (!configure_caller(caller.get()))
    {
        return EXIT_FAILURE;
    }

    sockaddr_in address{};
    if (!bind_ephemeral_udp(listener.get(), address) ||
        !srt_ok(srt_listen(listener.get(), 1), "srt_listen"))
    {
        return EXIT_FAILURE;
    }

    if (!srt_ok(srt_connect(caller.get(), reinterpret_cast<const sockaddr *>(&address),
                            static_cast<int>(sizeof(address))),
                "srt_connect"))
    {
        return EXIT_FAILURE;
    }

    Socket accepted(srt_accept(listener.get(), nullptr, nullptr));
    if (accepted.get() == SRT_INVALID_SOCK || !configure_receiver(accepted.get()))
    {
        std::cerr << "FAIL: srt_accept\n";
        return EXIT_FAILURE;
    }

    Session caller_session;
    Session accepted_session;
    if (!create_session(caller.get(), caller_session) ||
        !create_session(accepted.get(), accepted_session))
    {
        std::cerr << "FAIL: telemetry session creation\n";
        return EXIT_FAILURE;
    }

    robotweax_srt_telemetry_public_srt_result_v1 caller_result{};
    robotweax_srt_telemetry_public_srt_result_v1 accepted_result{};
    if (!sample(caller_session, caller_result) || !sample(accepted_session, accepted_result) ||
        !send_payload(caller.get(), 'A') || !receive_payload(accepted.get(), 'A') ||
        !send_payload(accepted.get(), 'B') || !receive_payload(caller.get(), 'B'))
    {
        std::cerr << "FAIL: bidirectional payload exchange\n";
        return EXIT_FAILURE;
    }

    if (!sample(caller_session, caller_result) || !sample(accepted_session, accepted_result) ||
        !has_bidirectional_deltas(caller_result) || !has_bidirectional_deltas(accepted_result))
    {
        std::cerr << "FAIL: live samples did not expose bidirectional packet deltas\n";
        return EXIT_FAILURE;
    }

    std::cout << "public SRT adapter live loopback passed\n";
    return EXIT_SUCCESS;
}

int run_listener_peer()
{
    Runtime runtime;
    Socket listener(srt_create_socket());
    if (!runtime.active() || listener.get() == SRT_INVALID_SOCK)
    {
        std::cerr << "FAIL: listener startup\n";
        return EXIT_FAILURE;
    }

    sockaddr_in address{};
    if (!bind_ephemeral_udp(listener.get(), address) ||
        !srt_ok(srt_listen(listener.get(), 1), "srt_listen"))
    {
        return EXIT_FAILURE;
    }
    std::cout << "READY {\"port\":" << ntohs(address.sin_port) << "}\n" << std::flush;

    Socket accepted(srt_accept(listener.get(), nullptr, nullptr));
    if (accepted.get() == SRT_INVALID_SOCK || !configure_receiver(accepted.get()))
    {
        std::cerr << "FAIL: srt_accept\n";
        return EXIT_FAILURE;
    }
    Session session;
    if (!create_session(accepted.get(), session))
    {
        std::cerr << "FAIL: listener telemetry session creation\n";
        return EXIT_FAILURE;
    }
    std::cout << "CONNECTED\n" << std::flush;

    robotweax_srt_telemetry_public_srt_result_v1 result{};
    if (!wait_for_command("BASELINE") || !sample(session, result))
    {
        return EXIT_FAILURE;
    }
    std::cout << "BASELINED\n" << std::flush;
    if (!wait_for_command("TRANSFER") || !receive_payload(accepted.get(), 'A') ||
        !send_payload(accepted.get(), 'B'))
    {
        std::cerr << "FAIL: listener payload transfer\n";
        return EXIT_FAILURE;
    }
    std::cout << "TRANSFERRED\n" << std::flush;
    if (!wait_for_command("SAMPLE") || !sample(session, result) ||
        !has_bidirectional_deltas(result))
    {
        std::cerr << "FAIL: listener bidirectional telemetry\n";
        return EXIT_FAILURE;
    }
    emit_result("listener", result);
    return EXIT_SUCCESS;
}

int parse_port(const char *text)
{
    char *end = nullptr;
    const auto value = std::strtol(text, &end, 10);
    return end != text && *end == '\0' && value > 0 && value <= 65'535 ? static_cast<int>(value)
                                                                       : 0;
}

int run_caller_peer(const int port)
{
    Runtime runtime;
    Socket caller(srt_create_socket());
    if (!runtime.active() || caller.get() == SRT_INVALID_SOCK || !configure_caller(caller.get()))
    {
        std::cerr << "FAIL: caller startup\n";
        return EXIT_FAILURE;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (!srt_ok(srt_connect(caller.get(), reinterpret_cast<const sockaddr *>(&address),
                            static_cast<int>(sizeof(address))),
                "srt_connect"))
    {
        return EXIT_FAILURE;
    }
    Session session;
    if (!create_session(caller.get(), session))
    {
        std::cerr << "FAIL: caller telemetry session creation\n";
        return EXIT_FAILURE;
    }
    std::cout << "CONNECTED\n" << std::flush;

    robotweax_srt_telemetry_public_srt_result_v1 result{};
    if (!wait_for_command("BASELINE") || !sample(session, result))
    {
        return EXIT_FAILURE;
    }
    std::cout << "BASELINED\n" << std::flush;
    if (!wait_for_command("TRANSFER") || !send_payload(caller.get(), 'A') ||
        !receive_payload(caller.get(), 'B'))
    {
        std::cerr << "FAIL: caller payload transfer\n";
        return EXIT_FAILURE;
    }
    std::cout << "TRANSFERRED\n" << std::flush;
    if (!wait_for_command("SAMPLE") || !sample(session, result) ||
        !has_bidirectional_deltas(result))
    {
        std::cerr << "FAIL: caller bidirectional telemetry\n";
        return EXIT_FAILURE;
    }
    emit_result("caller", result);
    return EXIT_SUCCESS;
}

struct TimingSummary
{
    double mean_nanoseconds = 0.0;
    double minimum_nanoseconds = 0.0;
    double p50_nanoseconds = 0.0;
    double p95_nanoseconds = 0.0;
    double maximum_nanoseconds = 0.0;
};

TimingSummary summarize_timings(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    const auto percentile = [&](const double fraction)
    {
        const auto rank = static_cast<std::size_t>(
            std::ceil(fraction * static_cast<double>(values.size())) - 1.0);
        return values[std::min(rank, values.size() - 1)];
    };
    TimingSummary summary{};
    summary.mean_nanoseconds =
        std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    summary.minimum_nanoseconds = values.front();
    summary.p50_nanoseconds = percentile(0.50);
    summary.p95_nanoseconds = percentile(0.95);
    summary.maximum_nanoseconds = values.back();
    return summary;
}

template <typename Operation>
bool measure_batch(const std::size_t iterations, Operation operation,
                   double &nanoseconds_per_sample)
{
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < iterations; ++iteration)
    {
        if (!operation())
        {
            return false;
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    nanoseconds_per_sample = static_cast<double>(elapsed) / static_cast<double>(iterations);
    return true;
}

template <typename DirectOperation, typename AdapterOperation>
bool measure_paired_batches(const std::size_t batches, const std::size_t iterations,
                            DirectOperation direct_operation, AdapterOperation adapter_operation,
                            std::vector<double> &direct_timings,
                            std::vector<double> &adapter_timings,
                            std::vector<double> &incremental_timings)
{
    direct_timings.clear();
    adapter_timings.clear();
    incremental_timings.clear();
    direct_timings.reserve(batches);
    adapter_timings.reserve(batches);
    incremental_timings.reserve(batches);
    for (std::size_t batch = 0; batch < batches; ++batch)
    {
        double direct_nanoseconds = 0.0;
        double adapter_nanoseconds = 0.0;
        const auto direct_first = batch % 2 == 0;
        const auto first_ok =
            direct_first ? measure_batch(iterations, direct_operation, direct_nanoseconds)
                         : measure_batch(iterations, adapter_operation, adapter_nanoseconds);
        const auto second_ok =
            direct_first ? measure_batch(iterations, adapter_operation, adapter_nanoseconds)
                         : measure_batch(iterations, direct_operation, direct_nanoseconds);
        if (!first_ok || !second_ok)
        {
            return false;
        }
        direct_timings.push_back(direct_nanoseconds);
        adapter_timings.push_back(adapter_nanoseconds);
        incremental_timings.push_back(adapter_nanoseconds - direct_nanoseconds);
    }
    return true;
}

void emit_timing_summary(const TimingSummary &summary)
{
    std::cout << "{\"meanNanoseconds\":" << summary.mean_nanoseconds
              << ",\"minimumNanoseconds\":" << summary.minimum_nanoseconds
              << ",\"p50Nanoseconds\":" << summary.p50_nanoseconds
              << ",\"p95Nanoseconds\":" << summary.p95_nanoseconds
              << ",\"maximumNanoseconds\":" << summary.maximum_nanoseconds << '}';
}

void emit_benchmark_pair(const TimingSummary &direct, const TimingSummary &adapter,
                         const TimingSummary &incremental)
{
    const auto ratio =
        direct.mean_nanoseconds > 0.0 ? adapter.mean_nanoseconds / direct.mean_nanoseconds : 0.0;
    std::cout << "{\"direct\":";
    emit_timing_summary(direct);
    std::cout << ",\"adapter\":";
    emit_timing_summary(adapter);
    std::cout << ",\"incremental\":";
    emit_timing_summary(incremental);
    std::cout << ",\"adapterToDirectRatio\":" << ratio << '}';
}

int run_sampling_benchmark()
{
    constexpr std::size_t warmup_iterations = 1'000;
    constexpr std::size_t batches = 25;
    constexpr std::size_t iterations_per_batch = 2'000;

    Runtime runtime;
    Socket listener(srt_create_socket());
    Socket caller(srt_create_socket());
    if (!runtime.active() || listener.get() == SRT_INVALID_SOCK ||
        caller.get() == SRT_INVALID_SOCK || !configure_caller(caller.get()))
    {
        std::cerr << "FAIL: benchmark startup\n";
        return EXIT_FAILURE;
    }
    sockaddr_in address{};
    if (!bind_ephemeral_udp(listener.get(), address) ||
        !srt_ok(srt_listen(listener.get(), 1), "benchmark listen") ||
        !srt_ok(srt_connect(caller.get(), reinterpret_cast<const sockaddr *>(&address),
                            static_cast<int>(sizeof(address))),
                "benchmark connect"))
    {
        return EXIT_FAILURE;
    }
    Socket accepted(srt_accept(listener.get(), nullptr, nullptr));
    if (accepted.get() == SRT_INVALID_SOCK)
    {
        std::cerr << "FAIL: benchmark accept\n";
        return EXIT_FAILURE;
    }
    Session session;
    if (!create_session(caller.get(), session))
    {
        std::cerr << "FAIL: benchmark telemetry session creation\n";
        return EXIT_FAILURE;
    }

    SRT_TRACEBSTATS statistics{};
    const auto direct_sample = [&]() { return srt_bistats(caller.get(), &statistics, 0, 1) == 0; };

    robotweax_srt_telemetry_public_srt_result_v1 result{};
    result.struct_size = sizeof(result);
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    std::uint64_t monotonic_nano = 1'000'000'000ULL;
    const auto adapter_sample = [&]()
    {
        monotonic_nano += 1'000'000ULL;
        return robotweax_srt_telemetry_public_srt_session_sample(
                   session.value, 1'800'000'000'000'000'000ULL + monotonic_nano, monotonic_nano,
                   &result) == ROBOTWEAX_SRT_TELEMETRY_OK;
    };

    for (std::size_t iteration = 0; iteration < warmup_iterations; ++iteration)
    {
        if (!direct_sample() || !adapter_sample())
        {
            std::cerr << "FAIL: benchmark warmup\n";
            return EXIT_FAILURE;
        }
    }

    std::vector<double> direct_timings;
    std::vector<double> adapter_timings;
    std::vector<double> incremental_timings;
    if (!measure_paired_batches(batches, iterations_per_batch, direct_sample, adapter_sample,
                                direct_timings, adapter_timings, incremental_timings))
    {
        std::cerr << "FAIL: provider benchmark sample\n";
        return EXIT_FAILURE;
    }
    const auto provider_direct = summarize_timings(direct_timings);
    const auto provider_adapter = summarize_timings(adapter_timings);
    const auto provider_incremental = summarize_timings(incremental_timings);

    Session synthetic_session;
    if (!create_session(1, synthetic_session, &benchmark_bistats))
    {
        std::cerr << "FAIL: synthetic benchmark session creation\n";
        return EXIT_FAILURE;
    }
    SRT_TRACEBSTATS synthetic_statistics{};
    const auto synthetic_direct = [&]()
    { return benchmark_bistats(1, &synthetic_statistics, 0, 1) == 0; };
    robotweax_srt_telemetry_public_srt_result_v1 synthetic_result{};
    synthetic_result.struct_size = sizeof(synthetic_result);
    synthetic_result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    std::uint64_t synthetic_monotonic_nano = 1'000'000'000ULL;
    const auto synthetic_adapter = [&]()
    {
        synthetic_monotonic_nano += 1'000'000ULL;
        return robotweax_srt_telemetry_public_srt_session_sample(
                   synthetic_session.value, 1'800'000'000'000'000'000ULL + synthetic_monotonic_nano,
                   synthetic_monotonic_nano, &synthetic_result) == ROBOTWEAX_SRT_TELEMETRY_OK;
    };
    for (std::size_t iteration = 0; iteration < warmup_iterations; ++iteration)
    {
        if (!synthetic_direct() || !synthetic_adapter())
        {
            std::cerr << "FAIL: synthetic benchmark warmup\n";
            return EXIT_FAILURE;
        }
    }
    if (!measure_paired_batches(batches, iterations_per_batch, synthetic_direct, synthetic_adapter,
                                direct_timings, adapter_timings, incremental_timings))
    {
        std::cerr << "FAIL: synthetic benchmark sample\n";
        return EXIT_FAILURE;
    }
    const auto synthetic_direct_summary = summarize_timings(direct_timings);
    const auto synthetic_adapter_summary = summarize_timings(adapter_timings);
    const auto synthetic_incremental_summary = summarize_timings(incremental_timings);

    std::cout << std::setprecision(17) << "BENCHMARK {\"provider\":\"" << runtime_provider_name
              << "\",\"warmupIterations\":" << warmup_iterations << ",\"batches\":" << batches
              << ",\"iterationsPerBatch\":" << iterations_per_batch << ",\"providerCall\":";
    emit_benchmark_pair(provider_direct, provider_adapter, provider_incremental);
    std::cout << ",\"syntheticSdk\":";
    emit_benchmark_pair(synthetic_direct_summary, synthetic_adapter_summary,
                        synthetic_incremental_summary);
    std::cout << "}\n";
    return EXIT_SUCCESS;
}

int run_close_sample_lifecycle()
{
    Runtime runtime;
    Socket listener(srt_create_socket());
    Socket caller(srt_create_socket());
    if (!runtime.active() || listener.get() == SRT_INVALID_SOCK ||
        caller.get() == SRT_INVALID_SOCK || !configure_caller(caller.get()))
    {
        std::cerr << "FAIL: lifecycle startup\n";
        return EXIT_FAILURE;
    }
    sockaddr_in address{};
    if (!bind_ephemeral_udp(listener.get(), address) ||
        !srt_ok(srt_listen(listener.get(), 1), "lifecycle listen") ||
        !srt_ok(srt_connect(caller.get(), reinterpret_cast<const sockaddr *>(&address),
                            static_cast<int>(sizeof(address))),
                "lifecycle connect"))
    {
        return EXIT_FAILURE;
    }
    Socket accepted(srt_accept(listener.get(), nullptr, nullptr));
    if (accepted.get() == SRT_INVALID_SOCK)
    {
        std::cerr << "FAIL: lifecycle accept\n";
        return EXIT_FAILURE;
    }

    const auto stale_socket = caller.get();
    {
        Session orderly_session;
        robotweax_srt_telemetry_public_srt_result_v1 orderly_result{};
        if (!create_session(stale_socket, orderly_session) ||
            !sample(orderly_session, orderly_result))
        {
            std::cerr << "FAIL: supported session-before-close lifecycle\n";
            return EXIT_FAILURE;
        }
    }

    const auto close_started = std::chrono::steady_clock::now();
    const auto close_result = caller.close();
    const auto close_elapsed_nano = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - close_started)
                                        .count();

    Session stale_session;
    if (!create_session(stale_socket, stale_session))
    {
        std::cerr << "FAIL: stale-handle qualification session creation\n";
        return EXIT_FAILURE;
    }
    robotweax_srt_telemetry_public_srt_result_v1 post_close_result{};
    post_close_result.struct_size = sizeof(post_close_result);
    post_close_result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    const auto post_close_started = std::chrono::steady_clock::now();
    const auto post_close_error = robotweax_srt_telemetry_public_srt_session_sample(
        stale_session.value, 1'800'000'010'000'000'000ULL, 10'000'000'000ULL, &post_close_result);
    const auto post_close_elapsed_nano = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - post_close_started)
                                             .count();

    if (close_result == SRT_ERROR ||
        post_close_error != ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE ||
        post_close_result.native_result != SRT_ERROR || post_close_result.raw.available_fields != 0)
    {
        std::cerr << "FAIL: deterministic close/sample lifecycle contract\n";
        return EXIT_FAILURE;
    }

    std::cout << "LIFECYCLE {\"provider\":\"" << runtime_provider_name
              << "\",\"sessionDestroyedBeforeClose\":true,"
                 "\"staleHandleProbeUnsupported\":true,\"closeNativeResult\":"
              << close_result << ",\"closeElapsedNano\":" << close_elapsed_nano
              << ",\"postCloseError\":" << post_close_error
              << ",\"postCloseNativeResult\":" << post_close_result.native_result
              << ",\"postCloseAvailableFields\":" << post_close_result.raw.available_fields
              << ",\"postCloseElapsedNano\":" << post_close_elapsed_nano << "}\n";
    return EXIT_SUCCESS;
}

int run_unsafe_close_sample_race()
{
    const auto race_started = std::chrono::steady_clock::now();
    Runtime runtime;
    Socket listener(srt_create_socket());
    Socket caller(srt_create_socket());
    if (!runtime.active() || listener.get() == SRT_INVALID_SOCK ||
        caller.get() == SRT_INVALID_SOCK || !configure_caller(caller.get()))
    {
        std::cerr << "FAIL: lifecycle startup\n";
        return EXIT_FAILURE;
    }
    sockaddr_in address{};
    if (!bind_ephemeral_udp(listener.get(), address) ||
        !srt_ok(srt_listen(listener.get(), 1), "lifecycle listen") ||
        !srt_ok(srt_connect(caller.get(), reinterpret_cast<const sockaddr *>(&address),
                            static_cast<int>(sizeof(address))),
                "lifecycle connect"))
    {
        return EXIT_FAILURE;
    }
    Socket accepted(srt_accept(listener.get(), nullptr, nullptr));
    if (accepted.get() == SRT_INVALID_SOCK)
    {
        std::cerr << "FAIL: lifecycle accept\n";
        return EXIT_FAILURE;
    }

    {
        Session orderly_session;
        robotweax_srt_telemetry_public_srt_result_v1 orderly_result{};
        if (!create_session(caller.get(), orderly_session) ||
            !sample(orderly_session, orderly_result))
        {
            std::cerr << "FAIL: supported stop-before-close lifecycle\n";
            return EXIT_FAILURE;
        }
    }

    Session racing_session;
    if (!create_session(caller.get(), racing_session))
    {
        std::cerr << "FAIL: racing telemetry session creation\n";
        return EXIT_FAILURE;
    }

    std::atomic<bool> stop{false};
    std::atomic<bool> close_started{false};
    std::atomic<std::uint64_t> successful_samples{0};
    std::atomic<std::uint64_t> provider_failures{0};
    std::atomic<int> failure_native_result{0};
    std::atomic<int> unexpected_error{ROBOTWEAX_SRT_TELEMETRY_OK};
    std::thread sampler(
        [&]()
        {
            robotweax_srt_telemetry_public_srt_result_v1 result{};
            result.struct_size = sizeof(result);
            result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
            std::uint64_t monotonic_nano = 1'000'000'000ULL;
            while (!stop.load(std::memory_order_relaxed))
            {
                monotonic_nano += 1'000'000ULL;
                const auto error = robotweax_srt_telemetry_public_srt_session_sample(
                    racing_session.value, 1'800'000'000'000'000'000ULL + monotonic_nano,
                    monotonic_nano, &result);
                if (error == ROBOTWEAX_SRT_TELEMETRY_OK)
                {
                    successful_samples.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (error == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE &&
                    close_started.load(std::memory_order_acquire))
                {
                    failure_native_result.store(result.native_result, std::memory_order_relaxed);
                    provider_failures.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                unexpected_error.store(error, std::memory_order_relaxed);
                break;
            }
        });

    const auto wait_until = [](const auto &predicate, const std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return predicate();
    };

    const auto sampled_before_close =
        wait_until([&]() { return successful_samples.load(std::memory_order_relaxed) >= 100; },
                   std::chrono::seconds(2));
    close_started.store(true, std::memory_order_release);
    const auto close_result = caller.close();
    const auto observed_failure = wait_until(
        [&]()
        {
            return provider_failures.load(std::memory_order_relaxed) > 0 ||
                   unexpected_error.load(std::memory_order_relaxed) != ROBOTWEAX_SRT_TELEMETRY_OK;
        },
        std::chrono::seconds(2));
    stop.store(true, std::memory_order_relaxed);
    sampler.join();

    robotweax_srt_telemetry_public_srt_result_v1 post_close_result{};
    post_close_result.struct_size = sizeof(post_close_result);
    post_close_result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    const auto post_close_error = robotweax_srt_telemetry_public_srt_session_sample(
        racing_session.value, 1'800'000'010'000'000'000ULL, 10'000'000'000ULL, &post_close_result);

    if (!sampled_before_close || !observed_failure || close_result == SRT_ERROR ||
        unexpected_error.load(std::memory_order_relaxed) != ROBOTWEAX_SRT_TELEMETRY_OK ||
        provider_failures.load(std::memory_order_relaxed) == 0 ||
        failure_native_result.load(std::memory_order_relaxed) != SRT_ERROR ||
        post_close_error != ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE ||
        post_close_result.native_result != SRT_ERROR || post_close_result.raw.available_fields != 0)
    {
        std::cerr << "FAIL: close/sample lifecycle contract\n";
        return EXIT_FAILURE;
    }

    const auto elapsed_nano = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - race_started)
                                  .count();
    std::cout << "CLOSE_RACE {\"provider\":\"" << runtime_provider_name
              << "\",\"orderlySessionBeforeClose\":true,\"successfulRaceSamples\":"
              << successful_samples.load(std::memory_order_relaxed)
              << ",\"providerFailuresAfterClose\":"
              << provider_failures.load(std::memory_order_relaxed)
              << ",\"raceFailureNativeResult\":"
              << failure_native_result.load(std::memory_order_relaxed)
              << ",\"closeNativeResult\":" << close_result
              << ",\"postCloseError\":" << post_close_error
              << ",\"postCloseNativeResult\":" << post_close_result.native_result
              << ",\"postCloseAvailableFields\":" << post_close_result.raw.available_fields
              << ",\"elapsedNano\":" << elapsed_nano << "}\n";
    return EXIT_SUCCESS;
}

} // namespace

int main(const int argc, char **argv)
{
    if (argc == 1)
    {
        return run_self_loopback();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark")
    {
        return run_sampling_benchmark();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--lifecycle")
    {
        return run_close_sample_lifecycle();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--close-race")
    {
        return run_unsafe_close_sample_race();
    }
    if (argc == 3 && std::string_view(argv[1]) == "--role" &&
        std::string_view(argv[2]) == "listener")
    {
        return run_listener_peer();
    }
    if (argc == 5 && std::string_view(argv[1]) == "--role" &&
        std::string_view(argv[2]) == "caller" && std::string_view(argv[3]) == "--port")
    {
        const auto port = parse_port(argv[4]);
        return port == 0 ? EXIT_FAILURE : run_caller_peer(port);
    }
    std::cerr << "usage: runtime-test [--benchmark | --lifecycle | --close-race | --role "
                 "listener | --role caller --port PORT]\n";
    return EXIT_FAILURE;
}
