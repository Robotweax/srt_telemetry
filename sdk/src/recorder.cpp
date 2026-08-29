#include <robotweax/srt_telemetry.h>

#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

struct robotweax_srt_telemetry_recorder
{
    explicit robotweax_srt_telemetry_recorder(const std::size_t capacity) : records(capacity) {}

    mutable std::mutex mutex;
    std::vector<robotweax_srt_telemetry_record_v1> records;
    std::size_t write_index = 0;
    std::size_t size = 0;
    std::uint64_t overwritten_count = 0;
};

namespace
{

constexpr std::size_t maximum_capacity = 1'000'000;
constexpr std::size_t maximum_metadata_length = 1'024;

bool valid_raw(const robotweax_srt_telemetry_raw_sample_v1 *raw)
{
    return raw != nullptr && raw->struct_size >= sizeof(robotweax_srt_telemetry_raw_sample_v1) &&
           raw->abi_version == ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
}

bool valid_delta(const robotweax_srt_telemetry_delta_v1 *delta)
{
    return delta != nullptr && delta->struct_size >= sizeof(robotweax_srt_telemetry_delta_v1) &&
           delta->abi_version == ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
}

bool valid_record(const robotweax_srt_telemetry_record_v1 *record)
{
    return record != nullptr && record->struct_size >= sizeof(robotweax_srt_telemetry_record_v1) &&
           record->abi_version == ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
}

robotweax_srt_telemetry_record_v1 make_record(const robotweax_srt_telemetry_raw_sample_v1 &raw,
                                              const robotweax_srt_telemetry_delta_v1 &delta)
{
    robotweax_srt_telemetry_record_v1 record{};
    record.struct_size = sizeof(record);
    record.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    record.raw = raw;
    record.delta = delta;
    return record;
}

bool valid_metadata(const char *value)
{
    if (value == nullptr)
    {
        return false;
    }
    std::size_t length = 0;
    while (length <= maximum_metadata_length && value[length] != '\0')
    {
        const auto character = static_cast<unsigned char>(value[length]);
        if (character < 0x20 || character > 0x7e)
        {
            return false;
        }
        ++length;
    }
    return length != 0 && length <= maximum_metadata_length;
}

bool valid_export_config(const robotweax_srt_telemetry_jsonl_export_config_v1 *config)
{
    return config != nullptr && config->struct_size >= sizeof(*config) &&
           config->abi_version == ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION &&
           valid_metadata(config->stream_id) && valid_metadata(config->flow_id) &&
           valid_metadata(config->provider_version) && valid_metadata(config->provider_revision) &&
           valid_metadata(config->build_profile) &&
           (config->role == ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER ||
            config->role == ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER) &&
           (config->configured_provider == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT ||
            config->configured_provider == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT) &&
           config->reserved == 0;
}

std::string_view provider_name(const robotweax_srt_telemetry_provider provider)
{
    switch (provider)
    {
    case ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT:
        return "haivision-srt";
    case ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT:
        return "robotweax-srt";
    case ROBOTWEAX_SRT_TELEMETRY_PROVIDER_UNKNOWN:
        return "unknown";
    }
    return "unknown";
}

std::string_view role_name(const robotweax_srt_telemetry_endpoint_role role)
{
    return role == ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER ? "sender" : "receiver";
}

void append_json_string(std::ostream &output, const std::string_view value)
{
    output << '"';
    for (const char character : value)
    {
        if (character == '"' || character == '\\')
        {
            output << '\\';
        }
        output << character;
    }
    output << '"';
}

void append_nullable_unsigned(std::ostream &output, const std::string_view name,
                              const std::uint64_t available, const std::uint64_t field,
                              const std::uint64_t value)
{
    output << ",\"" << name << "\":";
    if ((available & field) == 0)
    {
        output << "null";
    }
    else
    {
        output << value;
    }
}

bool append_nullable_double(std::ostream &output, const std::string_view name,
                            const std::uint64_t available, const std::uint64_t field,
                            const double value)
{
    output << ",\"" << name << "\":";
    if ((available & field) == 0)
    {
        output << "null";
        return true;
    }
    if (!std::isfinite(value))
    {
        return false;
    }
    output << value;
    return true;
}

std::string serialize_header(const robotweax_srt_telemetry_jsonl_export_config_v1 &config,
                             const std::size_t record_count, const std::size_t capacity,
                             const std::uint64_t overwritten_count)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\"schemaVersion\":\"1alpha1\",\"kind\":"
              "\"robotweax-srt-telemetry-recording\",\"streamId\":";
    append_json_string(output, config.stream_id);
    output << ",\"flowId\":";
    append_json_string(output, config.flow_id);
    output << ",\"role\":";
    append_json_string(output, role_name(config.role));
    output << ",\"configuredProvider\":";
    append_json_string(output, provider_name(config.configured_provider));
    output << ",\"providerVersion\":";
    append_json_string(output, config.provider_version);
    output << ",\"providerRevision\":";
    append_json_string(output, config.provider_revision);
    output << ",\"buildProfile\":";
    append_json_string(output, config.build_profile);
    output << ",\"telemetryVersion\":\"" << ROBOTWEAX_SRT_TELEMETRY_VERSION
           << "\",\"recordCount\":" << record_count << ",\"capacity\":" << capacity
           << ",\"overwrittenCount\":" << overwritten_count << "}\n";
    return output.str();
}

