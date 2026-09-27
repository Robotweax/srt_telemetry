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
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

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

struct Options final
{
    std::string role;
    std::string bind_address = "127.0.0.1";
    std::string remote_address = "127.0.0.1";
    std::size_t message_bytes = maximum_message_bytes;
    int port = 0;
    int message_ttl_milliseconds = -1;
    int receive_timeout_milliseconds = 15'000;
    std::optional<bool> too_late_packet_drop;
    bool verify_message_sequence = false;
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

void emit_sample(const std::string_view role, const std::uint64_t sample_index,
                 const robotweax_srt_telemetry_public_srt_result_v1 &result)
{
    const auto &raw = result.raw;
    const auto &delta = result.delta;
    std::cout << std::setprecision(17) << "INCIDENT_SAMPLE {\"provider\":\""
              << runtime_provider_name << "\",\"role\":\"" << role
              << "\",\"sampleIndex\":" << sample_index
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

bool export_recording(const robotweax::srt::telemetry::BoundedRecorder &recorder,
                      const RecordingConfig &config, const std::string_view role)
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
    std::cout << "RECORDED {\"role\":\"" << role << "\",\"recordCount\":" << recorder.size()
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

int run_commands(const SRTSOCKET socket, Session &session, const bool sender,
                 const std::string_view role, const Options &options)
{
    TransferWorker transfer_worker;
    const auto message_bytes = options.message_bytes;
    const auto &recording = options.recording;
    bool message_api = false;
    int message_size = sizeof(message_api);
    bool too_late_packet_drop = false;
    int drop_size = sizeof(too_late_packet_drop);
    int receive_timeout_milliseconds = 0;
    int timeout_size = sizeof(receive_timeout_milliseconds);
    if (!srt_ok(srt_getsockflag(socket, SRTO_MESSAGEAPI, &message_api, &message_size),
                "profile message API") ||
        !message_api ||
        !srt_ok(srt_getsockflag(socket, SRTO_TLPKTDROP, &too_late_packet_drop, &drop_size),
                "profile too-late packet drop") ||
        !srt_ok(
            srt_getsockflag(socket, SRTO_RCVTIMEO, &receive_timeout_milliseconds, &timeout_size),
            "profile receive timeout") ||
        (options.too_late_packet_drop.has_value() &&
         too_late_packet_drop != *options.too_late_packet_drop) ||
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
              << (options.verify_message_sequence ? "true" : "false") << "}\n"
              << std::flush;
    std::optional<robotweax::srt::telemetry::BoundedRecorder> recorder;
    if (recording.has_value())
    {
        recorder.emplace(recording->capacity);
    }
    std::optional<TriggerWindow> window;
    std::string line;
    while (std::getline(std::cin, line))
    {
        std::istringstream command(line);
        std::string operation;
        command >> operation;
        if (operation == "DONE" && no_extra_input(command) && !transfer_worker.active())
        {
            if (window.has_value())
            {
                window->finish(window->trigger_count() == 0 ? "untriggered-end" : "connection-ended");
                emit_window(*window);
            }
            if (recording.has_value() &&
                !export_recording(window.has_value() ? window->recording() : *recorder,
                                  *recording, role))
            {
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }
        if (operation == "ARM")
        {
            std::size_t pre_records = 0;
            std::uint64_t post_ms = 0;
            if (!recording.has_value() || window.has_value() ||
                !(command >> pre_records >> post_ms) || !no_extra_input(command))
                throw std::invalid_argument("invalid ARM command (requires recording; once per epoch)");
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
            robotweax_srt_telemetry_public_srt_result_v1 result{};
            if (!sample_at(session, unix_nano, monotonic_nano, result))
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
                message_count == 0 || !no_extra_input(command) ||
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
    int port = 0;
    if (!bind_ephemeral_udp(listener.get(), options.bind_address, port) ||
        !srt_ok(srt_listen(listener.get(), 1), "incident listen"))
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
    Session session;
    if (!create_session(accepted.get(), session))
    {
        std::cerr << "FAIL: listener telemetry session\n";
        return EXIT_FAILURE;
    }
    std::cout << "CONNECTED\n" << std::flush;
    return run_commands(accepted.get(), session, false, "receiver", options);
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
    return run_commands(caller.get(), session, true, "sender", options);
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
    if (saw_recording_option &&
        (recording->output.empty() || recording->flow_id.empty() ||
         recording->provider_version.empty() || recording->provider_revision.empty() ||
         recording->build_profile.empty()))
    {
        return false;
    }
    if (options.verify_message_sequence && options.message_bytes < 16)
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
                         "[--message-bytes 1..1316] "
                         "[--too-late-packet-drop enabled|disabled] "
                         "[--message-ttl-ms -1..60000] [--receive-timeout-ms 1..60000] "
                         "[--verify-message-sequence (requires >=16 message bytes)] "
                         "[--jsonl-output PATH --flow-id ID --provider-version VERSION "
                         "--provider-revision REVISION --build-profile PROFILE "
                         "[--recorder-capacity RECORDS]]\n";
            return EXIT_FAILURE;
        }
        return options.role == "listener" ? run_listener(options) : run_caller(options);
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: incident peer exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
