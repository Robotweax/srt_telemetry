#include <robotweax/srt_telemetry.hpp>
#include "trigger_window.hpp"
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
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
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

constexpr std::uint64_t incident_fields =
    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_V1_5_5_CANDIDATE_FIELDS;
constexpr std::size_t maximum_message_bytes = 1'316;

struct RecordingConfig final
{
    std::filesystem::path output;
    std::string flow_id;
    std::string provider_version;
    std::string provider_revision;
    std::string build_profile;
    std::size_t capacity = 4'096;
};

// A plain UDP endpoint for the opaque media relay: the caller's local input
// (one datagram = one SRT message) or the listener's output destination.
struct MediaEndpoint final
{
    sockaddr_storage address{};
    int address_size = 0;
    std::string text;
};

// One caller-side member of a connection group: its local and remote IP
// address (the port is --port for every member) and its Backup weight.
struct MemberSpec final
{
    std::string local_address;
    std::string remote_address;
    int weight = 0;
};

constexpr std::size_t maximum_group_members = 8;

struct Options final
{
    std::string role;
    std::string group; // empty, "broadcast" or "backup"
    std::vector<MemberSpec> members; // caller in group mode
    std::size_t group_members = 0;   // listener in group mode: members to wait for
    std::optional<int> group_min_stable_timeout_milliseconds;
    std::string bind_address = "127.0.0.1";
    std::string remote_address = "127.0.0.1";
    std::size_t message_bytes = maximum_message_bytes;
    int port = 0;
    int message_ttl_milliseconds = -1;
    int receive_timeout_milliseconds = 15'000;
    std::optional<int> peer_idle_timeout_milliseconds;
    std::optional<bool> too_late_packet_drop;
    bool verify_message_sequence = false;
    std::optional<MediaEndpoint> media;
    std::optional<RecordingConfig> recording;
};

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
    explicit Socket(const SRTSOCKET socket = SRT_INVALID_SOCK) : socket_(socket) {}

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
    SRTSOCKET socket_;
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

bool configure_io_timeouts(const SRTSOCKET socket, const int timeout_milliseconds)
{
    return srt_ok(srt_setsockflag(socket, SRTO_RCVTIMEO, &timeout_milliseconds,
                                  sizeof(timeout_milliseconds)),
                  "receive timeout") &&
           srt_ok(srt_setsockflag(socket, SRTO_SNDTIMEO, &timeout_milliseconds,
                                  sizeof(timeout_milliseconds)),
                  "send timeout");
}

bool configure_live_message_profile(const SRTSOCKET socket, const Options &options)
{
    const SRT_TRANSTYPE transport = SRTT_LIVE;
    const bool message_api = true;
    if (!srt_ok(srt_setsockflag(socket, SRTO_TRANSTYPE, &transport, sizeof(transport)),
                "live transport profile") ||
        !srt_ok(srt_setsockflag(socket, SRTO_MESSAGEAPI, &message_api, sizeof(message_api)),
                "message API profile"))
        return false;
    if (options.too_late_packet_drop.has_value())
    {
        const bool enabled = *options.too_late_packet_drop;
        if (!srt_ok(srt_setsockflag(socket, SRTO_TLPKTDROP, &enabled, sizeof(enabled)),
                    "too-late packet drop"))
            return false;
    }
    if (options.peer_idle_timeout_milliseconds.has_value())
    {
        const int idle = *options.peer_idle_timeout_milliseconds;
        if (!srt_ok(srt_setsockflag(socket, SRTO_PEERIDLETIMEO, &idle, sizeof(idle)),
                    "peer idle timeout"))
            return false;
    }
    return true;
}

bool configure_caller(const SRTSOCKET socket, const Options &options)
{
    const int timeout_milliseconds = 15'000;
    return configure_live_message_profile(socket, options) &&
           srt_ok(srt_setsockflag(socket, SRTO_CONNTIMEO, &timeout_milliseconds,
                                  sizeof(timeout_milliseconds)),
                  "connect timeout") &&
           configure_io_timeouts(socket, options.receive_timeout_milliseconds);
}

int parse_ip_address(const std::string &text, const std::uint16_t port, sockaddr_storage &address)
{
    address = {};
    auto *ipv4 = reinterpret_cast<sockaddr_in *>(&address);
    ipv4->sin_family = AF_INET;
    ipv4->sin_port = htons(port);
    if (inet_pton(AF_INET, text.c_str(), &ipv4->sin_addr) == 1)
    {
        return static_cast<int>(sizeof(*ipv4));
    }

    address = {};
    auto *ipv6 = reinterpret_cast<sockaddr_in6 *>(&address);
    ipv6->sin6_family = AF_INET6;
    ipv6->sin6_port = htons(port);
    if (inet_pton(AF_INET6, text.c_str(), &ipv6->sin6_addr) == 1)
    {
        return static_cast<int>(sizeof(*ipv6));
    }
    address = {};
    return 0;
}

int address_port(const sockaddr_storage &address)
{
    if (address.ss_family == AF_INET)
    {
        return ntohs(reinterpret_cast<const sockaddr_in *>(&address)->sin_port);
    }
    if (address.ss_family == AF_INET6)
    {
        return ntohs(reinterpret_cast<const sockaddr_in6 *>(&address)->sin6_port);
    }
    return 0;
}

bool parse_media_endpoint(const std::string &text, MediaEndpoint &endpoint)
{
    std::string host;
    std::string port_text;
    if (!text.empty() && text.front() == '[')
    {
        const auto close = text.find(']');
        if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':')
            return false;
        host = text.substr(1, close - 1);
        port_text = text.substr(close + 2);
    }
    else
    {
        const auto colon = text.rfind(':');
        if (colon == std::string::npos || text.find(':') != colon)
            return false;
        host = text.substr(0, colon);
        port_text = text.substr(colon + 1);
    }
    char *end = nullptr;
    const auto port = std::strtol(port_text.c_str(), &end, 10);
    if (port_text.empty() || *end != '\0' || port <= 0 || port > 65'535)
        return false;
    endpoint.address_size =
        parse_ip_address(host, static_cast<std::uint16_t>(port), endpoint.address);
    endpoint.text = text;
    return endpoint.address_size != 0;
}

bool bind_ephemeral_udp(const SRTSOCKET listener, const std::string &bind_address, int &port)
{
    sockaddr_storage address{};
    const auto address_size = parse_ip_address(bind_address, 0, address);
    if (address_size == 0)
    {
        std::cerr << "FAIL: invalid listener IP bind address\n";
        return false;
    }
    const auto native_socket = ::socket(address.ss_family, SOCK_DGRAM, 0);
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
    if (::bind(native_socket, reinterpret_cast<const sockaddr *>(&address), address_size) != 0)
    {
        std::cerr << "FAIL: native UDP bind\n";
        close_native();
        return false;
    }
#if defined(_WIN32)
    int acquired_address_size = sizeof(address);
#else
    socklen_t acquired_address_size = sizeof(address);
#endif
    if (::getsockname(native_socket, reinterpret_cast<sockaddr *>(&address),
                      &acquired_address_size) != 0 ||
        srt_bind_acquire(listener, native_socket) == SRT_ERROR)
    {
        std::cerr << "FAIL: acquire ephemeral UDP socket\n";
        close_native();
        return false;
    }
    port = address_port(address);
    if (port == 0)
    {
        std::cerr << "FAIL: acquired listener port is invalid\n";
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
    config.qualified_fields = incident_fields;
    config.bistats = &srt_bistats;
    return robotweax_srt_telemetry_public_srt_session_create(&config, &session.value) ==
           ROBOTWEAX_SRT_TELEMETRY_OK;
}

bool sample_at(Session &session, const std::uint64_t unix_nano, const std::uint64_t monotonic_nano,
               robotweax_srt_telemetry_public_srt_result_v1 &result)
{
    result = {};
    result.struct_size = sizeof(result);
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    const auto error = robotweax_srt_telemetry_public_srt_session_sample(session.value, unix_nano,
                                                                         monotonic_nano, &result);
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        std::cerr << "FAIL: telemetry sample error " << error << ", native " << result.native_result
                  << '\n';
        return false;
    }
    return true;
}