bool serialize_record(const robotweax_srt_telemetry_jsonl_export_config_v1 &config,
                      const robotweax_srt_telemetry_record_v1 &record,
                      const std::size_t sample_index, std::string &line)
{
    const auto &raw = record.raw;
    const auto &delta = record.delta;
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
           << "{\"schemaVersion\":\"1alpha1\",\"kind\":"
              "\"robotweax-srt-telemetry-sample\",\"streamId\":";
    append_json_string(output, config.stream_id);
    output << ",\"sampleIndex\":" << sample_index << ",\"raw\":{\"provider\":";
    append_json_string(output, provider_name(raw.provider));
    output << ",\"capabilities\":" << raw.capabilities
           << ",\"available_fields\":" << raw.available_fields
           << ",\"observed_at_unix_nano\":" << raw.observed_at_unix_nano
           << ",\"sample_monotonic_nano\":" << raw.sample_monotonic_nano;

#define ROBOTWEAX_APPEND_RAW_UNSIGNED(field_name, member_name)                                     \
    append_nullable_unsigned(output, #member_name, raw.available_fields, field_name,               \
                             raw.member_name)

    ROBOTWEAX_APPEND_RAW_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL,
                                  sent_packet_total);
    ROBOTWEAX_APPEND_RAW_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_PACKET_TOTAL,
                                  received_packet_total);
    ROBOTWEAX_APPEND_RAW_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL,
                                  unique_sent_packet_total);
    ROBOTWEAX_APPEND_RAW_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL,
                                  unique_received_packet_total);
    ROBOTWEAX_APPEND_RAW_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_LOSS_PACKET_TOTAL,
                                  sender_loss_packet_total);
    ROBOTWEAX_APPEND_RAW_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_LOSS_PACKET_TOTAL,
                                  receiver_loss_packet_total);
    ROBOTWEAX_APPEND_RAW_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_PACKET_TOTAL,
                                  retransmitted_packet_total);
    ROBOTWEAX_APPEND_RAW_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_DROP_PACKET_TOTAL,
                                  sender_drop_packet_total);
    ROBOTWEAX_APPEND_RAW_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_DROP_PACKET_TOTAL,
                                  receiver_drop_packet_total);
    ROBOTWEAX_APPEND_RAW_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_UNDECRYPTABLE_PACKET_TOTAL,
                                  receiver_undecryptable_packet_total);
    ROBOTWEAX_APPEND_RAW_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL,
                                  sent_wire_byte_total);
    ROBOTWEAX_APPEND_RAW_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_WIRE_BYTE_TOTAL,
                                  received_wire_byte_total);
    ROBOTWEAX_APPEND_RAW_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL,
                                  unique_sent_wire_byte_total);
    ROBOTWEAX_APPEND_RAW_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL,
                                  unique_received_wire_byte_total);
    ROBOTWEAX_APPEND_RAW_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_WIRE_BYTE_TOTAL,
                                  retransmitted_wire_byte_total);

