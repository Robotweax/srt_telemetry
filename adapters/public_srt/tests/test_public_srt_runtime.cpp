#include <robotweax/srt_telemetry_public_srt.h>

#if defined(ROBOTWEAX_SRT_TELEMETRY_RUNTIME_USE_FLAT_SRT_HEADER)
#include <srt.h>
#else
#include <srt/srt.h>
#endif

#if defined(ROBOTWEAX_SRT_TELEMETRY_RUNTIME_USE_FLAT_SRT_HEADER)
#define ROBOTWEAX_SRT_TELEMETRY_USE_FLAT_SRT_HEADER
#endif
#include <robotweax/srt_telemetry_public_srt_host.h>

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
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <mutex>
#include <map>
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

    void adopt(const SRTSOCKET socket) noexcept
    {
        static_cast<void>(close());
        socket_ = socket;
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


constexpr std::uint64_t coverage_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_V1_5_5_CANDIDATE_FIELDS;
constexpr std::uint64_t coverage_ext_fields =
#if defined(ROBOTWEAX_SRT_TELEMETRY_RUNTIME_PROVIDER_ROBOTWEAX)
    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_ROBOTWEAX_EXT_CANDIDATE_FIELDS;
#else
    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_HAIVISION_V1_5_5_EXT_CANDIDATE_FIELDS;
#endif

bool create_full_session(const SRTSOCKET socket, Session &session)
{
    robotweax_srt_telemetry_public_srt_config_v2 config{};
    config.struct_size = sizeof(config);
    config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config.socket = socket;
    config.provider = runtime_provider;
    config.capabilities =
        ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1 | ROBOTWEAX_SRT_TELEMETRY_CAP_FEC_STATS_V1;
    config.qualified_fields = coverage_fields;
    config.qualified_ext_fields = coverage_ext_fields;
    config.bistats = &srt_bistats;
    return robotweax_srt_telemetry_public_srt_session_create_v2(&config, &session.value) ==
           ROBOTWEAX_SRT_TELEMETRY_OK;
}

bool sample_full(Session &session, robotweax_srt_telemetry_public_srt_result_v1 &result,
                 robotweax_srt_telemetry_public_srt_ext_result_v1 &ext)
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
    ext = {};
    ext.struct_size = sizeof(ext);
    ext.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    return robotweax_srt_telemetry_public_srt_session_sample_ext(
               session.value, unix_nano, monotonic_nano, &result, &ext) ==
           ROBOTWEAX_SRT_TELEMETRY_OK;
}

void report_missing(const std::string_view endpoint, const std::string_view set,
                    const std::uint64_t expected, const std::uint64_t actual)
{
    const auto missing = expected & ~actual;
    if (missing != 0)
    {
        std::cerr << "FAIL: " << endpoint << " " << set << " missing bits 0x" << std::hex
                  << missing << std::dec << '\n';
    }
}

bool endpoint_covered(const std::string_view endpoint,
                      const robotweax_srt_telemetry_public_srt_result_v1 &result,
                      const robotweax_srt_telemetry_public_srt_ext_result_v1 &ext)
{
    constexpr auto ext_counters = coverage_ext_fields & ROBOTWEAX_SRT_TELEMETRY_EXT_COUNTER_FIELDS;
    report_missing(endpoint, "v1 fields", coverage_fields, result.raw.available_fields);
    report_missing(endpoint, "ext fields", coverage_ext_fields, ext.ext.available_fields);
    report_missing(endpoint, "ext deltas", ext_counters, ext.ext_delta.available_delta_fields);
    const auto complete = result.raw.available_fields == coverage_fields &&
                          ext.ext.available_fields == coverage_ext_fields &&
                          ext.ext_delta.available_delta_fields == ext_counters;
    /* Sanity relations that hold on a healthy bidirectional loopback. */
    const auto plausible = ext.ext.connection_age_seconds > 0.0 && ext.ext.mss_bytes > 0 &&
                           ext.ext_delta.sent_ack_packet_delta > 0 &&
                           ext.ext_delta.received_ack_packet_delta > 0 &&
                           ext.ext_delta.sender_active_nano_delta > 0 &&
                           ext.ext.filter_unrecovered_packet_total == 0;
    if (!plausible)
    {
        std::cerr << "FAIL: " << endpoint << " implausible values: age "
                  << ext.ext.connection_age_seconds << " s, MSS " << ext.ext.mss_bytes
                  << ", ACK sent/received delta " << ext.ext_delta.sent_ack_packet_delta << "/"
                  << ext.ext_delta.received_ack_packet_delta << ", sender-active delta "
                  << ext.ext_delta.sender_active_nano_delta << " ns\n";
    }
    return complete && plausible;
}