// A connection group as the peer runs it: one statistics session and, when
// recording, one bounded recorder per member socket. Members are fixed at
// connection time; a member that breaks is reported, never replaced.
struct GroupMember final
{
    std::size_t index = 0;
    SRTSOCKET socket = SRT_INVALID_SOCK;
    int weight = 0;
    std::string local_address;
    int local_port = 0;
    std::string peer_address;
    int peer_port = 0;
    Session session;
    std::optional<robotweax::srt::telemetry::BoundedRecorder> recorder;
    bool statistics_available = true;
};

struct Group final
{
    std::string type;
    SRTSOCKET handle = SRT_INVALID_SOCK;
    std::vector<std::unique_ptr<GroupMember>> members;
};

std::string address_text(const sockaddr_storage &address)
{
    std::array<char, INET6_ADDRSTRLEN> text{};
    if (address.ss_family == AF_INET)
    {
        const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(&address);
        if (inet_ntop(AF_INET, &ipv4->sin_addr, text.data(), text.size()) != nullptr)
            return text.data();
    }
    if (address.ss_family == AF_INET6)
    {
        const auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(&address);
        // An IPv4 peer of a dual-stack socket is printed as plain IPv4, so both
        // ends name the same address the same way.
        if (IN6_IS_ADDR_V4MAPPED(&ipv6->sin6_addr))
        {
            if (inet_ntop(AF_INET, ipv6->sin6_addr.s6_addr + 12, text.data(), text.size()) !=
                nullptr)
                return text.data();
        }
        else if (inet_ntop(AF_INET6, &ipv6->sin6_addr, text.data(), text.size()) != nullptr)
        {
            return text.data();
        }
    }
    return "";
}

// Orders members by address bytes (IPv4 before IPv6, IPv4-mapped as IPv4) and
// then port, both numerically.
std::pair<std::vector<std::uint8_t>, int> address_order_key(const sockaddr_storage &address)
{
    std::vector<std::uint8_t> bytes;
    if (address.ss_family == AF_INET)
    {
        const auto *raw = reinterpret_cast<const std::uint8_t *>(
            &reinterpret_cast<const sockaddr_in *>(&address)->sin_addr);
        bytes.assign(raw, raw + 4);
    }
    else if (address.ss_family == AF_INET6)
    {
        const auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(&address);
        if (IN6_IS_ADDR_V4MAPPED(&ipv6->sin6_addr))
            bytes.assign(ipv6->sin6_addr.s6_addr + 12, ipv6->sin6_addr.s6_addr + 16);
        else
            bytes.assign(ipv6->sin6_addr.s6_addr, ipv6->sin6_addr.s6_addr + 16);
    }
    return {bytes, address_port(address)};
}

std::string_view member_state_name(const SRT_MEMBERSTATUS state)
{
    switch (state)
    {
    case SRT_GST_PENDING:
        return "pending";
    case SRT_GST_IDLE:
        return "idle";
    case SRT_GST_RUNNING:
        return "running";
    case SRT_GST_BROKEN:
        return "broken";
    }
    return "unknown";
}

bool group_snapshot(const SRTSOCKET group, std::vector<SRT_SOCKGROUPDATA> &members)
{
    members.assign(maximum_group_members, SRT_SOCKGROUPDATA{});
    std::size_t count = members.size();
    const auto result = srt_group_data(group, members.data(), &count);
    if (result == SRT_ERROR)
    {
        members.clear();
        return false;
    }
    members.resize(static_cast<std::size_t>(result));
    return true;
}

// srt_connect_group and a group accept return after the first member. Wait
// until `expected` members are connected (running, or idle for a Backup
// standby), bounded by the connect timeout.
bool wait_for_group_members(const SRTSOCKET group, const std::size_t expected,
                            std::vector<SRT_SOCKGROUPDATA> &members)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (group_snapshot(group, members))
        {
            const auto connected = std::count_if(
                members.begin(), members.end(), [](const SRT_SOCKGROUPDATA &member)
                {
                    return member.sockstate == SRTS_CONNECTED &&
                           (member.memberstate == SRT_GST_RUNNING ||
                            member.memberstate == SRT_GST_IDLE);
                });
            if (members.size() == expected && static_cast<std::size_t>(connected) == expected)
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cerr << "FAIL: group members did not all connect\n";
    return false;
}

bool add_group_member(Group &group, const SRT_SOCKGROUPDATA &data)
{
    auto member = std::make_unique<GroupMember>();
    member->index = group.members.size();
    member->socket = data.id;
    member->weight = data.weight;
    sockaddr_storage address{};
    int address_size = sizeof(address);
    if (!srt_ok(srt_getsockname(data.id, reinterpret_cast<sockaddr *>(&address), &address_size),
                "group member local address"))
        return false;
    member->local_address = address_text(address);
    member->local_port = address_port(address);
    address = {};
    address_size = sizeof(address);
    if (!srt_ok(srt_getpeername(data.id, reinterpret_cast<sockaddr *>(&address), &address_size),
                "group member peer address"))
        return false;
    member->peer_address = address_text(address);
    member->peer_port = address_port(address);
    if (!create_session(data.id, member->session))
    {
        std::cerr << "FAIL: group member telemetry session\n";
        return false;
    }
    group.members.push_back(std::move(member));
    return true;
}

void emit_group(const Group &group)
{
    std::cout << "GROUP {\"type\":\"" << group.type << "\",\"members\":[";
    for (const auto &member : group.members)
    {
        std::cout << (member->index == 0 ? "" : ",") << "{\"index\":" << member->index
                  << ",\"socket\":" << member->socket << ",\"weight\":" << member->weight
                  << ",\"localAddress\":\"" << member->local_address
                  << "\",\"localPort\":" << member->local_port << ",\"peerAddress\":\""
                  << member->peer_address << "\",\"peerPort\":" << member->peer_port << "}";
    }
    std::cout << "]}\n" << std::flush;
}

void write_u64(std::array<char, maximum_message_bytes> &payload, const std::size_t offset,
               const std::uint64_t value)
{
    for (std::size_t byte = 0; byte < 8; ++byte)
        payload[offset + byte] = static_cast<char>((value >> (byte * 8)) & 0xff);
}

bool transfer_messages(const SRTSOCKET socket, const bool sender, const std::uint64_t transfer_id,
                       const std::uint64_t message_count, const std::size_t message_bytes,
                       const int ttl_milliseconds, const bool verify_sequence,
                       std::uint64_t &completed_messages,
                       const std::uint64_t pacing_microseconds = 0)
{
    std::array<char, maximum_message_bytes> payload{};
    payload.fill(static_cast<char>('A' + transfer_id % 26));
    completed_messages = 0;
    for (std::uint64_t message = 0; message < message_count; ++message)
    {
        if (verify_sequence)
        {
            write_u64(payload, 0, transfer_id);
            write_u64(payload, 8, message);
        }
        if (sender)
        {
            const auto sent = srt_sendmsg(socket, payload.data(), static_cast<int>(message_bytes),
                                          ttl_milliseconds, 1);
            if (sent != static_cast<int>(message_bytes))
            {
                return srt_ok(sent, "incident send") && false;
            }
            ++completed_messages;
            if (pacing_microseconds != 0 && message + 1 < message_count)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(pacing_microseconds));
            }
            continue;
        }
        std::array<char, maximum_message_bytes> received{};
        const auto size = srt_recvmsg(socket, received.data(), static_cast<int>(received.size()));
        if (size != static_cast<int>(message_bytes) ||
            !std::equal(payload.begin(), payload.begin() + message_bytes, received.begin()))
        {
            std::cerr << "FAIL: incident receive mismatch at message " << message << '\n';
            return srt_ok(size, "incident receive") && false;
        }
        ++completed_messages;
    }
    return true;
}