#undef ROBOTWEAX_APPEND_RAW_UNSIGNED

    if (!append_nullable_double(output, "rtt_seconds", raw.available_fields,
                                ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS, raw.rtt_seconds))
    {
        return false;
    }

#define ROBOTWEAX_APPEND_RAW_GAUGE(field_name, member_name)                                        \
    append_nullable_unsigned(output, #member_name, raw.available_fields, field_name,               \
                             raw.member_name)

    ROBOTWEAX_APPEND_RAW_GAUGE(ROBOTWEAX_SRT_TELEMETRY_FIELD_FLIGHT_SIZE_PACKETS,
                               flight_size_packets);
    ROBOTWEAX_APPEND_RAW_GAUGE(ROBOTWEAX_SRT_TELEMETRY_FIELD_FLOW_WINDOW_PACKETS,
                               flow_window_packets);
    ROBOTWEAX_APPEND_RAW_GAUGE(ROBOTWEAX_SRT_TELEMETRY_FIELD_CONGESTION_WINDOW_PACKETS,
                               congestion_window_packets);
    ROBOTWEAX_APPEND_RAW_GAUGE(ROBOTWEAX_SRT_TELEMETRY_FIELD_AVAILABLE_SEND_BUFFER_BYTES,
                               available_send_buffer_bytes);
    ROBOTWEAX_APPEND_RAW_GAUGE(ROBOTWEAX_SRT_TELEMETRY_FIELD_AVAILABLE_RECEIVE_BUFFER_BYTES,
                               available_receive_buffer_bytes);
    ROBOTWEAX_APPEND_RAW_GAUGE(ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_BYTES, send_buffer_bytes);
    ROBOTWEAX_APPEND_RAW_GAUGE(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_BYTES,
                               receive_buffer_bytes);

#undef ROBOTWEAX_APPEND_RAW_GAUGE

    if (!append_nullable_double(output, "send_buffer_seconds", raw.available_fields,
                                ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_SECONDS,
                                raw.send_buffer_seconds) ||
        !append_nullable_double(output, "receive_buffer_seconds", raw.available_fields,
                                ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_SECONDS,
                                raw.receive_buffer_seconds) ||
        !append_nullable_double(output, "sender_tsbpd_delay_seconds", raw.available_fields,
                                ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_TSBPD_DELAY_SECONDS,
                                raw.sender_tsbpd_delay_seconds) ||
        !append_nullable_double(output, "receiver_tsbpd_delay_seconds", raw.available_fields,
                                ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_TSBPD_DELAY_SECONDS,
                                raw.receiver_tsbpd_delay_seconds))
    {
        return false;
    }

    output << "},\"delta\":{\"quality_flags\":" << delta.quality_flags
           << ",\"interval_nano\":" << delta.interval_nano
           << ",\"available_delta_fields\":" << delta.available_delta_fields
           << ",\"counter_reset_fields\":" << delta.counter_reset_fields
           << ",\"available_rates\":" << delta.available_rates;

