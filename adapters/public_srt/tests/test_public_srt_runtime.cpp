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
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{

constexpr auto runtime_provider =
#if defined(ROBOTWEAX_SRT_TELEMETRY_RUNTIME_PROVIDER_ROBOTWEAX)
    ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
#else
    ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
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

robotweax_srt_telemetry_public_srt_result_v1 sample(Session &session,
                                                    const std::uint64_t monotonic_nano)
{
    robotweax_srt_telemetry_public_srt_result_v1 result{};
    result.struct_size = sizeof(result);
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    if (robotweax_srt_telemetry_public_srt_session_sample(
            session.value, 1'800'000'000'000'000'000ULL + monotonic_nano, monotonic_nano,
            &result) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        std::cerr << "FAIL: live adapter sample returned " << result.native_result << '\n';
        std::exit(EXIT_FAILURE);
    }
    return result;
}

bool send_and_receive(const SRTSOCKET sender, const SRTSOCKET receiver, const char fill)
{
    std::array<char, 1'316> outbound{};
    std::array<char, 1'316> inbound{};
    outbound.fill(fill);
    const auto sent =
        srt_sendmsg(sender, outbound.data(), static_cast<int>(outbound.size()), -1, 1);
    if (sent != static_cast<int>(outbound.size()))
    {
        return srt_ok(sent, "srt_sendmsg") && false;
    }
    const auto received = srt_recvmsg(receiver, inbound.data(), static_cast<int>(inbound.size()));
    if (received != static_cast<int>(outbound.size()))
    {
        return srt_ok(received, "srt_recvmsg") && false;
    }
    return inbound == outbound;
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

} // namespace

int main()
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

    const int timeout_milliseconds = 3'000;
    if (!srt_ok(srt_setsockflag(caller.get(), SRTO_CONNTIMEO, &timeout_milliseconds,
                                sizeof(timeout_milliseconds)),
                "caller connect timeout") ||
        !srt_ok(srt_setsockflag(caller.get(), SRTO_RCVTIMEO, &timeout_milliseconds,
                                sizeof(timeout_milliseconds)),
                "caller receive timeout"))
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
    if (accepted.get() == SRT_INVALID_SOCK ||
        !srt_ok(srt_setsockflag(accepted.get(), SRTO_RCVTIMEO, &timeout_milliseconds,
                                sizeof(timeout_milliseconds)),
                "accepted receive timeout"))
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

    static_cast<void>(sample(caller_session, 1'000'000'000ULL));
    static_cast<void>(sample(accepted_session, 1'000'000'000ULL));
    if (!send_and_receive(caller.get(), accepted.get(), 'A') ||
        !send_and_receive(accepted.get(), caller.get(), 'B'))
    {
        std::cerr << "FAIL: bidirectional payload exchange\n";
        return EXIT_FAILURE;
    }

    const auto caller_result = sample(caller_session, 2'000'000'000ULL);
    const auto accepted_result = sample(accepted_session, 2'000'000'000ULL);
    if (!has_bidirectional_deltas(caller_result) || !has_bidirectional_deltas(accepted_result))
    {
        std::cerr << "FAIL: live samples did not expose bidirectional packet deltas\n";
        return EXIT_FAILURE;
    }

    std::cout << "public SRT adapter live loopback passed\n";
    return EXIT_SUCCESS;
}
