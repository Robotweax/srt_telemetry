#include <robotweax/srt_telemetry.hpp>
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
    ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_PACKET_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_LOSS_PACKET_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_LOSS_PACKET_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_PACKET_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_DROP_PACKET_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_DROP_PACKET_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_UNDECRYPTABLE_PACKET_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_WIRE_BYTE_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_WIRE_BYTE_TOTAL |
    ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS;

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
    int port = 0;
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

bool configure_io_timeouts(const SRTSOCKET socket)
{
    const int timeout_milliseconds = 15'000;
    return srt_ok(srt_setsockflag(socket, SRTO_RCVTIMEO, &timeout_milliseconds,
                                  sizeof(timeout_milliseconds)),
                  "receive timeout") &&
           srt_ok(srt_setsockflag(socket, SRTO_SNDTIMEO, &timeout_milliseconds,
                                  sizeof(timeout_milliseconds)),
                  "send timeout");
}

bool configure_caller(const SRTSOCKET socket)
{
    const int timeout_milliseconds = 15'000;
    return srt_ok(srt_setsockflag(socket, SRTO_CONNTIMEO, &timeout_milliseconds,
                                  sizeof(timeout_milliseconds)),
                  "connect timeout") &&
           configure_io_timeouts(socket);
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
    if (::getsockname(native_socket, reinterpret_cast<sockaddr *>(&address), &address_size) != 0 ||
        srt_bind_acquire(listener, native_socket) == SRT_ERROR)
    {
        std::cerr << "FAIL: acquire ephemeral UDP socket\n";
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

bool transfer_messages(const SRTSOCKET socket, const bool sender, const std::uint64_t sample_index,
                       const std::uint64_t message_count,
                       const std::uint64_t pacing_microseconds = 0)
{
    std::array<char, 1'316> payload{};
    payload.fill(static_cast<char>('A' + sample_index % 26));
    for (std::uint64_t message = 0; message < message_count; ++message)
    {
        if (sender)
        {
            const auto sent =
                srt_sendmsg(socket, payload.data(), static_cast<int>(payload.size()), -1, 1);
            if (sent != static_cast<int>(payload.size()))
            {
                return srt_ok(sent, "incident send") && false;
            }
            if (pacing_microseconds != 0 && message + 1 < message_count)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(pacing_microseconds));
            }
            continue;
        }
        std::array<char, 1'316> received{};
        const auto size = srt_recvmsg(socket, received.data(), static_cast<int>(received.size()));
        if (size != static_cast<int>(received.size()) || received != payload)
        {
            return srt_ok(size, "incident receive") && false;
        }
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
               const std::uint64_t message_count, const std::uint64_t pacing_microseconds)
    {
        if (active())
        {
            return false;
        }
        transfer_id_ = transfer_id;
        message_count_ = message_count;
        success_ = false;
        try
        {
            thread_ = std::thread(
                [this, socket, sender, transfer_id, message_count, pacing_microseconds]()
                {
                    success_ = transfer_messages(socket, sender, transfer_id, message_count,
                                                 pacing_microseconds);
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

  private:
    std::thread thread_;
    std::uint64_t transfer_id_ = 0;
    std::uint64_t message_count_ = 0;
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

    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    output.write(jsonl.data(), static_cast<std::streamsize>(jsonl.size()));
    output.close();
    if (!output)
    {
        std::cerr << "FAIL: writing incident recording " << partial << '\n';
        return false;
    }
    std::error_code rename_error;
    std::filesystem::rename(partial, config.output, rename_error);
    if (rename_error)
    {
        std::cerr << "FAIL: publishing incident recording " << config.output << ": "
                  << rename_error.message() << '\n';
        return false;
    }
    std::cout << "RECORDED {\"role\":\"" << role << "\",\"recordCount\":" << recorder.size()
              << ",\"capacity\":" << recorder.capacity()
              << ",\"overwrittenCount\":" << recorder.overwritten_count() << "}\n"
              << std::flush;
    return true;
}

int run_commands(const SRTSOCKET socket, Session &session, const bool sender,
                 const std::string_view role, const std::optional<RecordingConfig> &recording)
{
    TransferWorker transfer_worker;
    std::optional<robotweax::srt::telemetry::BoundedRecorder> recorder;
    if (recording.has_value())
    {
        recorder.emplace(recording->capacity);
    }
    std::string line;
    while (std::getline(std::cin, line))
    {
        std::istringstream command(line);
        std::string operation;
        command >> operation;
        if (operation == "DONE" && no_extra_input(command) && !transfer_worker.active())
        {
            if (recording.has_value() && !export_recording(*recorder, *recording, role))
            {
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }
        if (operation == "SAMPLE")
        {
            std::uint64_t sample_index = 0;
            std::uint64_t unix_nano = 0;
            std::uint64_t monotonic_nano = 0;
            if (!(command >> sample_index >> unix_nano >> monotonic_nano) ||
                !no_extra_input(command))
            {
                std::cerr << "FAIL: invalid SAMPLE command\n";
                return EXIT_FAILURE;
            }
            robotweax_srt_telemetry_public_srt_result_v1 result{};
            if (!sample_at(session, unix_nano, monotonic_nano, result))
            {
                return EXIT_FAILURE;
            }
            if (recorder.has_value())
            {
                recorder->push(result.raw, result.delta);
            }
            emit_sample(role, sample_index, result);
            continue;
        }
        if (operation == "TRANSFER")
        {
            std::uint64_t sample_index = 0;
            std::uint64_t message_count = 0;
            if (!(command >> sample_index >> message_count) || message_count == 0 ||
                !no_extra_input(command) ||
                !transfer_messages(socket, sender, sample_index, message_count))
            {
                std::cerr << "FAIL: invalid or failed TRANSFER command\n";
                return EXIT_FAILURE;
            }
            std::cout << "TRANSFERRED {\"sampleIndex\":" << sample_index
                      << ",\"messages\":" << message_count << "}\n"
                      << std::flush;
            continue;
        }
        if (operation == "START_TRANSFER")
        {
            std::uint64_t transfer_id = 0;
            std::uint64_t message_count = 0;
            std::uint64_t pacing_microseconds = 0;
            if (!(command >> transfer_id >> message_count >> pacing_microseconds) ||
                message_count == 0 || !no_extra_input(command) ||
                !transfer_worker.start(socket, sender, transfer_id, message_count,
                                       pacing_microseconds))
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
                !no_extra_input(command) || !transfer_worker.finish(transfer_id, message_count))
            {
                std::cerr << "FAIL: invalid or failed WAIT_TRANSFER command\n";
                return EXIT_FAILURE;
            }
            std::cout << "TRANSFERRED {\"transferId\":" << transfer_id
                      << ",\"messages\":" << message_count << "}\n"
                      << std::flush;
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
        !configure_io_timeouts(listener.get()))
    {
        std::cerr << "FAIL: listener startup\n";
        return EXIT_FAILURE;
    }
    sockaddr_in address{};
    if (!bind_ephemeral_udp(listener.get(), address) ||
        !srt_ok(srt_listen(listener.get(), 1), "incident listen"))
    {
        return EXIT_FAILURE;
    }
    std::cout << "READY {\"port\":" << ntohs(address.sin_port) << "}\n" << std::flush;
    Socket accepted(srt_accept(listener.get(), nullptr, nullptr));
    if (accepted.get() == SRT_INVALID_SOCK || !configure_io_timeouts(accepted.get()))
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
    return run_commands(accepted.get(), session, false, "receiver", options.recording);
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
    if (!runtime.active() || caller.get() == SRT_INVALID_SOCK || !configure_caller(caller.get()))
    {
        std::cerr << "FAIL: caller startup\n";
        return EXIT_FAILURE;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(options.port));
    if (!srt_ok(srt_connect(caller.get(), reinterpret_cast<const sockaddr *>(&address),
                            static_cast<int>(sizeof(address))),
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
    return run_commands(caller.get(), session, true, "sender", options.recording);
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

bool parse_options(const int argc, char **argv, Options &options)
{
    std::optional<RecordingConfig> recording;
    bool saw_recording_option = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view option(argv[index]);
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
        (options.role == "caller" && options.port == 0))
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
            std::cerr << "usage: incident-peer --role listener|caller [--port PORT] "
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