#define ROBOTWEAX_APPEND_DELTA_UNSIGNED(field_name, member_name)                                   \
    append_nullable_unsigned(output, #member_name, delta.available_delta_fields, field_name,       \
                             delta.member_name)

    ROBOTWEAX_APPEND_DELTA_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL,
                                    sent_packet_delta);
    ROBOTWEAX_APPEND_DELTA_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_PACKET_TOTAL,
                                    received_packet_delta);
    ROBOTWEAX_APPEND_DELTA_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL,
                                    unique_sent_packet_delta);
    ROBOTWEAX_APPEND_DELTA_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL,
                                    unique_received_packet_delta);
    ROBOTWEAX_APPEND_DELTA_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_LOSS_PACKET_TOTAL,
                                    sender_loss_packet_delta);
    ROBOTWEAX_APPEND_DELTA_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_LOSS_PACKET_TOTAL,
                                    receiver_loss_packet_delta);
    ROBOTWEAX_APPEND_DELTA_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_PACKET_TOTAL,
                                    retransmitted_packet_delta);
    ROBOTWEAX_APPEND_DELTA_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_DROP_PACKET_TOTAL,
                                    sender_drop_packet_delta);
    ROBOTWEAX_APPEND_DELTA_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_DROP_PACKET_TOTAL,
                                    receiver_drop_packet_delta);
    ROBOTWEAX_APPEND_DELTA_UNSIGNED(
        ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_UNDECRYPTABLE_PACKET_TOTAL,
        receiver_undecryptable_packet_delta);
    ROBOTWEAX_APPEND_DELTA_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL,
                                    sent_wire_byte_delta);
    ROBOTWEAX_APPEND_DELTA_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_WIRE_BYTE_TOTAL,
                                    received_wire_byte_delta);
    ROBOTWEAX_APPEND_DELTA_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL,
                                    unique_sent_wire_byte_delta);
    ROBOTWEAX_APPEND_DELTA_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL,
                                    unique_received_wire_byte_delta);
    ROBOTWEAX_APPEND_DELTA_UNSIGNED(ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_WIRE_BYTE_TOTAL,
                                    retransmitted_wire_byte_delta);

#undef ROBOTWEAX_APPEND_DELTA_UNSIGNED

    if (!append_nullable_double(output, "sent_wire_bits_per_second", delta.available_rates,
                                ROBOTWEAX_SRT_TELEMETRY_RATE_SENT_WIRE_BITS_PER_SECOND,
                                delta.sent_wire_bits_per_second) ||
        !append_nullable_double(output, "received_wire_bits_per_second", delta.available_rates,
                                ROBOTWEAX_SRT_TELEMETRY_RATE_RECEIVED_WIRE_BITS_PER_SECOND,
                                delta.received_wire_bits_per_second) ||
        !append_nullable_double(output, "unique_sent_wire_bits_per_second", delta.available_rates,
                                ROBOTWEAX_SRT_TELEMETRY_RATE_UNIQUE_SENT_WIRE_BITS_PER_SECOND,
                                delta.unique_sent_wire_bits_per_second) ||
        !append_nullable_double(output, "unique_received_wire_bits_per_second",
                                delta.available_rates,
                                ROBOTWEAX_SRT_TELEMETRY_RATE_UNIQUE_RECEIVED_WIRE_BITS_PER_SECOND,
                                delta.unique_received_wire_bits_per_second))
    {
        return false;
    }
    output << "}}\n";
    line = output.str();
    return true;
}

struct RecorderSnapshot final
{
    std::vector<robotweax_srt_telemetry_record_v1> records;
    std::size_t capacity = 0;
    std::uint64_t overwritten_count = 0;
};