class TransferWorker final
{
  public:
    ~TransferWorker()
    {
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    TransferWorker(const TransferWorker &) = delete;
    TransferWorker &operator=(const TransferWorker &) = delete;
    TransferWorker() = default;

    [[nodiscard]] bool active() const noexcept
    {
        return thread_.joinable();
    }

    bool start(const SRTSOCKET socket, const bool sender, const std::uint64_t transfer_id,
               const std::uint64_t message_count, const std::size_t message_bytes,
               const int ttl_milliseconds, const bool verify_sequence,
               const std::uint64_t pacing_microseconds)
    {
        if (active())
        {
            return false;
        }
        transfer_id_ = transfer_id;
        message_count_ = message_count;
        success_ = false;
        completed_messages_ = 0;
        try
        {
            thread_ = std::thread(
                [this, socket, sender, transfer_id, message_count, message_bytes, ttl_milliseconds,
                 verify_sequence, pacing_microseconds]()
                {
                    success_ = transfer_messages(socket, sender, transfer_id, message_count,
                                                 message_bytes, ttl_milliseconds, verify_sequence,
                                                 completed_messages_, pacing_microseconds);
                });
        }
        catch (const std::system_error &error)
        {
            std::cerr << "FAIL: transfer worker startup: " << error.what() << '\n';
            return false;
        }
        return true;
    }

    bool finish(const std::uint64_t transfer_id, const std::uint64_t message_count)
    {
        if (!active() || transfer_id != transfer_id_ || message_count != message_count_)
        {
            return false;
        }
        thread_.join();
        return success_;
    }

    [[nodiscard]] std::uint64_t completed_messages() const noexcept
    {
        return completed_messages_;
    }

  private:
    std::thread thread_;
    std::uint64_t transfer_id_ = 0;
    std::uint64_t message_count_ = 0;
    std::uint64_t completed_messages_ = 0;
    bool success_ = false;
};

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket invalid_native_socket = INVALID_SOCKET;
void close_native_socket(const NativeSocket native)
{
    static_cast<void>(closesocket(native));
}
#else
using NativeSocket = int;
constexpr NativeSocket invalid_native_socket = -1;
void close_native_socket(const NativeSocket native)
{
    static_cast<void>(close(native));
}
#endif

std::uint64_t relay_monotonic_now()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

bool connection_ended(const int code)
{
    return code == SRT_ECONNLOST || code == SRT_ENOCONN || code == SRT_EINVSOCK ||
           code == SRT_ECONNFAIL;
}

// Counters of one media relay epoch. The relay carries opaque datagrams and
// never inspects or validates the media; gaps are recorded, not fatal.
struct MediaCounters final
{
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    std::uint64_t oversize_datagrams = 0;
    std::uint64_t send_failures = 0;
    std::uint64_t receive_timeouts = 0;
    std::uint64_t output_failures = 0;
    std::uint64_t first_message_nano = 0;
    std::uint64_t last_message_nano = 0;
    std::uint64_t longest_gap_nano = 0;
    bool connection_broken = false;
};

class MediaRelay final
{
  public:
    MediaRelay() = default;
    MediaRelay(const MediaRelay &) = delete;
    MediaRelay &operator=(const MediaRelay &) = delete;

    ~MediaRelay()
    {
        static_cast<void>(stop());
    }

    [[nodiscard]] bool active() const noexcept
    {
        return thread_.joinable();
    }

    // The native socket is created (and, for input, bound) before this
    // returns, so a source started after MEDIA_STARTED cannot race the bind.
    bool start(const SRTSOCKET socket, const bool sender, const MediaEndpoint &endpoint,
               const int ttl_milliseconds)
    {
        if (active())
            return false;
        native_ = ::socket(endpoint.address.ss_family, SOCK_DGRAM, 0);
        if (native_ == invalid_native_socket)
        {
            std::cerr << "FAIL: media relay UDP socket creation\n";
            return false;
        }
        if (sender)
        {
            const int receive_buffer = 4 * 1024 * 1024;
            static_cast<void>(::setsockopt(native_, SOL_SOCKET, SO_RCVBUF,
                                           reinterpret_cast<const char *>(&receive_buffer),
                                           sizeof(receive_buffer)));
            if (::bind(native_, reinterpret_cast<const sockaddr *>(&endpoint.address),
                       endpoint.address_size) != 0)
            {
                std::cerr << "FAIL: media relay UDP input bind " << endpoint.text << '\n';
                close_native_socket(native_);
                native_ = invalid_native_socket;
                return false;
            }
        }
        counters_ = {};
        stopping_.store(false);
        try
        {
            thread_ = std::thread(
                [this, socket, sender, endpoint, ttl_milliseconds]()
                {
                    if (sender)
                        relay_to_srt(socket, ttl_milliseconds);
                    else
                        relay_from_srt(socket, endpoint);
                });
        }
        catch (const std::system_error &error)
        {
            std::cerr << "FAIL: media relay startup: " << error.what() << '\n';
            close_native_socket(native_);
            native_ = invalid_native_socket;
            return false;
        }
        return true;
    }

    MediaCounters stop()
    {
        stopping_.store(true);
        if (thread_.joinable())
            thread_.join();
        if (native_ != invalid_native_socket)
        {
            close_native_socket(native_);
            native_ = invalid_native_socket;
        }
        return counters_;
    }

  private:
    void delivered(const std::size_t size)
    {
        const auto now = relay_monotonic_now();
        if (counters_.messages == 0)
            counters_.first_message_nano = now;
        else
            counters_.longest_gap_nano =
                std::max(counters_.longest_gap_nano, now - counters_.last_message_nano);
        counters_.last_message_nano = now;
        ++counters_.messages;
        counters_.bytes += size;
    }

