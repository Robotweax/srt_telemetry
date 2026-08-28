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

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

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
        if (socket_ != SRT_INVALID_SOCK)
        {
            static_cast<void>(srt_close(socket_));
        }
    }

    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;

    [[nodiscard]] SRTSOCKET get() const noexcept
    {
        return socket_;
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

bool create_session(const SRTSOCKET socket, Session &session)
{
    robotweax_srt_telemetry_public_srt_config_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config.socket = socket;
    config.provider = runtime_provider;
    config.capabilities = ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1;
    config.qualified_fields = runtime_fields;
    config.bistats = &srt_bistats;
    return robotweax_srt_telemetry_public_srt_session_create(&config, &session.value) ==
           ROBOTWEAX_SRT_TELEMETRY_OK;
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

} // namespace

int main(const int argc, char **argv)
{
    if (argc == 1)
    {
        return run_self_loopback();
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
    std::cerr << "usage: runtime-test [--role listener | --role caller --port PORT]\n";
    return EXIT_FAILURE;
}