RecorderSnapshot snapshot(const robotweax_srt_telemetry_recorder &recorder)
{
    std::lock_guard lock(recorder.mutex);
    RecorderSnapshot result;
    result.records.reserve(recorder.size);
    result.capacity = recorder.records.size();
    result.overwritten_count = recorder.overwritten_count;
    const auto oldest =
        recorder.size == recorder.records.size() ? recorder.write_index : std::size_t{0};
    for (std::size_t index = 0; index < recorder.size; ++index)
    {
        result.records.push_back(recorder.records[(oldest + index) % recorder.records.size()]);
    }
    return result;
}

} // namespace

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_recorder_create(const std::size_t capacity,
                                        robotweax_srt_telemetry_recorder **recorder)
{
    if (recorder == nullptr || capacity == 0 || capacity > maximum_capacity)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *recorder = nullptr;
    try
    {
        *recorder = new robotweax_srt_telemetry_recorder(capacity);
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" void robotweax_srt_telemetry_recorder_destroy(robotweax_srt_telemetry_recorder *recorder)
{
    delete recorder;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_recorder_push(robotweax_srt_telemetry_recorder *recorder,
                                      const robotweax_srt_telemetry_raw_sample_v1 *raw,
                                      const robotweax_srt_telemetry_delta_v1 *delta)
{
    if (recorder == nullptr || raw == nullptr || delta == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (!valid_raw(raw) || !valid_delta(delta))
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }

    std::lock_guard lock(recorder->mutex);
    recorder->records[recorder->write_index] = make_record(*raw, *delta);
    recorder->write_index = (recorder->write_index + 1) % recorder->records.size();
    if (recorder->size < recorder->records.size())
    {
        ++recorder->size;
    }
    else if (recorder->overwritten_count != std::numeric_limits<std::uint64_t>::max())
    {
        ++recorder->overwritten_count;
    }
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_recorder_read(const robotweax_srt_telemetry_recorder *recorder,
                                      const std::size_t oldest_first_index,
                                      robotweax_srt_telemetry_record_v1 *record)
{
    if (recorder == nullptr || record == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (!valid_record(record))
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }

    std::lock_guard lock(recorder->mutex);
    if (oldest_first_index >= recorder->size)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    const auto oldest =
        recorder->size == recorder->records.size() ? recorder->write_index : std::size_t{0};
    *record = recorder->records[(oldest + oldest_first_index) % recorder->records.size()];
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" std::size_t
robotweax_srt_telemetry_recorder_size(const robotweax_srt_telemetry_recorder *recorder)
{
    if (recorder == nullptr)
    {
        return 0;
    }
    std::lock_guard lock(recorder->mutex);
    return recorder->size;
}

extern "C" std::size_t
robotweax_srt_telemetry_recorder_capacity(const robotweax_srt_telemetry_recorder *recorder)
{
    return recorder == nullptr ? 0 : recorder->records.size();
}

extern "C" std::uint64_t
robotweax_srt_telemetry_recorder_overwritten_count(const robotweax_srt_telemetry_recorder *recorder)
{
    if (recorder == nullptr)
    {
        return 0;
    }
    std::lock_guard lock(recorder->mutex);
    return recorder->overwritten_count;
}

extern "C" void robotweax_srt_telemetry_recorder_clear(robotweax_srt_telemetry_recorder *recorder)
{
    if (recorder == nullptr)
    {
        return;
    }
    std::lock_guard lock(recorder->mutex);
    recorder->write_index = 0;
    recorder->size = 0;
    recorder->overwritten_count = 0;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_recorder_export_jsonl(
    const robotweax_srt_telemetry_recorder *recorder,
    const robotweax_srt_telemetry_jsonl_export_config_v1 *config,
    const robotweax_srt_telemetry_jsonl_sink sink, void *user_data)
{
    if (recorder == nullptr || config == nullptr || sink == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (config->struct_size < sizeof(*config) ||
        config->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    if (!valid_export_config(config))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    try
    {
        const auto recorder_snapshot = snapshot(*recorder);
        const auto header =
            serialize_header(*config, recorder_snapshot.records.size(), recorder_snapshot.capacity,
                             recorder_snapshot.overwritten_count);
        if (sink(user_data, header.data(), header.size()) != 0)
        {
            return ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE;
        }
        for (std::size_t index = 0; index < recorder_snapshot.records.size(); ++index)
        {
            std::string line;
            if (!serialize_record(*config, recorder_snapshot.records[index], index, line))
            {
                return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
            }
            if (sink(user_data, line.data(), line.size()) != 0)
            {
                return ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE;
            }
        }
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    catch (...)
    {
        return ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE;
    }
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}