    void relay_to_srt(const SRTSOCKET socket, const int ttl_milliseconds)
    {
        std::array<char, 2'048> datagram{};
        while (!stopping_.load())
        {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(native_, &readable);
            timeval wait{0, 100'000};
            const auto ready =
                ::select(static_cast<int>(native_) + 1, &readable, nullptr, nullptr, &wait);
            if (ready <= 0)
                continue;
            const auto size =
                ::recv(native_, datagram.data(), static_cast<int>(datagram.size()), 0);
            if (size <= 0)
                continue;
            if (static_cast<std::size_t>(size) > maximum_message_bytes)
            {
                ++counters_.oversize_datagrams;
                continue;
            }
            const auto sent =
                srt_sendmsg(socket, datagram.data(), static_cast<int>(size), ttl_milliseconds, 1);
            if (sent == static_cast<int>(size))
            {
                delivered(static_cast<std::size_t>(size));
                continue;
            }
            if (connection_ended(srt_getlasterror(nullptr)))
            {
                counters_.connection_broken = true;
                return;
            }
            ++counters_.send_failures;
        }
    }

    void relay_from_srt(const SRTSOCKET socket, const MediaEndpoint &endpoint)
    {
        std::array<char, maximum_message_bytes> message{};
        while (!stopping_.load())
        {
            const auto size = srt_recvmsg(socket, message.data(), static_cast<int>(message.size()));
            if (size == SRT_ERROR)
            {
                const auto code = srt_getlasterror(nullptr);
                if (connection_ended(code))
                {
                    counters_.connection_broken = true;
                    return;
                }
                ++counters_.receive_timeouts;
                continue;
            }
            if (size <= 0)
                continue;
            delivered(static_cast<std::size_t>(size));
            if (::sendto(native_, message.data(), size, 0,
                         reinterpret_cast<const sockaddr *>(&endpoint.address),
                         endpoint.address_size) != size)
                ++counters_.output_failures;
        }
    }

    std::atomic<bool> stopping_{false};
    std::thread thread_;
    NativeSocket native_ = invalid_native_socket;
    MediaCounters counters_;
};

void emit_media_stopped(const MediaCounters &counters)
{
    std::cout << "MEDIA_STOPPED {\"messages\":" << counters.messages
              << ",\"bytes\":" << counters.bytes
              << ",\"oversizeDatagrams\":" << counters.oversize_datagrams
              << ",\"sendFailures\":" << counters.send_failures
              << ",\"receiveTimeouts\":" << counters.receive_timeouts
              << ",\"outputFailures\":" << counters.output_failures
              << ",\"firstMessageMonotonicNano\":" << counters.first_message_nano
              << ",\"lastMessageMonotonicNano\":" << counters.last_message_nano
              << ",\"longestGapNano\":" << counters.longest_gap_nano
              << ",\"connectionBroken\":" << (counters.connection_broken ? "true" : "false")
              << "}\n"
              << std::flush;
}

void emit_sample(const std::string_view role, const std::uint64_t sample_index,
                 const robotweax_srt_telemetry_public_srt_result_v1 &result,
                 const std::optional<std::size_t> member = std::nullopt)
{
    std::string member_field;
    if (member.has_value())
        member_field = ",\"member\":" + std::to_string(*member);
    const auto &raw = result.raw;
    const auto &delta = result.delta;
    std::cout << std::setprecision(17) << "INCIDENT_SAMPLE {\"provider\":\""
              << runtime_provider_name << "\",\"role\":\"" << role << "\"" << member_field
              << ",\"sampleIndex\":" << sample_index
              << ",\"availableFields\":" << raw.available_fields
              << ",\"observedAtUnixNano\":" << raw.observed_at_unix_nano
              << ",\"sampleMonotonicNano\":" << raw.sample_monotonic_nano
              << ",\"qualityFlags\":" << delta.quality_flags
              << ",\"intervalNano\":" << delta.interval_nano
              << ",\"availableDeltaFields\":" << delta.available_delta_fields
              << ",\"availableRates\":" << delta.available_rates
              << ",\"raw\":{\"sent_packet_total\":" << raw.sent_packet_total
              << ",\"received_packet_total\":" << raw.received_packet_total
              << ",\"unique_sent_packet_total\":" << raw.unique_sent_packet_total
              << ",\"unique_received_packet_total\":" << raw.unique_received_packet_total
              << ",\"sender_loss_packet_total\":" << raw.sender_loss_packet_total
              << ",\"receiver_loss_packet_total\":" << raw.receiver_loss_packet_total
              << ",\"retransmitted_packet_total\":" << raw.retransmitted_packet_total
              << ",\"sender_drop_packet_total\":" << raw.sender_drop_packet_total
              << ",\"receiver_drop_packet_total\":" << raw.receiver_drop_packet_total
              << ",\"receiver_undecryptable_packet_total\":"
              << raw.receiver_undecryptable_packet_total
              << ",\"sent_wire_byte_total\":" << raw.sent_wire_byte_total
              << ",\"received_wire_byte_total\":" << raw.received_wire_byte_total
              << ",\"unique_sent_wire_byte_total\":" << raw.unique_sent_wire_byte_total
              << ",\"unique_received_wire_byte_total\":" << raw.unique_received_wire_byte_total
              << ",\"retransmitted_wire_byte_total\":" << raw.retransmitted_wire_byte_total
              << ",\"rtt_seconds\":" << raw.rtt_seconds
              << ",\"flight_size_packets\":" << raw.flight_size_packets
              << ",\"flow_window_packets\":" << raw.flow_window_packets
              << ",\"congestion_window_packets\":" << raw.congestion_window_packets
              << ",\"available_send_buffer_bytes\":" << raw.available_send_buffer_bytes
              << ",\"available_receive_buffer_bytes\":" << raw.available_receive_buffer_bytes
              << ",\"send_buffer_bytes\":" << raw.send_buffer_bytes
              << ",\"send_buffer_seconds\":" << raw.send_buffer_seconds
              << ",\"receive_buffer_bytes\":" << raw.receive_buffer_bytes
              << ",\"receive_buffer_seconds\":" << raw.receive_buffer_seconds
              << ",\"sender_tsbpd_delay_seconds\":" << raw.sender_tsbpd_delay_seconds
              << ",\"receiver_tsbpd_delay_seconds\":" << raw.receiver_tsbpd_delay_seconds
              << "},\"delta\":{\"sent_packet_delta\":" << delta.sent_packet_delta
              << ",\"received_packet_delta\":" << delta.received_packet_delta
              << ",\"unique_sent_packet_delta\":" << delta.unique_sent_packet_delta
              << ",\"unique_received_packet_delta\":" << delta.unique_received_packet_delta
              << ",\"sender_loss_packet_delta\":" << delta.sender_loss_packet_delta
              << ",\"receiver_loss_packet_delta\":" << delta.receiver_loss_packet_delta
              << ",\"retransmitted_packet_delta\":" << delta.retransmitted_packet_delta
              << ",\"sender_drop_packet_delta\":" << delta.sender_drop_packet_delta
              << ",\"receiver_drop_packet_delta\":" << delta.receiver_drop_packet_delta
              << ",\"receiver_undecryptable_packet_delta\":"
              << delta.receiver_undecryptable_packet_delta
              << ",\"sent_wire_byte_delta\":" << delta.sent_wire_byte_delta
              << ",\"received_wire_byte_delta\":" << delta.received_wire_byte_delta
              << ",\"unique_sent_wire_byte_delta\":" << delta.unique_sent_wire_byte_delta
              << ",\"unique_received_wire_byte_delta\":" << delta.unique_received_wire_byte_delta
              << ",\"retransmitted_wire_byte_delta\":" << delta.retransmitted_wire_byte_delta
              << ",\"sent_wire_bits_per_second\":" << delta.sent_wire_bits_per_second
              << ",\"received_wire_bits_per_second\":" << delta.received_wire_bits_per_second
              << ",\"unique_sent_wire_bits_per_second\":" << delta.unique_sent_wire_bits_per_second
              << ",\"unique_received_wire_bits_per_second\":"
              << delta.unique_received_wire_bits_per_second << "}}\n"
              << std::flush;
}

bool no_extra_input(std::istringstream &command)
{
    std::string extra;
    return !(command >> extra);
}

// In group mode each member's recording is its own file beside the configured
// output, named <stem>.member-<index><extension>, with the member in its flow
// and stream identifiers. The schema is the single-socket one.
RecordingConfig member_recording_config(const RecordingConfig &config, const std::size_t member)
{
    RecordingConfig result = config;
    const auto suffix = ".member-" + std::to_string(member);
    result.output = config.output.parent_path() /
                    (config.output.stem().string() + suffix + config.output.extension().string());
    result.flow_id = config.flow_id + "-m" + std::to_string(member);
    return result;
}

bool export_recording(const robotweax::srt::telemetry::BoundedRecorder &recorder,
                      const RecordingConfig &config, const std::string_view role,
                      const std::optional<std::size_t> member = std::nullopt)
{
    if (recorder.size() == 0)
    {
        std::cerr << "FAIL: refusing to export an empty incident recording\n";
        return false;
    }
    const auto partial = std::filesystem::path(config.output.string() + ".partial");
    if (std::filesystem::exists(config.output) || std::filesystem::exists(partial))
    {
        std::cerr << "FAIL: refusing to overwrite incident recording " << config.output << '\n';
        return false;
    }

    robotweax::srt::telemetry::JsonlExportMetadata metadata;
    metadata.stream_id = config.flow_id + "-" + std::string(role);
    metadata.flow_id = config.flow_id;
    metadata.provider_version = config.provider_version;
    metadata.provider_revision = config.provider_revision;
    metadata.build_profile = config.build_profile;
    metadata.role = role == "sender" ? ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER
                                     : ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;
    metadata.configured_provider = runtime_provider;
    const auto jsonl = recorder.export_jsonl(metadata);

    // C11 exclusive creation closes the exists/open race. Publication through
    // a same-directory hard link fails if the destination appears meanwhile.
    auto *output = std::fopen(partial.string().c_str(), "wbx");
    if (output == nullptr)
    {
        std::cerr << "FAIL: exclusively creating incident recording " << partial << '\n';
        return false;
    }
    const bool wrote = std::fwrite(jsonl.data(), 1, jsonl.size(), output) == jsonl.size();
    const bool closed = std::fclose(output) == 0;
    const auto remove_partial = [&]()
    {
        std::error_code cleanup_error;
        std::filesystem::remove(partial, cleanup_error);
        if (cleanup_error)
            std::cerr << "FAIL: removing owned partial " << cleanup_error.message() << '\n';
    };
    if (!wrote || !closed)
    {
        std::cerr << "FAIL: writing incident recording " << partial << '\n';
        remove_partial();
        return false;
    }
    std::error_code publish_error;
    std::filesystem::create_hard_link(partial, config.output, publish_error);
    if (publish_error)
    {
        std::cerr << "FAIL: publishing incident recording " << config.output << ": "
                  << publish_error.message() << '\n';
        remove_partial();
        return false;
    }
    remove_partial();
    std::cout << "RECORDED {\"role\":\"" << role << "\"";
    if (member.has_value())
        std::cout << ",\"member\":" << *member;
    std::cout << ",\"recordCount\":" << recorder.size()
              << ",\"capacity\":" << recorder.capacity()
              << ",\"overwrittenCount\":" << recorder.overwritten_count() << "}\n"
              << std::flush;
    return true;
}

std::uint64_t monotonic_now()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t unix_now()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void emit_window(const TriggerWindow &window)
{
    std::cout << "WINDOW_STATE {\"triggerCount\":" << window.trigger_count()
              << ",\"triggerMonotonicNano\":" << window.trigger_nano()
              << ",\"deadlineMonotonicNano\":" << window.deadline_nano()
              << ",\"preRecordCount\":" << window.pre_count()
              << ",\"preOverwrittenCount\":" << window.pre_overwrites()
              << ",\"recordCount\":" << window.recording().size()
              << ",\"allocatedRecords\":" << window.allocated_records()
              << ",\"allocatedRecordBytes\":"
              << window.allocated_records() * sizeof(robotweax_srt_telemetry_record_v1)
              << ",\"frozen\":" << (window.frozen() ? "true" : "false")
              << ",\"finishReason\":\"" << window.reason() << "\"}\n" << std::flush;
}

// One group sample: the group's own counters, each member's state, and then
// one INCIDENT_SAMPLE per member whose statistics could be read. A member whose
// statistics read fails (its socket is gone) is not sampled again.
bool sample_group(Group &group, const std::string_view role, const std::uint64_t sample_index,
                  const std::uint64_t unix_nano, const std::uint64_t monotonic_nano)
{
    std::vector<SRT_SOCKGROUPDATA> snapshot;
    const bool listed = group_snapshot(group.handle, snapshot);
    std::vector<std::optional<robotweax_srt_telemetry_public_srt_result_v1>> results(
        group.members.size());
    for (auto &member : group.members)
    {
        if (!member->statistics_available)
            continue;
        robotweax_srt_telemetry_public_srt_result_v1 result{};
        result.struct_size = sizeof(result);
        result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        if (robotweax_srt_telemetry_public_srt_session_sample(
                member->session.value, unix_nano, monotonic_nano, &result) !=
            ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            member->statistics_available = false;
            std::cerr << "NOTE: group member " << member->index
                      << " statistics unavailable (native " << result.native_result
                      << "); it is not sampled again\n";
            continue;
        }
        if (member->recorder.has_value())
            member->recorder->push(result.raw, result.delta);
        results[member->index] = result;
    }
    SRT_TRACEBSTATS counters{};
    const bool counted = srt_bistats(group.handle, &counters, 0, 1) != SRT_ERROR;
    std::cout << "GROUP_SAMPLE {\"role\":\"" << role << "\",\"type\":\"" << group.type
              << "\",\"sampleIndex\":" << sample_index << ",\"observedAtUnixNano\":" << unix_nano
              << ",\"sampleMonotonicNano\":" << monotonic_nano << ",\"membersListed\":"
              << (listed ? "true" : "false") << ",\"groupCounters\":";
    if (counted)
    {
        std::cout << "{\"uniqueSentPacketTotal\":" << counters.pktSentUniqueTotal
                  << ",\"uniqueReceivedPacketTotal\":" << counters.pktRecvUniqueTotal
                  << ",\"receiverDropPacketTotal\":" << counters.pktRcvDropTotal
                  << ",\"retransmittedPacketTotal\":" << counters.pktRetransTotal << "}";
    }
    else
    {
        std::cout << "null";
    }
    std::cout << ",\"members\":[";
    for (const auto &member : group.members)
    {
        // Without a member list the state is unknown, not removed.
        std::string_view state = listed ? "removed" : "unknown";
        for (const auto &data : snapshot)
        {
            if (data.id == member->socket)
                state = member_state_name(data.memberstate);
        }
        std::cout << (member->index == 0 ? "" : ",") << "{\"index\":" << member->index
                  << ",\"socket\":" << member->socket << ",\"state\":\"" << state
                  << "\",\"sampled\":" << (results[member->index].has_value() ? "true" : "false")
                  << "}";
    }
    std::cout << "]}\n" << std::flush;
    for (const auto &member : group.members)
    {
        if (results[member->index].has_value())
            emit_sample(role, sample_index, *results[member->index], member->index);
    }
    return true;
}

// `socket` carries the data: a connected socket, or the group handle. In group
// mode `session` is null and `group` holds one session per member.
int run_commands(const SRTSOCKET socket, Session *session, Group *group, const bool sender,
                 const std::string_view role, const Options &options)
{
    TransferWorker transfer_worker;
    MediaRelay media_relay;
    // A group handle answers for its own I/O timeouts; link options are read
    // from the first member.
    const SRTSOCKET link = group != nullptr ? group->members.front()->socket : socket;
    const auto message_bytes = options.message_bytes;
    const auto &recording = options.recording;
    bool message_api = false;
    int message_size = sizeof(message_api);
    bool too_late_packet_drop = false;
    int drop_size = sizeof(too_late_packet_drop);
    int receive_timeout_milliseconds = 0;
    int timeout_size = sizeof(receive_timeout_milliseconds);
    int peer_idle_timeout_milliseconds = 0;
    int idle_size = sizeof(peer_idle_timeout_milliseconds);
    if (!srt_ok(srt_getsockflag(link, SRTO_MESSAGEAPI, &message_api, &message_size),
                "profile message API") ||
        !message_api ||
        !srt_ok(srt_getsockflag(link, SRTO_TLPKTDROP, &too_late_packet_drop, &drop_size),
                "profile too-late packet drop") ||
        !srt_ok(
            srt_getsockflag(socket, SRTO_RCVTIMEO, &receive_timeout_milliseconds, &timeout_size),
            "profile receive timeout") ||
        !srt_ok(srt_getsockflag(link, SRTO_PEERIDLETIMEO, &peer_idle_timeout_milliseconds,
                                &idle_size),
                "profile peer idle timeout") ||
        (options.too_late_packet_drop.has_value() &&
         too_late_packet_drop != *options.too_late_packet_drop) ||
        (options.peer_idle_timeout_milliseconds.has_value() &&
         peer_idle_timeout_milliseconds != *options.peer_idle_timeout_milliseconds) ||
        receive_timeout_milliseconds != options.receive_timeout_milliseconds)
    {
        std::cerr << "FAIL: incident peer profile mismatch\n";
        return EXIT_FAILURE;
    }
    std::cout << "PROFILE {\"provider\":\"" << runtime_provider_name
              << "\",\"publicApiVersionHex\":" << srt_getversion()
              << ",\"messageApi\":true,\"transport\":\"live\",\"recordBytes\":"
              << sizeof(robotweax_srt_telemetry_record_v1)
              << ",\"tooLatePacketDrop\":" << (too_late_packet_drop ? "true" : "false")
              << ",\"receiveTimeoutMillis\":" << receive_timeout_milliseconds
              << ",\"configuredMessageTtlMillis\":" << options.message_ttl_milliseconds
              << ",\"sequenceVerification\":"
              << (options.verify_message_sequence ? "true" : "false")
              << ",\"peerIdleTimeoutMillis\":" << peer_idle_timeout_milliseconds
              << ",\"mediaRelay\":\""
              << (!options.media.has_value() ? "none"
                  : sender                   ? "udp-to-srt"
                                             : "srt-to-udp")
              << "\"";
    if (group != nullptr)
        std::cout << ",\"group\":\"" << group->type << "\",\"groupMembers\":"
                  << group->members.size();
    std::cout << "}\n" << std::flush;
    std::optional<robotweax::srt::telemetry::BoundedRecorder> recorder;
    if (recording.has_value() && group == nullptr)
    {
        recorder.emplace(recording->capacity);
    }
    if (recording.has_value() && group != nullptr)
    {
        for (auto &member : group->members)
            member->recorder.emplace(recording->capacity);
    }
    std::optional<TriggerWindow> window;
    std::string line;
    while (std::getline(std::cin, line))
    {
        std::istringstream command(line);
        std::string operation;
        command >> operation;
        if (operation == "DONE" && no_extra_input(command) && !transfer_worker.active() &&
            !media_relay.active())
        {
            if (window.has_value())
            {
                window->finish(window->trigger_count() == 0 ? "untriggered-end" : "connection-ended");
                emit_window(*window);
            }
            if (recording.has_value() && group != nullptr)
            {
                for (const auto &member : group->members)
                {
                    // A member that broke before its first sample has nothing
                    // to write; say so and keep the other members' files.
                    if (member->recorder->size() == 0)
                    {
                        std::cout << "RECORDED {\"role\":\"" << role << "\",\"member\":"
                                  << member->index << ",\"recordCount\":0,\"capacity\":"
                                  << member->recorder->capacity()
                                  << ",\"overwrittenCount\":0}\n"
                                  << std::flush;
                        continue;
                    }
                    if (!export_recording(*member->recorder,
                                          member_recording_config(*recording, member->index), role,
                                          member->index))
                        return EXIT_FAILURE;
                }
                return EXIT_SUCCESS;
            }
            if (recording.has_value() &&
                !export_recording(window.has_value() ? window->recording() : *recorder,
                                  *recording, role))
            {
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }
        if (operation == "START_MEDIA")
        {
            if (!no_extra_input(command) || !options.media.has_value() || media_relay.active() ||
                transfer_worker.active() ||
                !media_relay.start(socket, sender, *options.media,
                                   options.message_ttl_milliseconds))
            {
                std::cerr << "FAIL: invalid or failed START_MEDIA command\n";
                return EXIT_FAILURE;
            }
            std::cout << "MEDIA_STARTED {\"direction\":\"" << (sender ? "udp-to-srt" : "srt-to-udp")
                      << "\"}\n"
                      << std::flush;
            continue;
        }
        if (operation == "STOP_MEDIA")
        {
            if (!no_extra_input(command) || !media_relay.active())
            {
                std::cerr << "FAIL: invalid STOP_MEDIA command\n";
                return EXIT_FAILURE;
            }
            emit_media_stopped(media_relay.stop());
            continue;
        }
        if (operation == "ARM")
        {
            std::size_t pre_records = 0;
            std::uint64_t post_ms = 0;
            if (!recording.has_value() || window.has_value() || group != nullptr ||
                !(command >> pre_records >> post_ms) || !no_extra_input(command))
                throw std::invalid_argument(
                    "invalid ARM command (requires recording; once per epoch; not in group mode)");
            recorder.reset();
            window.emplace(pre_records, recording->capacity, post_ms);
            emit_window(*window);
            continue;
        }
        if (operation == "TRIGGER" || operation == "TRIGGER_NOW")
        {
            std::uint64_t trigger_nano = 0;
            if (!window.has_value() ||
                (operation == "TRIGGER" && !(command >> trigger_nano)) ||
                !no_extra_input(command))
                throw std::invalid_argument("invalid TRIGGER command");
            if (operation == "TRIGGER_NOW") trigger_nano = monotonic_now();
            window->trigger(trigger_nano);
            emit_window(*window);
            continue;
        }
        if (operation == "SAMPLE" || operation == "SAMPLE_NOW")
        {
            std::uint64_t sample_index = 0;
            std::uint64_t unix_nano = 0;
            std::uint64_t monotonic_nano = 0;
            if (!(command >> sample_index) ||
                (operation == "SAMPLE" && !(command >> unix_nano >> monotonic_nano)) ||
                !no_extra_input(command))
            {
                std::cerr << "FAIL: invalid SAMPLE command\n";
                return EXIT_FAILURE;
            }
            if (operation == "SAMPLE_NOW")
            {
                unix_nano = unix_now();
                monotonic_nano = monotonic_now();
            }
            if (group != nullptr)
            {
                if (!sample_group(*group, role, sample_index, unix_nano, monotonic_nano))
                    return EXIT_FAILURE;
                continue;
            }
            robotweax_srt_telemetry_public_srt_result_v1 result{};
            if (!sample_at(*session, unix_nano, monotonic_nano, result))
            {
                return EXIT_FAILURE;
            }
            if (window.has_value()) window->push(result.raw, result.delta);
            else if (recorder.has_value()) recorder->push(result.raw, result.delta);
            emit_sample(role, sample_index, result);
            continue;
        }
        if (operation == "TRANSFER")
        {
            std::uint64_t sample_index = 0;
            std::uint64_t message_count = 0;
            std::uint64_t completed_messages = 0;
            if (!(command >> sample_index >> message_count) || message_count == 0 ||
                !no_extra_input(command) ||
                !transfer_messages(socket, sender, sample_index, message_count, message_bytes,
                                   options.message_ttl_milliseconds,
                                   options.verify_message_sequence, completed_messages))
            {
                std::cerr << "FAIL: invalid or failed TRANSFER command\n";
                return EXIT_FAILURE;
            }
            std::cout << "TRANSFERRED {\"sampleIndex\":" << sample_index
                      << ",\"messages\":" << message_count;
            if (options.verify_message_sequence)
                std::cout << ",\"completedMessages\":" << completed_messages;
            std::cout << "}\n" << std::flush;
            continue;
        }
        if (operation == "START_TRANSFER")
        {
            std::uint64_t transfer_id = 0;
            std::uint64_t message_count = 0;
            std::uint64_t pacing_microseconds = 0;
            if (!(command >> transfer_id >> message_count >> pacing_microseconds) ||
                message_count == 0 || !no_extra_input(command) || media_relay.active() ||
                !transfer_worker.start(socket, sender, transfer_id, message_count, message_bytes,
                                       options.message_ttl_milliseconds,
                                       options.verify_message_sequence, pacing_microseconds))
            {
                std::cerr << "FAIL: invalid or failed START_TRANSFER command\n";
                return EXIT_FAILURE;
            }
            std::cout << "TRANSFER_STARTED {\"transferId\":" << transfer_id
                      << ",\"messages\":" << message_count << "}\n"
                      << std::flush;
            continue;
        }
        if (operation == "WAIT_TRANSFER")
        {
            std::uint64_t transfer_id = 0;
            std::uint64_t message_count = 0;
            if (!(command >> transfer_id >> message_count) || message_count == 0 ||
                !no_extra_input(command))
            {
                std::cerr << "FAIL: invalid or failed WAIT_TRANSFER command\n";
                return EXIT_FAILURE;
            }
            const bool completed = transfer_worker.finish(transfer_id, message_count);
            if (!completed)
            {
                std::cout << "TRANSFER_FAILED {\"transferId\":" << transfer_id
                          << ",\"messages\":" << message_count
                          << ",\"completedMessages\":" << transfer_worker.completed_messages()
                          << "}\n"
                          << std::flush;
                std::cerr << "FAIL: invalid or failed WAIT_TRANSFER command\n";
                return EXIT_FAILURE;
            }
            std::cout << "TRANSFERRED {\"transferId\":" << transfer_id
                      << ",\"messages\":" << message_count;
            if (options.verify_message_sequence)
                std::cout << ",\"completedMessages\":" << transfer_worker.completed_messages();
            std::cout << "}\n" << std::flush;
            continue;
        }
        std::cerr << "FAIL: unsupported command " << line << '\n';
        return EXIT_FAILURE;
    }
    std::cerr << "FAIL: coordinator input ended before DONE\n";
    return EXIT_FAILURE;
}

int run_listener(const Options &options)
{
    Runtime runtime;
    Socket listener(srt_create_socket());
    if (!runtime.active() || listener.get() == SRT_INVALID_SOCK ||
        !configure_live_message_profile(listener.get(), options) ||
        !configure_io_timeouts(listener.get(), options.receive_timeout_milliseconds))
    {
        std::cerr << "FAIL: listener startup\n";
        return EXIT_FAILURE;
    }
    const bool grouped = !options.group.empty();
    if (grouped)
    {
        const int accept_groups = 1;
        if (!srt_ok(srt_setsockflag(listener.get(), SRTO_GROUPCONNECT, &accept_groups,
                                    sizeof(accept_groups)),
                    "group admission"))
            return EXIT_FAILURE;
    }
    int port = 0;
    if (!bind_ephemeral_udp(listener.get(), options.bind_address, port) ||
        !srt_ok(srt_listen(listener.get(), grouped ? static_cast<int>(maximum_group_members) : 1),
                "incident listen"))
    {
        return EXIT_FAILURE;
    }
    std::cout << "READY {\"port\":" << port << "}\n" << std::flush;
    Socket accepted(srt_accept(listener.get(), nullptr, nullptr));
    if (accepted.get() == SRT_INVALID_SOCK ||
        !configure_io_timeouts(accepted.get(), options.receive_timeout_milliseconds))
    {
        std::cerr << "FAIL: incident accept\n";
        return EXIT_FAILURE;
    }
    if (grouped)
    {
        // The accepted handle is the mirror group. Members are ordered by the
        // caller's address and port, which the coordinator knows per path.
        std::vector<SRT_SOCKGROUPDATA> members;
        if (!wait_for_group_members(accepted.get(), options.group_members, members))
            return EXIT_FAILURE;
        std::sort(members.begin(), members.end(),
                  [](const SRT_SOCKGROUPDATA &left, const SRT_SOCKGROUPDATA &right)
                  {
                      const auto left_key = address_order_key(left.peeraddr);
                      const auto right_key = address_order_key(right.peeraddr);
                      if (left_key.first.size() != right_key.first.size())
                          return left_key.first.size() < right_key.first.size();
                      return left_key < right_key;
                  });
        Group group;
        group.type = options.group;
        group.handle = accepted.get();
        for (const auto &member : members)
        {
            if (!add_group_member(group, member))
                return EXIT_FAILURE;
        }
        emit_group(group);
        std::cout << "CONNECTED\n" << std::flush;
        return run_commands(accepted.get(), nullptr, &group, false, "receiver", options);
    }
    Session session;
    if (!create_session(accepted.get(), session))
    {
        std::cerr << "FAIL: listener telemetry session\n";
        return EXIT_FAILURE;
    }
    std::cout << "CONNECTED\n" << std::flush;
    return run_commands(accepted.get(), &session, nullptr, false, "receiver", options);
}

int parse_port(const char *text)
{
    char *end = nullptr;
    const auto value = std::strtol(text, &end, 10);
    return end != text && *end == '\0' && value > 0 && value <= 65'535 ? static_cast<int>(value)
                                                                       : 0;
}

int run_caller(const Options &options)
{
    Runtime runtime;
    Socket caller(srt_create_socket());
    if (!runtime.active() || caller.get() == SRT_INVALID_SOCK ||
        !configure_caller(caller.get(), options))
    {
        std::cerr << "FAIL: caller startup\n";
        return EXIT_FAILURE;
    }
    sockaddr_storage address{};
    const auto address_size =
        parse_ip_address(options.remote_address, static_cast<std::uint16_t>(options.port), address);
    if (address_size == 0)
    {
        std::cerr << "FAIL: invalid caller IP remote address\n";
        return EXIT_FAILURE;
    }
    if (!srt_ok(
            srt_connect(caller.get(), reinterpret_cast<const sockaddr *>(&address), address_size),
            "incident connect"))
    {
        return EXIT_FAILURE;
    }
    Session session;
    if (!create_session(caller.get(), session))
    {
        std::cerr << "FAIL: caller telemetry session\n";
        return EXIT_FAILURE;
    }
    std::cout << "CONNECTED\n" << std::flush;
    return run_commands(caller.get(), &session, nullptr, true, "sender", options);
}

int run_group_caller(const Options &options)
{
    Runtime runtime;
    const bool backup = options.group == "backup";
    Socket handle(srt_create_group(backup ? SRT_GTYPE_BACKUP : SRT_GTYPE_BROADCAST));
    if (!runtime.active() || handle.get() == SRT_INVALID_SOCK)
    {
        std::cerr << "FAIL: this SRT library created no connection group\n";
        return EXIT_FAILURE;
    }
    // Group options are the defaults of every member.
    if (!configure_caller(handle.get(), options))
    {
        std::cerr << "FAIL: group caller startup\n";
        return EXIT_FAILURE;
    }
    if (options.group_min_stable_timeout_milliseconds.has_value())
    {
        const int stable = *options.group_min_stable_timeout_milliseconds;
        if (!srt_ok(srt_setsockflag(handle.get(), SRTO_GROUPMINSTABLETIMEO, &stable,
                                    sizeof(stable)),
                    "group minimum stability timeout"))
            return EXIT_FAILURE;
    }
    std::vector<SRT_SOCKGROUPCONFIG> endpoints;
    for (const auto &spec : options.members)
    {
        sockaddr_storage local{};
        sockaddr_storage remote{};
        const auto local_size = parse_ip_address(spec.local_address, 0, local);
        const auto remote_size = parse_ip_address(
            spec.remote_address, static_cast<std::uint16_t>(options.port), remote);
        if (local_size == 0 || remote_size == 0 || local_size != remote_size)
        {
            std::cerr << "FAIL: invalid group member addresses\n";
            return EXIT_FAILURE;
        }
        auto endpoint = srt_prepare_endpoint(reinterpret_cast<const sockaddr *>(&local),
                                             reinterpret_cast<const sockaddr *>(&remote),
                                             remote_size);
        if (endpoint.errorcode != SRT_SUCCESS)
        {
            std::cerr << "FAIL: group endpoint preparation\n";
            return EXIT_FAILURE;
        }
        endpoint.weight = static_cast<std::uint16_t>(spec.weight);
        endpoints.push_back(endpoint);
    }
    if (!srt_ok(srt_connect_group(handle.get(), endpoints.data(),
                                  static_cast<int>(endpoints.size())),
                "incident group connect"))
        return EXIT_FAILURE;
    std::vector<SRT_SOCKGROUPDATA> members;
    if (!wait_for_group_members(handle.get(), endpoints.size(), members))
        return EXIT_FAILURE;
    Group group;
    group.type = options.group;
    group.handle = handle.get();
    // Members keep the order of the --member options.
    for (const auto &endpoint : endpoints)
    {
        const auto found = std::find_if(members.begin(), members.end(),
                                        [&](const SRT_SOCKGROUPDATA &member)
                                        {
                                            return member.id == endpoint.id;
                                        });
        if (found == members.end() || !add_group_member(group, *found))
        {
            std::cerr << "FAIL: group member is not connected\n";
            return EXIT_FAILURE;
        }
    }
    emit_group(group);
    std::cout << "CONNECTED\n" << std::flush;
    return run_commands(handle.get(), nullptr, &group, true, "sender", options);
}

bool parse_size(const char *text, std::size_t &result)
{
    char *end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value == 0 ||
        value > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
    {
        return false;
    }
    result = static_cast<std::size_t>(value);
    return true;
}

bool parse_integer(const char *text, const long minimum, const long maximum, int &result)
{
    char *end = nullptr;
    const auto value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < minimum || value > maximum)
        return false;
    result = static_cast<int>(value);
    return true;
}

bool parse_options(const int argc, char **argv, Options &options)
{
    std::optional<RecordingConfig> recording;
    bool saw_recording_option = false;
    std::string media_option;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view option(argv[index]);
        if (option == "--verify-message-sequence")
        {
            options.verify_message_sequence = true;
            continue;
        }
        if (index + 1 >= argc)
        {
            return false;
        }
        const char *value = argv[++index];
        if (option == "--role")
        {
            options.role = value;
        }
        else if (option == "--port")
        {
            options.port = parse_port(value);
        }
        else if (option == "--bind-address")
        {
            options.bind_address = value;
        }
        else if (option == "--remote-address")
        {
            options.remote_address = value;
        }
        else if (option == "--message-bytes")
        {
            if (!parse_size(value, options.message_bytes) ||
                options.message_bytes > maximum_message_bytes)
            {
                return false;
            }
        }
        else if (option == "--message-ttl-ms")
        {
            if (!parse_integer(value, -1, 60'000, options.message_ttl_milliseconds))
                return false;
        }
        else if (option == "--receive-timeout-ms")
        {
            if (!parse_integer(value, 1, 60'000, options.receive_timeout_milliseconds))
                return false;
        }
        else if (option == "--peer-idle-timeout-ms")
        {
            int idle = 0;
            if (!parse_integer(value, 1'000, 300'000, idle))
                return false;
            options.peer_idle_timeout_milliseconds = idle;
        }
        else if (option == "--media-udp-input" || option == "--media-udp-output")
        {
            MediaEndpoint endpoint;
            if (options.media.has_value() || !parse_media_endpoint(value, endpoint))
                return false;
            media_option = std::string(option);
            options.media = endpoint;
        }
        else if (option == "--group")
        {
            if (std::string_view(value) != "broadcast" && std::string_view(value) != "backup")
                return false;
            options.group = value;
        }
        else if (option == "--member")
        {
            // LOCAL_IP,REMOTE_IP[,WEIGHT]; IP addresses contain no comma.
            const std::string text(value);
            const auto first = text.find(',');
            if (first == std::string::npos)
                return false;
            const auto second = text.find(',', first + 1);
            MemberSpec spec;
            spec.local_address = text.substr(0, first);
            spec.remote_address = text.substr(
                first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
            sockaddr_storage probe{};
            if (parse_ip_address(spec.local_address, 0, probe) == 0 ||
                parse_ip_address(spec.remote_address, 0, probe) == 0 ||
                (second != std::string::npos &&
                 !parse_integer(text.substr(second + 1).c_str(), 0, 32'767, spec.weight)))
                return false;
            options.members.push_back(spec);
        }
        else if (option == "--group-members")
        {
            if (!parse_size(value, options.group_members))
                return false;
        }
        else if (option == "--group-min-stable-timeout-ms")
        {
            int stable = 0;
            if (!parse_integer(value, 60, 5'000, stable))
                return false;
            options.group_min_stable_timeout_milliseconds = stable;
        }
        else if (option == "--too-late-packet-drop")
        {
            if (std::string_view(value) != "enabled" && std::string_view(value) != "disabled")
                return false;
            options.too_late_packet_drop = std::string_view(value) == "enabled";
        }
        else
        {
            saw_recording_option = true;
            if (!recording.has_value())
            {
                recording.emplace();
            }
            if (option == "--jsonl-output")
            {
                recording->output = value;
            }
            else if (option == "--flow-id")
            {
                recording->flow_id = value;
            }
            else if (option == "--provider-version")
            {
                recording->provider_version = value;
            }
            else if (option == "--provider-revision")
            {
                recording->provider_revision = value;
            }
            else if (option == "--build-profile")
            {
                recording->build_profile = value;
            }
            else if (option == "--recorder-capacity")
            {
                if (!parse_size(value, recording->capacity))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
        }
    }
    if ((options.role != "listener" && options.role != "caller") ||
        (options.role == "listener" && options.port != 0) ||
        (options.role == "caller" && options.port == 0) ||
        (options.role == "listener" && options.remote_address != "127.0.0.1") ||
        (options.role == "caller" && options.bind_address != "127.0.0.1"))
    {
        return false;
    }
    // Group mode: the caller names its members, the listener how many to wait for.
    const bool grouped = !options.group.empty();
    const bool group_caller = grouped && options.role == "caller";
    const bool group_listener = grouped && options.role == "listener";
    if ((group_caller ? options.members.size() < 2 || options.members.size() > maximum_group_members
                      : !options.members.empty()) ||
        (group_listener ? options.group_members < 2 || options.group_members > maximum_group_members
                        : options.group_members != 0) ||
        (group_caller && options.remote_address != "127.0.0.1") ||
        (options.group_min_stable_timeout_milliseconds.has_value() &&
         !(group_caller && options.group == "backup")))
    {
        return false;
    }
    if (saw_recording_option &&
        (recording->output.empty() || recording->flow_id.empty() ||
         recording->provider_version.empty() || recording->provider_revision.empty() ||
         recording->build_profile.empty()))
    {
        return false;
    }
    if (options.verify_message_sequence && options.message_bytes < 16)
        return false;
    // The caller relays UDP input into SRT; the listener relays SRT to UDP.
    if ((media_option == "--media-udp-input" && options.role != "caller") ||
        (media_option == "--media-udp-output" && options.role != "listener"))
        return false;
    options.recording = std::move(recording);
    return true;
}

} // namespace

int main(const int argc, char **argv)
{
    try
    {
        Options options;
        if (!parse_options(argc, argv, options))
        {
            std::cerr << "usage: incident-peer --role listener|caller "
                         "[--bind-address IP | --remote-address IP --port PORT] "
                         "[--group broadcast|backup with --group-members 2..8 (listener) or "
                         "--port PORT and 2..8 times --member LOCAL_IP,REMOTE_IP[,WEIGHT] "
                         "(caller); --group-min-stable-timeout-ms 60..5000 (backup caller)] "
                         "[--message-bytes 1..1316] "
                         "[--too-late-packet-drop enabled|disabled] "
                         "[--message-ttl-ms -1..60000] [--receive-timeout-ms 1..60000] "
                         "[--peer-idle-timeout-ms 1000..300000] "
                         "[--media-udp-input HOST:PORT (caller) | "
                         "--media-udp-output HOST:PORT (listener)] "
                         "[--verify-message-sequence (requires >=16 message bytes)] "
                         "[--jsonl-output PATH --flow-id ID --provider-version VERSION "
                         "--provider-revision REVISION --build-profile PROFILE "
                         "[--recorder-capacity RECORDS]]\n";
            return EXIT_FAILURE;
        }
        if (options.role == "listener")
            return run_listener(options);
        return options.group.empty() ? run_caller(options) : run_group_caller(options);
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: incident peer exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