int run_field_coverage()
{
    Runtime runtime;
    if (!runtime.active())
    {
        std::cerr << "FAIL: srt_startup\n";
        return EXIT_FAILURE;
    }
    Socket listener(srt_create_socket());
    Socket caller(srt_create_socket());
    sockaddr_in address{};
    if (listener.get() == SRT_INVALID_SOCK || caller.get() == SRT_INVALID_SOCK ||
        !configure_caller(caller.get()) || !bind_ephemeral_udp(listener.get(), address) ||
        !srt_ok(srt_listen(listener.get(), 1), "srt_listen") ||
        !srt_ok(srt_connect(caller.get(), reinterpret_cast<const sockaddr *>(&address),
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
    if (!create_full_session(caller.get(), caller_session) ||
        !create_full_session(accepted.get(), accepted_session))
    {
        std::cerr << "FAIL: full-field session creation\n";
        return EXIT_FAILURE;
    }

    robotweax_srt_telemetry_public_srt_result_v1 caller_result{};
    robotweax_srt_telemetry_public_srt_result_v1 accepted_result{};
    robotweax_srt_telemetry_public_srt_ext_result_v1 caller_ext{};
    robotweax_srt_telemetry_public_srt_ext_result_v1 accepted_ext{};
    if (!sample_full(caller_session, caller_result, caller_ext) ||
        !sample_full(accepted_session, accepted_result, accepted_ext))
    {
        std::cerr << "FAIL: baseline full-field sample\n";
        return EXIT_FAILURE;
    }

    /* Pipeline the messages: a ping-pong exchange would wait out the TSBPD
     * latency (120 ms by default) for every message. */
    constexpr int message_count = 50;
    for (int message = 0; message < message_count; ++message)
    {
        const auto fill = static_cast<char>('a' + message % 26);
        if (!send_payload(caller.get(), fill) || !send_payload(accepted.get(), fill))
        {
            std::cerr << "FAIL: payload send " << message << '\n';
            return EXIT_FAILURE;
        }
    }
    for (int message = 0; message < message_count; ++message)
    {
        const auto fill = static_cast<char>('a' + message % 26);
        if (!receive_payload(accepted.get(), fill) || !receive_payload(caller.get(), fill))
        {
            std::cerr << "FAIL: payload receive " << message << '\n';
            return EXIT_FAILURE;
        }
    }
    /* Full ACKs run every 10 ms; 200 ms leaves margin on a loaded CI host. */
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    if (!sample_full(caller_session, caller_result, caller_ext) ||
        !sample_full(accepted_session, accepted_result, accepted_ext))
    {
        std::cerr << "FAIL: full-field sample after exchange\n";
        return EXIT_FAILURE;
    }

    const auto caller_ok = endpoint_covered("caller", caller_result, caller_ext);
    const auto accepted_ok = endpoint_covered("listener", accepted_result, accepted_ext);
    const auto mapped = std::popcount(coverage_fields) + std::popcount(coverage_ext_fields);
    std::cout << "FIELD_COVERAGE {\"provider\":\"" << runtime_provider_name
              << "\",\"mappedObservations\":" << mapped
              << ",\"v1Fields\":" << std::popcount(coverage_fields)
              << ",\"extFields\":" << std::popcount(coverage_ext_fields)
              << ",\"excludedObservations\":2,\"complete\":"
              << (caller_ok && accepted_ok ? "true" : "false") << "}\n";
    return caller_ok && accepted_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}


struct Monitor final
{
    ~Monitor()
    {
        robotweax_srt_telemetry_public_srt_connection_monitor_destroy(value);
    }

    robotweax_srt_telemetry_public_srt_connection_monitor *value = nullptr;
};

bool create_monitor(const SRTSOCKET socket, Monitor &monitor, const bool capture_stream_id = false)
{
    robotweax_srt_telemetry_public_srt_connection_config_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config.socket = socket;
    config.provider = runtime_provider;
    config.capture_stream_id = capture_stream_id ? 1u : 0u;
    robotweax_srt_telemetry_host_connection_functions(&config);
    return robotweax_srt_telemetry_public_srt_connection_monitor_create(&config, &monitor.value) ==
           ROBOTWEAX_SRT_TELEMETRY_OK;
}

robotweax_srt_telemetry_public_srt_connection_result_v1 poll_monitor(Monitor &monitor)
{
    robotweax_srt_telemetry_public_srt_connection_result_v1 result{};
    result.struct_size = sizeof(result);
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (robotweax_srt_telemetry_public_srt_connection_monitor_poll(monitor.value, now, now,
                                                                   &result) !=
        ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        std::cerr << "FAIL: connection monitor poll\n";
    }
    return result;
}

/* Polls until predicate(result) holds or the deadline passes; returns the
 * union of all events seen and the last result. */
template <typename Predicate>
bool poll_until(Monitor &monitor, Predicate predicate,
                robotweax_srt_telemetry_public_srt_connection_result_v1 &last,
                std::uint64_t &seen_events, const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        last = poll_monitor(monitor);
        seen_events |= last.events;
        if (predicate(last))
        {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

struct PairOptions
{
    const char *caller_passphrase = nullptr;
    const char *listener_passphrase = nullptr;
    const char *packet_filter = nullptr;
    const char *stream_id = nullptr;
};

bool set_text(const SRTSOCKET socket, const SRT_SOCKOPT option, const char *value,
              const std::string_view name)
{
    return value == nullptr ||
           srt_ok(srt_setsockflag(socket, option, value, static_cast<int>(std::strlen(value))),
                  name);
}

/* Connects caller to a listener. Returns false if the connection failed;
 * accepted is left invalid in that case. */
bool connect_pair(const PairOptions &options, Socket &listener, Socket &caller, Socket &accepted,
                  const bool expect_failure = false)
{
    sockaddr_in address{};
    if (listener.get() == SRT_INVALID_SOCK || caller.get() == SRT_INVALID_SOCK ||
        !configure_caller(caller.get()) ||
        !set_text(caller.get(), SRTO_PASSPHRASE, options.caller_passphrase, "caller passphrase") ||
        !set_text(listener.get(), SRTO_PASSPHRASE, options.listener_passphrase,
                  "listener passphrase") ||
        !set_text(caller.get(), SRTO_PACKETFILTER, options.packet_filter, "caller filter") ||
        !set_text(listener.get(), SRTO_PACKETFILTER, options.packet_filter, "listener filter") ||
        !set_text(caller.get(), SRTO_STREAMID, options.stream_id, "stream id") ||
        !bind_ephemeral_udp(listener.get(), address) ||
        !srt_ok(srt_listen(listener.get(), 1), "srt_listen"))
    {
        return false;
    }
    const auto connected = srt_connect(caller.get(), reinterpret_cast<const sockaddr *>(&address),
                                       static_cast<int>(sizeof(address)));
    if (connected == SRT_ERROR)
    {
        if (!expect_failure)
        {
            std::cerr << "FAIL: srt_connect: " << srt_getlasterror_str() << '\n';
        }
        return false;
    }
    accepted.adopt(srt_accept(listener.get(), nullptr, nullptr));
    if (accepted.get() == SRT_INVALID_SOCK || !configure_receiver(accepted.get()))
    {
        std::cerr << "FAIL: srt_accept\n";
        return false;
    }
    return true;
}

bool scenario_plain_and_disconnect()
{
    Socket listener(srt_create_socket());
    Socket caller(srt_create_socket());
    Socket accepted;
    PairOptions options;
    options.stream_id = "telemetry-test-stream";
    if (!connect_pair(options, listener, caller, accepted))
    {
        return false;
    }
    Monitor caller_monitor;
    Monitor accepted_monitor;
    if (!create_monitor(caller.get(), caller_monitor) ||
        !create_monitor(accepted.get(), accepted_monitor, true))
    {
        std::cerr << "FAIL: monitor creation\n";
        return false;
    }
    bool ok = true;
    for (auto *monitor : {&caller_monitor, &accepted_monitor})
    {
        const auto first = poll_monitor(*monitor);
        const auto &s = first.snapshot;
        constexpr auto required =
            ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_STATE |
            ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_REJECT_REASON |
            ROBOTWEAX_SRT_TELEMETRY_CONN_SETTINGS_FIELDS | ROBOTWEAX_SRT_TELEMETRY_CONN_KM_FIELDS |
            ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PEER_SRT_VERSION |
            ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_LOCAL_SRT_VERSION;
        if ((s.available_fields & required) != required)
        {
            std::cerr << "FAIL: connection fields missing 0x" << std::hex
                      << (required & ~s.available_fields) << std::dec << '\n';
            ok = false;
        }
        if ((first.events & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_CONNECTED) == 0 ||
            s.state != SRTS_CONNECTED || s.reject_reason != 0 ||
            std::abs(s.receiver_latency_seconds - 0.12) > 1e-9 || s.peer_srt_version == 0 ||
            s.km_state != SRT_KM_S_UNSECURED || s.key_length_bytes != 0 ||
            s.packet_filter[0] != '\0')
        {
            std::cerr << "FAIL: plain snapshot state " << s.state << ", latency "
                      << s.receiver_latency_seconds << ", peer version " << s.peer_srt_version
                      << ", KM " << s.km_state << '\n';
            ok = false;
        }
        if (monitor == &caller_monitor &&
            (s.available_fields & ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_STREAM_ID) != 0)
        {
            std::cerr << "FAIL: stream ID captured without opt-in\n";
            ok = false;
        }
        if (monitor == &accepted_monitor &&
            std::string_view(s.stream_id) != "telemetry-test-stream")
        {
            std::cerr << "FAIL: listener did not report the caller's stream ID: '" << s.stream_id
                      << "'\n";
            ok = false;
        }
    }

    static_cast<void>(caller.close());
    robotweax_srt_telemetry_public_srt_connection_result_v1 last{};
    std::uint64_t seen = 0;
    if (!poll_until(
            accepted_monitor,
            [](const auto &result) { return result.snapshot.state != SRTS_CONNECTED; }, last,
            seen, std::chrono::milliseconds(5'000)) ||
        (seen & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_DISCONNECTED) == 0)
    {
        std::cerr << "FAIL: peer close not reported as a disconnect (state "
                  << last.snapshot.state << ")\n";
        ok = false;
    }
    std::cout << "CONNECTION {\"provider\":\"" << runtime_provider_name
              << "\",\"scenario\":\"peer-close\",\"stateAfterPeerClose\":" << last.snapshot.state
              << "}\n";
    return ok;
}

bool scenario_encrypted()
{
    Socket listener(srt_create_socket());
    Socket caller(srt_create_socket());
    Socket accepted;
    PairOptions options;
    options.caller_passphrase = "telemetry-passphrase";
    options.listener_passphrase = "telemetry-passphrase";
    if (!connect_pair(options, listener, caller, accepted))
    {
        return false;
    }
    bool ok = true;
    for (const auto socket : {caller.get(), accepted.get()})
    {
        Monitor monitor;
        if (!create_monitor(socket, monitor))
        {
            return false;
        }
        robotweax_srt_telemetry_public_srt_connection_result_v1 last{};
        std::uint64_t seen = 0;
        const auto secured = [](const auto &result)
        {
            const auto &s = result.snapshot;
            return s.km_state == SRT_KM_S_SECURED && s.send_km_state == SRT_KM_S_SECURED &&
                   s.receive_km_state == SRT_KM_S_SECURED;
        };
        if (!poll_until(monitor, secured, last, seen, std::chrono::milliseconds(3'000)) ||
            last.snapshot.key_length_bytes != 16)
        {
            std::cerr << "FAIL: encrypted connection KM " << last.snapshot.km_state << "/"
                      << last.snapshot.send_km_state << "/" << last.snapshot.receive_km_state
                      << ", key length " << last.snapshot.key_length_bytes << '\n';
            ok = false;
        }
    }
    return ok;
}

bool scenario_wrong_passphrase()
{
    Socket listener(srt_create_socket());
    Socket caller(srt_create_socket());
    Socket accepted;
    PairOptions options;
    options.caller_passphrase = "telemetry-passphrase";
    options.listener_passphrase = "another-passphrase";
    if (connect_pair(options, listener, caller, accepted, true))
    {
        std::cerr << "FAIL: mismatched passphrases connected\n";
        return false;
    }
    Monitor monitor;
    if (!create_monitor(caller.get(), monitor))
    {
        return false;
    }
    const auto result = poll_monitor(monitor);
    std::cout << "CONNECTION {\"provider\":\"" << runtime_provider_name
              << "\",\"scenario\":\"wrong-passphrase\",\"state\":" << result.snapshot.state
              << ",\"rejectReason\":" << result.snapshot.reject_reason << "}\n";
    if ((result.events & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_REJECTED) == 0 ||
        result.snapshot.reject_reason != SRT_REJ_BADSECRET)
    {
        std::cerr << "FAIL: wrong passphrase not reported as BADSECRET rejection ("
                  << result.snapshot.reject_reason << ")\n";
        return false;
    }
    return true;
}

bool scenario_packet_filter()
{
    Socket listener(srt_create_socket());
    Socket caller(srt_create_socket());
    Socket accepted;
    PairOptions options;
    options.packet_filter = "fec,cols:4,rows:2";
    if (!connect_pair(options, listener, caller, accepted))
    {
        return false;
    }
    Monitor monitor;
    if (!create_monitor(accepted.get(), monitor))
    {
        return false;
    }
    const auto result = poll_monitor(monitor);
    const std::string_view filter(result.snapshot.packet_filter);
    if (filter.substr(0, 4) != "fec," || filter.find("cols:4") == std::string_view::npos)
    {
        std::cerr << "FAIL: negotiated packet filter '" << filter << "'\n";
        return false;
    }
    return true;
}

int run_connection_events()
{
    Runtime runtime;
    if (!runtime.active())
    {
        std::cerr << "FAIL: srt_startup\n";
        return EXIT_FAILURE;
    }
    const auto plain = scenario_plain_and_disconnect();
    const auto encrypted = scenario_encrypted();
    const auto rejected = scenario_wrong_passphrase();
    const auto filter = scenario_packet_filter();
    std::cout << "CONNECTION_EVENTS {\"provider\":\"" << runtime_provider_name
              << "\",\"plainAndDisconnect\":" << plain << ",\"encrypted\":" << encrypted
              << ",\"wrongPassphrase\":" << rejected << ",\"packetFilter\":" << filter << "}\n";
    return plain && encrypted && rejected && filter ? EXIT_SUCCESS : EXIT_FAILURE;
}


struct SamplerObserved
{
    std::mutex mutex;
    std::map<std::uint64_t, std::vector<robotweax_srt_telemetry_window_summary_v1>> windows;
    std::map<std::uint64_t, int> samples;
    std::map<std::uint64_t, int> errors;
    std::map<std::uint64_t, std::string> role_label;
};

void sampler_on_sample(void *user, const robotweax_srt_telemetry_source_info_v1 *source,
                       const robotweax_srt_telemetry_source_output_v1 *,
                       const robotweax_srt_telemetry_metric_sample_v1 *)
{
    auto &observed = *static_cast<SamplerObserved *>(user);
    std::lock_guard lock(observed.mutex);
    observed.samples[source->source_id] += 1;
    if (source->label_count > 0)
    {
        observed.role_label[source->source_id] = source->labels[0].value;
    }
}

void sampler_on_window(void *user, const robotweax_srt_telemetry_source_info_v1 *source,
                       const robotweax_srt_telemetry_window_summary_v1 *window)
{
    auto &observed = *static_cast<SamplerObserved *>(user);
    std::lock_guard lock(observed.mutex);
    observed.windows[source->source_id].push_back(*window);
}

void sampler_on_error(void *user, const robotweax_srt_telemetry_source_info_v1 *source,
                      robotweax_srt_telemetry_error, std::int32_t)
{
    auto &observed = *static_cast<SamplerObserved *>(user);
    std::lock_guard lock(observed.mutex);
    observed.errors[source->source_id] += 1;
}

int run_sampler()
{
    Runtime runtime;
    if (!runtime.active())
    {
        std::cerr << "FAIL: srt_startup\n";
        return EXIT_FAILURE;
    }
    Socket listener(srt_create_socket());
    Socket caller(srt_create_socket());
    Socket accepted;
    if (!connect_pair(PairOptions{}, listener, caller, accepted))
    {
        return EXIT_FAILURE;
    }
    Session caller_session;
    Session accepted_session;
    if (!create_full_session(caller.get(), caller_session) ||
        !create_full_session(accepted.get(), accepted_session))
    {
        std::cerr << "FAIL: session creation\n";
        return EXIT_FAILURE;
    }

    SamplerObserved observed;
    robotweax_srt_telemetry_sampler_config_v1 config{};
    robotweax_srt_telemetry_sampler_config_default(&config);
    config.period_nano = UINT64_C(50'000'000);
    config.windows.expected_period_nano = config.period_nano;
    config.windows.window_count = 1;
    config.windows.window_nano[0] = UINT64_C(500'000'000);
    config.callbacks.user_data = &observed;
    config.callbacks.on_sample = &sampler_on_sample;
    config.callbacks.on_window = &sampler_on_window;
    config.callbacks.on_error = &sampler_on_error;
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    if (robotweax_srt_telemetry_sampler_create(&config, &sampler) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        std::cerr << "FAIL: sampler creation\n";
        return EXIT_FAILURE;
    }

    if (robotweax_srt_telemetry_sampler_enable_history(sampler, 600) != ROBOTWEAX_SRT_TELEMETRY_OK ||
        robotweax_srt_telemetry_sampler_enable_journal(sampler, 256) != ROBOTWEAX_SRT_TELEMETRY_OK ||
        robotweax_srt_telemetry_sampler_enable_health(sampler, 1) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        std::cerr << "FAIL: history/journal/health\n";
        robotweax_srt_telemetry_sampler_destroy(sampler);
        return EXIT_FAILURE;
    }
    const robotweax_srt_telemetry_label_v1 sender_labels[] = {{"endpoint", "sender"}};
    const robotweax_srt_telemetry_label_v1 receiver_labels[] = {{"endpoint", "receiver"}};
    robotweax_srt_telemetry_source_config_v1 sender_source{};
    robotweax_srt_telemetry_source_config_v1 receiver_source{};
    robotweax_srt_telemetry_public_srt_source_config(caller_session.value,
                                                     ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER,
                                                     sender_labels, 1, &sender_source);
    robotweax_srt_telemetry_public_srt_source_config(accepted_session.value,
                                                     ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER,
                                                     receiver_labels, 1, &receiver_source);
    std::uint64_t sender_id = 0;
    std::uint64_t receiver_id = 0;
    if (robotweax_srt_telemetry_sampler_add_source(sampler, &sender_source, &sender_id) !=
            ROBOTWEAX_SRT_TELEMETRY_OK ||
        robotweax_srt_telemetry_sampler_add_source(sampler, &receiver_source, &receiver_id) !=
            ROBOTWEAX_SRT_TELEMETRY_OK ||
        robotweax_srt_telemetry_sampler_start(sampler) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        std::cerr << "FAIL: sampler setup\n";
        robotweax_srt_telemetry_sampler_destroy(sampler);
        return EXIT_FAILURE;
    }

    /* About 300 packets/s for 3 s, received on a second thread (health
     * needs 2 s of evidence before it says OK). */
    constexpr int message_count = 900;
    std::atomic<int> received{0};
    std::thread reader(
        [&]
        {
            for (int index = 0; index < message_count; ++index)
            {
                if (!receive_payload(accepted.get(), static_cast<char>('a' + index % 26)))
                {
                    return;
                }
                received.fetch_add(1);
            }
        });
    bool sent_all = true;
    const auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < message_count; ++index)
    {
        std::this_thread::sleep_until(start + std::chrono::microseconds(3'333 * index));
        sent_all = send_payload(caller.get(), static_cast<char>('a' + index % 26)) && sent_all;
    }
    reader.join();

    /* Health of a clean loopback stream, read right after it. */
    robotweax_srt_telemetry_health_status_v1 sender_health{};
    robotweax_srt_telemetry_health_status_v1 receiver_health{};
    static_cast<void>(robotweax_srt_telemetry_sampler_health_read(sampler, sender_id, &sender_health));
    static_cast<void>(robotweax_srt_telemetry_sampler_health_read(sampler, receiver_id, &receiver_health));

    /* History: 500 ms records of the receiver, read while it is registered. */
    std::vector<robotweax_srt_telemetry_compact_record_v1> history(64);
    std::size_t history_count = 0;
    static_cast<void>(robotweax_srt_telemetry_sampler_history_read(
        sampler, receiver_id, 0, history.data(), history.size(), &history_count));
    double best_history_rate = 0.0;
    for (std::size_t index = 0; index < history_count; ++index)
    {
        if ((history[index].available & ROBOTWEAX_SRT_TELEMETRY_COMPACT_UNIQUE_RECEIVED_BPS) != 0)
        {
            best_history_rate = std::max(
                best_history_rate,
                static_cast<double>(history[index].unique_received_bits_per_second));
        }
    }

    /* Export: both formats render the live sources. */
    std::string prometheus;
    std::string snapshot;
    {
        robotweax_srt_telemetry_render_options_v1 options;
        const auto collect = [](void *user, const char *data, const std::size_t size)
        {
            static_cast<std::string *>(user)->append(data, size);
            return 0;
        };
        robotweax_srt_telemetry_render_options_default(ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS, &options);
        options.window_nano = UINT64_C(500'000'000);
        static_cast<void>(robotweax_srt_telemetry_sampler_render_to(sampler, &options, collect, &prometheus));
        robotweax_srt_telemetry_render_options_default(ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON, &options);
        options.window_nano = UINT64_C(500'000'000);
        static_cast<void>(robotweax_srt_telemetry_sampler_render_to(sampler, &options, collect, &snapshot));
    }
    const bool exported =
        prometheus.find("srt_receiver_recovery_headroom_ratio{source_id=") != std::string::npos &&
        prometheus.find("srt_sender_unique_sent_packets_total{source_id=") != std::string::npos &&
        prometheus.find("srt_health_verdict{") != std::string::npos &&
        snapshot.find("\"srt.receiver.unique_received.packet_rate\"") != std::string::npos;

    /* Connection events into the journal: watch the receiver while the
     * caller closes, as a host would. */
    Monitor monitor;
    bool disconnect_journaled = false;
    bool sender_removed = false;
    robotweax_srt_telemetry_health_context_v1 receiver_context;
    robotweax_srt_telemetry_health_context_init(&receiver_context);
    /* Haivision SRT fails statistics reads on a broken socket, so only
     * errors before the close count. */
    int errors_before_close = -1;
    if (create_monitor(accepted.get(), monitor))
    {
        const auto record = [&](const robotweax_srt_telemetry_public_srt_connection_result_v1 &result)
        {
            static_cast<void>(robotweax_srt_telemetry_public_srt_health_context(&result, &receiver_context));
            static_cast<void>(
                robotweax_srt_telemetry_sampler_set_health_context(sampler, receiver_id, &receiver_context));
            robotweax_srt_telemetry_event_v1 events[8]{};
            std::size_t count = 0;
            static_cast<void>(robotweax_srt_telemetry_public_srt_connection_events(
                &result, receiver_id, events, 8, &count));
            for (std::size_t index = 0; index < count; ++index)
            {
                static_cast<void>(robotweax_srt_telemetry_sampler_record_event(sampler, &events[index]));
            }
        };
        record(poll_monitor(monitor));
        {
            std::lock_guard lock(observed.mutex);
            errors_before_close = observed.errors[sender_id] + observed.errors[receiver_id];
        }
        /* Lifecycle rule: stop sampling a socket before closing it. */
        sender_removed = robotweax_srt_telemetry_sampler_remove_source(sampler, sender_id) ==
                         ROBOTWEAX_SRT_TELEMETRY_OK;
        static_cast<void>(caller.close());
        robotweax_srt_telemetry_public_srt_connection_result_v1 last{};
        std::uint64_t seen = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline &&
               (seen & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_DISCONNECTED) == 0)
        {
            last = poll_monitor(monitor);
            seen |= last.events;
            record(last);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    /* The lost connection reaches the verdict within a sampling period. */
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    robotweax_srt_telemetry_health_status_v1 lost_health{};
    static_cast<void>(robotweax_srt_telemetry_sampler_health_read(sampler, receiver_id, &lost_health));
    std::vector<robotweax_srt_telemetry_event_v1> journal_events(256);
    std::size_t journal_count = 0;
    static_cast<void>(robotweax_srt_telemetry_sampler_journal_read(
        sampler, 0, journal_events.data(), journal_events.size(), &journal_count, nullptr));
    int added = 0;
    bool failing_journaled = false;
    for (std::size_t index = 0; index < journal_count; ++index)
    {
        const auto &event = journal_events[index];
        added += event.kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_ADDED ? 1 : 0;
        failing_journaled = failing_journaled ||
                            (event.kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_VERDICT_CHANGED &&
                             event.source_id == receiver_id &&
                             event.current_value == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                             event.code == ROBOTWEAX_SRT_TELEMETRY_REASON_CONNECTION_LOST);
        disconnect_journaled = disconnect_journaled ||
                               (event.kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_DISCONNECTED &&
                                event.source_id == receiver_id);
    }

    /* Remove before destroying sessions or closing sockets. */
    const auto removed =
        sender_removed && robotweax_srt_telemetry_sampler_remove_source(sampler, receiver_id) ==
                              ROBOTWEAX_SRT_TELEMETRY_OK;
    robotweax_srt_telemetry_sampler_destroy(sampler);

    std::lock_guard lock(observed.mutex);
    const auto best = [&](const std::uint64_t id, const robotweax_srt_telemetry_metric metric)
    {
        double value = 0.0;
        bool found = false;
        for (const auto &window : observed.windows[id])
        {
            if ((window.available_metrics & (UINT64_C(1) << metric)) != 0 &&
                (window.quality_flags & ROBOTWEAX_SRT_TELEMETRY_WINDOW_PARTIAL) == 0)
            {
                value = std::max(value, window.metrics[metric].rate > 0.0
                                            ? window.metrics[metric].rate
                                            : window.metrics[metric].mean);
                found = true;
            }
        }
        return found ? value : -1.0;
    };
    const auto sent_pps = best(sender_id, ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_PACKETS_PER_SECOND);
    const auto received_pps =
        best(receiver_id, ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_RECEIVED_PACKETS_PER_SECOND);
    const auto headroom = best(receiver_id, ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO);
    const auto rtt = best(sender_id, ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS);
    std::cout << std::setprecision(6) << "SAMPLER {\"provider\":\"" << runtime_provider_name
              << "\",\"senderSamples\":" << observed.samples[sender_id]
              << ",\"receiverSamples\":" << observed.samples[receiver_id]
              << ",\"senderWindows\":" << observed.windows[sender_id].size()
              << ",\"sentPacketsPerSecond\":" << sent_pps
              << ",\"receivedPacketsPerSecond\":" << received_pps
              << ",\"bestRecoveryHeadroom\":" << headroom << ",\"rttSeconds\":" << rtt
              << ",\"errors\":" << errors_before_close
              << ",\"historyRecords\":" << history_count
              << ",\"bestHistoryReceivedBps\":" << best_history_rate
              << ",\"journalEvents\":" << journal_count
              << ",\"disconnectJournaled\":" << disconnect_journaled << ",\"exported\":" << exported
              << ",\"prometheusBytes\":" << prometheus.size()
              << ",\"senderVerdict\":\"" << robotweax_srt_telemetry_health_verdict_name(sender_health.verdict)
              << "\",\"receiverVerdict\":\""
              << robotweax_srt_telemetry_health_verdict_name(receiver_health.verdict) << "\",\"receiverReason\":\""
              << robotweax_srt_telemetry_health_reason_name(receiver_health.reason) << "\",\"afterClose\":\""
              << robotweax_srt_telemetry_health_verdict_name(lost_health.verdict) << '/'
              << robotweax_srt_telemetry_health_reason_name(lost_health.reason) << "\"}\n";

    const auto ok = sent_all && received.load() == message_count && removed &&
                    observed.samples[sender_id] >= 20 && observed.windows[sender_id].size() >= 3 &&
                    sent_pps > 200.0 && sent_pps < 400.0 && received_pps > 200.0 &&
                    received_pps < 400.0 && headroom > 0.5 && rtt >= 0.0 &&
                    observed.role_label[sender_id] == "sender" &&
                    history_count >= 2 && best_history_rate > 2e6 && added == 2 && exported &&
                    disconnect_journaled &&
                    receiver_health.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK &&
                    (sender_health.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK ||
                     sender_health.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN) &&
                    lost_health.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                    lost_health.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_CONNECTION_LOST && failing_journaled &&
                    errors_before_close == 0;
    if (!ok)
    {
        std::cerr << "FAIL: sampler over live SRT (received " << received.load() << ")\n";
    }
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
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
    if (argc == 2 && std::string_view(argv[1]) == "--sampler")
    {
        return run_sampler();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--connection-events")
    {
        return run_connection_events();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--field-coverage")
    {
        return run_field_coverage();
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
    std::cerr << "usage: runtime-test [--benchmark | --field-coverage | --connection-events | --sampler | "
                 "--lifecycle | "
                 "--close-race | --role "
                 "listener | --role caller --port PORT]\n";
    return EXIT_FAILURE;
}
