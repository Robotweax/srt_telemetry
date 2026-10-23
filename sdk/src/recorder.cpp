#include <robotweax/srt_telemetry.h>
#include <robotweax/srt_telemetry_health.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
/* A sample's member state (1alpha2), kept beside its record. */
struct MemberCell
{
    bool present = false;
    std::uint32_t state = ROBOTWEAX_SRT_TELEMETRY_RECORDER_MEMBER_UNKNOWN;
    bool standby = false;
    std::uint32_t authority = ROBOTWEAX_SRT_TELEMETRY_RECORDER_AUTHORITY_UNKNOWN;
};

/* A health transition (1alpha2), graded on the sample with this push number:
 * only what a health line carries (64 bytes, not the 464 of a status). */
struct HealthCell
{
    std::uint64_t sample = 0;
    std::uint64_t observed_at_unix_nano = 0;
    std::uint64_t verdict_since_unix_nano = 0;
    double suggested_value = 0;
    std::int32_t verdict = 0;
    std::int32_t uncapped_verdict = 0;
    std::int32_t reason = 0;
    std::int32_t knob = 0;
    std::int32_t knob_direction = 0;
    std::uint16_t usable_paths = 0;
    std::uint16_t expected_paths = 0;
    bool capped = false;
    bool standby = false;
};
} // namespace

namespace
{
/* ages[i]: the sample's connection age (1alpha3), or none. */
constexpr std::uint64_t no_age = std::numeric_limits<std::uint64_t>::max();
} // namespace

struct robotweax_srt_telemetry_recorder
{
    explicit robotweax_srt_telemetry_recorder(const std::size_t capacity)
        : records(capacity), members(capacity), ages(capacity, no_age)
    {
    }

    mutable std::mutex mutex;
    std::vector<robotweax_srt_telemetry_record_v1> records;
    std::vector<MemberCell> members;
    std::vector<std::uint64_t> ages;
    std::size_t write_index = 0;
    std::size_t size = 0;
    std::uint64_t overwritten_count = 0;
    /* Samples pushed since creation or clear: the latest is pushed - 1. */
    std::uint64_t pushed = 0;
    /* Health transitions, oldest first: the one in force at the first kept
     * sample and those graded on kept samples (at most health_limit), and
     * how many were dropped from the front. */
    std::deque<HealthCell> health;
    std::uint64_t health_dropped = 0;
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
                             const std::uint64_t overwritten_count, const std::string_view version = "1alpha1",
                             const std::string_view extra = {})
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\"schemaVersion\":\"" << version << "\",\"kind\":"
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
           << ",\"overwrittenCount\":" << overwritten_count << extra << "}\n";
    return output.str();
}

bool serialize_record(const robotweax_srt_telemetry_jsonl_export_config_v1 &config,
                      const robotweax_srt_telemetry_record_v1 &record,
                      const std::size_t sample_index, std::string &line,
                      const std::string_view version = "1alpha1", const std::string_view extra = {})
{
    const auto &raw = record.raw;
    const auto &delta = record.delta;
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
           << "{\"schemaVersion\":\"" << version
           << "\",\"kind\":"
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
    output << "}" << extra << "}\n";
    line = output.str();
    return true;
}

struct RecorderSnapshot final
{
    std::vector<robotweax_srt_telemetry_record_v1> records;
    std::vector<MemberCell> members;
    std::vector<std::uint64_t> ages;
    std::size_t capacity = 0;
    std::uint64_t overwritten_count = 0;
    /* Push number of records[0]. */
    std::uint64_t first_sample = 0;
    std::vector<HealthCell> health;
    std::uint64_t health_dropped = 0;
};

/* Drops transitions no export can write: those superseded before the first
 * kept sample (the one in force there stays). Several transitions can be
 * graded on one sample (a source whose samples fail), so a hard limit of
 * 2 x capacity + 2 bounds memory too, dropping the oldest. */
void prune_health(robotweax_srt_telemetry_recorder &recorder)
{
    const auto first = recorder.pushed - recorder.size;
    while (recorder.health.size() >= 2 && recorder.health[0].sample < first &&
           recorder.health[1].sample <= first)
    {
        recorder.health.pop_front();
        ++recorder.health_dropped;
    }
    while (recorder.health.size() > 2 * recorder.records.size() + 2)
    {
        recorder.health.pop_front();
        ++recorder.health_dropped;
    }
}

void push_locked(robotweax_srt_telemetry_recorder &recorder, const robotweax_srt_telemetry_raw_sample_v1 &raw,
                 const robotweax_srt_telemetry_delta_v1 &delta, const MemberCell &member,
                 const std::uint64_t age = no_age)
{
    recorder.records[recorder.write_index] = make_record(raw, delta);
    recorder.members[recorder.write_index] = member;
    recorder.ages[recorder.write_index] = age;
    recorder.write_index = (recorder.write_index + 1) % recorder.records.size();
    if (recorder.size < recorder.records.size())
    {
        ++recorder.size;
    }
    else if (recorder.overwritten_count != std::numeric_limits<std::uint64_t>::max())
    {
        ++recorder.overwritten_count;
    }
    ++recorder.pushed;
    prune_health(recorder);
}

RecorderSnapshot snapshot(const robotweax_srt_telemetry_recorder &recorder)
{
    std::lock_guard lock(recorder.mutex);
    RecorderSnapshot result;
    result.records.reserve(recorder.size);
    result.capacity = recorder.records.size();
    result.overwritten_count = recorder.overwritten_count;
    const auto oldest =
        recorder.size == recorder.records.size() ? recorder.write_index : std::size_t{0};
    result.members.reserve(recorder.size);
    result.ages.reserve(recorder.size);
    for (std::size_t index = 0; index < recorder.size; ++index)
    {
        result.records.push_back(recorder.records[(oldest + index) % recorder.records.size()]);
        result.members.push_back(recorder.members[(oldest + index) % recorder.records.size()]);
        result.ages.push_back(recorder.ages[(oldest + index) % recorder.records.size()]);
    }
    result.first_sample = recorder.pushed - recorder.size;
    result.health.assign(recorder.health.begin(), recorder.health.end());
    result.health_dropped = recorder.health_dropped;
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
    push_locked(*recorder, *raw, *delta, MemberCell{});
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_recorder_push_member_v1(
    robotweax_srt_telemetry_recorder *recorder, const robotweax_srt_telemetry_raw_sample_v1 *raw,
    const robotweax_srt_telemetry_delta_v1 *delta, const robotweax_srt_telemetry_recorder_member_v1 *member)
{
    return robotweax_srt_telemetry_recorder_push_sample_v1(recorder, raw, delta, member, nullptr);
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_recorder_push_sample_v1(
    robotweax_srt_telemetry_recorder *recorder, const robotweax_srt_telemetry_raw_sample_v1 *raw,
    const robotweax_srt_telemetry_delta_v1 *delta, const robotweax_srt_telemetry_recorder_member_v1 *member,
    const robotweax_srt_telemetry_recorder_connection_v1 *connection)
{
    if (recorder == nullptr || raw == nullptr || delta == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (!valid_raw(raw) || !valid_delta(delta) ||
        (member != nullptr && (member->struct_size < sizeof(*member) ||
                               member->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)) ||
        (connection != nullptr && (connection->struct_size < sizeof(*connection) ||
                                   connection->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)))
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    auto age = no_age;
    if (connection != nullptr)
    {
        if (connection->available > 1 || connection->reserved != 0 ||
            (connection->available == 1 && connection->connection_age_nano == no_age))
        {
            return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
        }
        if (connection->available == 1)
        {
            age = connection->connection_age_nano;
        }
    }
    MemberCell cell;
    if (member != nullptr)
    {
        if (member->state > ROBOTWEAX_SRT_TELEMETRY_RECORDER_MEMBER_UNKNOWN || member->standby > 1 ||
            member->authority > ROBOTWEAX_SRT_TELEMETRY_RECORDER_AUTHORITY_NONE || member->reserved != 0)
        {
            return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
        }
        cell = {true, member->state, member->standby != 0, member->authority};
    }
    std::lock_guard lock(recorder->mutex);
    push_locked(*recorder, *raw, *delta, cell, age);
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_recorder_push_health_v1(robotweax_srt_telemetry_recorder *recorder,
                                                const robotweax_srt_telemetry_health_status_v1 *status)
{
    if (recorder == nullptr || status == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (status->struct_size < sizeof(*status) || status->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    /* Refused here, so that every later export can write the line. */
    const auto named = [](const char *name) { return std::string_view(name) != "invalid"; };
    if (!named(robotweax_srt_telemetry_health_verdict_name(status->verdict)) ||
        !named(robotweax_srt_telemetry_health_verdict_name(status->uncapped_verdict)) ||
        !named(robotweax_srt_telemetry_health_reason_name(status->reason)) ||
        !named(robotweax_srt_telemetry_health_knob_name(status->knob)) ||
        static_cast<std::int64_t>(status->knob_direction) < ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_NONE ||
        static_cast<std::int64_t>(status->knob_direction) > ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_CHECK ||
        !std::isfinite(status->suggested_value))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    HealthCell cell;
    cell.observed_at_unix_nano = status->observed_at_unix_nano;
    /* The verdict's start on the wall clock, from the sample's two clocks. */
    const auto since = status->verdict_since_monotonic_nano <= status->sample_monotonic_nano
                           ? status->sample_monotonic_nano - status->verdict_since_monotonic_nano
                           : 0;
    cell.verdict_since_unix_nano = status->observed_at_unix_nano >= since ? status->observed_at_unix_nano - since : 0;
    cell.suggested_value =
        status->knob != ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE ? status->suggested_value : 0.0;
    cell.verdict = static_cast<std::int32_t>(status->verdict);
    cell.uncapped_verdict = static_cast<std::int32_t>(status->uncapped_verdict);
    cell.reason = static_cast<std::int32_t>(status->reason);
    cell.knob = static_cast<std::int32_t>(status->knob);
    cell.knob_direction = static_cast<std::int32_t>(status->knob_direction);
    cell.capped = (status->flags & ROBOTWEAX_SRT_TELEMETRY_HEALTH_STATUS_CAPPED) != 0;
    cell.standby = (status->flags & ROBOTWEAX_SRT_TELEMETRY_HEALTH_STATUS_STANDBY) != 0;
    if ((status->flags & ROBOTWEAX_SRT_TELEMETRY_HEALTH_STATUS_GROUP) != 0)
    {
        cell.usable_paths = status->group_usable_paths;
        cell.expected_paths = status->group_expected_paths;
    }
    std::lock_guard lock(recorder->mutex);
    if (recorder->pushed == 0)
    {
        return ROBOTWEAX_SRT_TELEMETRY_OK; /* no sample to grade yet */
    }
    cell.sample = recorder->pushed - 1;
    try
    {
        recorder->health.push_back(cell);
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    prune_health(*recorder);
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
    recorder->pushed = 0;
    recorder->health.clear();
    recorder->health_dropped = 0;
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

namespace
{

std::string_view member_state_name(const std::uint32_t state)
{
    switch (state)
    {
    case ROBOTWEAX_SRT_TELEMETRY_RECORDER_MEMBER_PENDING:
        return "pending";
    case ROBOTWEAX_SRT_TELEMETRY_RECORDER_MEMBER_IDLE:
        return "idle";
    case ROBOTWEAX_SRT_TELEMETRY_RECORDER_MEMBER_RUNNING:
        return "running";
    case ROBOTWEAX_SRT_TELEMETRY_RECORDER_MEMBER_BROKEN:
        return "broken";
    default:
        return "unknown";
    }
}

std::string member_json(const MemberCell &member)
{
    std::string out = ",\"member\":{\"state\":\"";
    out += member_state_name(member.present ? member.state : ROBOTWEAX_SRT_TELEMETRY_RECORDER_MEMBER_UNKNOWN);
    out += "\",\"standby\":";
    out += member.present && member.standby ? "true" : "false";
    out += ",\"authority\":";
    switch (member.present ? member.authority : ROBOTWEAX_SRT_TELEMETRY_RECORDER_AUTHORITY_UNKNOWN)
    {
    case ROBOTWEAX_SRT_TELEMETRY_RECORDER_AUTHORITY_ACTIVE:
        out += "\"active\"";
        break;
    case ROBOTWEAX_SRT_TELEMETRY_RECORDER_AUTHORITY_PROBE:
        out += "\"probe\"";
        break;
    case ROBOTWEAX_SRT_TELEMETRY_RECORDER_AUTHORITY_NONE:
        out += "\"none\"";
        break;
    default:
        out += "null";
        break;
    }
    out += '}';
    return out;
}

std::string_view knob_direction_name(const robotweax_srt_telemetry_knob_direction direction)
{
    switch (direction)
    {
    case ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_INCREASE:
        return "increase";
    case ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_DECREASE:
        return "decrease";
    case ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_CHECK:
        return "check";
    default:
        return "none";
    }
}

/* One health line; false for a status the schema cannot carry. */
bool serialize_health(const robotweax_srt_telemetry_jsonl_export_config_v1 &config, const HealthCell &cell,
                      const std::uint64_t sample_index, const bool carried, const bool group_row, std::string &line,
                      const std::string_view version)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
           << "{\"schemaVersion\":\"" << version << "\",\"kind\":\"robotweax-srt-telemetry-health\",\"streamId\":";
    append_json_string(output, config.stream_id);
    output << ",\"sampleIndex\":" << sample_index << ",\"observedAtUnixNano\":" << cell.observed_at_unix_nano
           << ",\"verdict\":\""
           << robotweax_srt_telemetry_health_verdict_name(
                  static_cast<robotweax_srt_telemetry_health_verdict>(cell.verdict))
           << "\",\"reason\":\""
           << robotweax_srt_telemetry_health_reason_name(static_cast<robotweax_srt_telemetry_health_reason>(cell.reason))
           << "\",\"knob\":\""
           << robotweax_srt_telemetry_health_knob_name(static_cast<robotweax_srt_telemetry_health_knob>(cell.knob))
           << "\",\"knobDirection\":\""
           << knob_direction_name(static_cast<robotweax_srt_telemetry_knob_direction>(cell.knob_direction))
           << "\",\"suggestedValue\":";
    if (cell.suggested_value != 0)
    {
        output << cell.suggested_value;
    }
    else
    {
        output << "null";
    }
    output << ",\"capped\":" << (cell.capped ? "true" : "false") << ",\"uncappedVerdict\":\""
           << robotweax_srt_telemetry_health_verdict_name(
                  static_cast<robotweax_srt_telemetry_health_verdict>(cell.uncapped_verdict))
           << "\",\"standby\":" << (cell.standby ? "true" : "false");
    if (group_row)
    {
        output << ",\"usablePaths\":" << cell.usable_paths << ",\"expectedPaths\":" << cell.expected_paths;
    }
    else
    {
        output << ",\"usablePaths\":null,\"expectedPaths\":null";
    }
    output << ",\"verdictSinceUnixNano\":" << cell.verdict_since_unix_nano
           << ",\"carried\":" << (carried ? "true" : "false") << "}\n";
    line = output.str();
    return true;
}

} // namespace

namespace
{

/* The 1alpha3 additions, validated. */
struct V3Header
{
    std::string session_ref; /* empty: null */
    std::string path_label;  /* lowercase hex; empty: null */
    std::int32_t group_id = -1;
    std::int32_t peer_group_id = -1;
};

bool valid_session_ref(const char *value)
{
    std::size_t length = 0;
    while (length <= ROBOTWEAX_SRT_TELEMETRY_RECORDING_SESSION_REF_CAPACITY && value[length] != '\0')
    {
        const auto character = static_cast<unsigned char>(value[length]);
        if (character < 0x20 || character > 0x7e)
        {
            return false;
        }
        ++length;
    }
    return length != 0 && length <= ROBOTWEAX_SRT_TELEMETRY_RECORDING_SESSION_REF_CAPACITY;
}

void append_optional_int(std::string &out, const std::string_view name, const std::int32_t value)
{
    out += ",\"";
    out += name;
    out += "\":";
    out += value >= 0 ? std::to_string(value) : std::string("null");
}

void append_optional_string(std::string &out, const std::string_view name, const std::string &value)
{
    out += ",\"";
    out += name;
    out += "\":";
    if (value.empty())
    {
        out += "null";
        return;
    }
    std::ostringstream text;
    append_json_string(text, value);
    out += text.str();
}

/* 1alpha2, or 1alpha3 when v3 is given. Arguments are validated. */
robotweax_srt_telemetry_error export_grouped(const robotweax_srt_telemetry_recorder &recorder,
                                             const robotweax_srt_telemetry_jsonl_export_config_v1 &config,
                                             const std::uint32_t group_type, const std::uint32_t group_row_kind,
                                             const bool health_lines, const V3Header *v3,
                                             const robotweax_srt_telemetry_jsonl_sink sink, void *user_data)
{
    static constexpr const char *group_types[] = {"null", "\"broadcast\"", "\"backup\"", "\"unknown\""};
    static constexpr const char *group_rows[] = {"null", "\"group\"", "\"member\""};
    const std::string_view version = v3 != nullptr ? "1alpha3" : "1alpha2";
    const bool member_row = group_row_kind == ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_MEMBER;
    const bool group_row = group_row_kind == ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_GROUP;
    try
    {
        const auto snap = snapshot(recorder);
        const auto first = snap.first_sample;
        const auto end = first + snap.records.size();
        /* Health lines to write: the transition in force at the first kept
         * sample (graded on an older one: carried), then those graded on kept
         * samples. Transitions older than that one are overwritten. */
        std::vector<std::pair<const HealthCell *, bool>> lines;
        std::uint64_t overwritten = snap.health_dropped;
        if (health_lines && !snap.records.empty())
        {
            const HealthCell *in_force = nullptr;
            for (const auto &cell : snap.health)
            {
                if (cell.sample < first)
                {
                    if (in_force != nullptr)
                    {
                        ++overwritten;
                    }
                    in_force = &cell;
                }
                else if (cell.sample < end)
                {
                    if (in_force != nullptr)
                    {
                        /* Superseded on the first kept sample itself: not in
                         * force there. */
                        if (cell.sample == first)
                        {
                            ++overwritten;
                        }
                        else
                        {
                            lines.emplace_back(in_force, true);
                        }
                        in_force = nullptr;
                    }
                    lines.emplace_back(&cell, false);
                }
            }
            if (in_force != nullptr)
            {
                lines.emplace_back(in_force, true);
            }
        }
        else
        {
            overwritten = 0;
        }
        std::string extra = ",\"groupType\":";
        extra += group_types[group_type];
        extra += ",\"groupRow\":";
        extra += group_rows[group_row_kind];
        extra += health_lines ? ",\"healthLines\":true" : ",\"healthLines\":false";
        extra += ",\"healthCount\":" + std::to_string(lines.size()) +
                 ",\"healthOverwrittenCount\":" + std::to_string(overwritten);
        if (v3 != nullptr)
        {
            append_optional_string(extra, "sessionRef", v3->session_ref);
            append_optional_int(extra, "groupId", v3->group_id);
            append_optional_int(extra, "peerGroupId", v3->peer_group_id);
            append_optional_string(extra, "pathLabel", v3->path_label);
            /* The first kept sample's connection: its clocks minus its age. */
            std::size_t aged = 0;
            while (aged < snap.ages.size() && snap.ages[aged] == no_age)
            {
                ++aged;
            }
            if (aged < snap.ages.size())
            {
                const auto &raw = snap.records[aged].raw;
                const auto age = snap.ages[aged];
                extra += ",\"connectedAtUnixNano\":" +
                         std::to_string(raw.observed_at_unix_nano > age ? raw.observed_at_unix_nano - age : 0) +
                         ",\"connectedAtMonotonicNano\":" +
                         std::to_string(raw.sample_monotonic_nano > age ? raw.sample_monotonic_nano - age : 0);
            }
            else
            {
                extra += ",\"connectedAtUnixNano\":null,\"connectedAtMonotonicNano\":null";
            }
        }
        const auto header = serialize_header(config, snap.records.size(), snap.capacity, snap.overwritten_count,
                                             version, extra);
        if (sink(user_data, header.data(), header.size()) != 0)
        {
            return ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE;
        }
        std::size_t next = 0;
        for (std::size_t index = 0; index < snap.records.size(); ++index)
        {
            std::string line;
            std::string sample_extra = member_row ? member_json(snap.members[index]) : ",\"member\":null";
            if (v3 != nullptr)
            {
                sample_extra += ",\"connectionAgeNano\":";
                sample_extra += snap.ages[index] != no_age ? std::to_string(snap.ages[index]) : std::string("null");
            }
            if (!serialize_record(config, snap.records[index], index, line, version, sample_extra))
            {
                return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
            }
            if (sink(user_data, line.data(), line.size()) != 0)
            {
                return ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE;
            }
            /* Health lines graded on this sample (a carried one at the first). */
            while (next < lines.size() &&
                   (lines[next].second ? index == 0 : lines[next].first->sample - first == index))
            {
                std::string health;
                if (!serialize_health(config, *lines[next].first, index, lines[next].second, group_row, health,
                                      version))
                {
                    return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
                }
                if (sink(user_data, health.data(), health.size()) != 0)
                {
                    return ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE;
                }
                ++next;
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

bool valid_group_context(const std::uint32_t group_type, const std::uint32_t group_row, const std::uint32_t health_lines)
{
    return group_type <= ROBOTWEAX_SRT_TELEMETRY_RECORDING_GROUP_UNKNOWN &&
           group_row <= ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_MEMBER && health_lines <= 1 &&
           (group_type == ROBOTWEAX_SRT_TELEMETRY_RECORDING_GROUP_NONE) ==
               (group_row == ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_NONE);
}

} // namespace

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_recorder_export_jsonl_v2(
    const robotweax_srt_telemetry_recorder *recorder, const robotweax_srt_telemetry_jsonl_export_config_v1 *config,
    const robotweax_srt_telemetry_jsonl_export_v2_options *options, const robotweax_srt_telemetry_jsonl_sink sink,
    void *user_data)
{
    if (recorder == nullptr || config == nullptr || options == nullptr || sink == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (config->struct_size < sizeof(*config) || config->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION ||
        options->struct_size < sizeof(*options) || options->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    if (!valid_export_config(config) ||
        !valid_group_context(options->group_type, options->group_row, options->health_lines) ||
        options->reserved != 0)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    return export_grouped(*recorder, *config, options->group_type, options->group_row, options->health_lines != 0,
                          nullptr, sink, user_data);
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_recorder_export_jsonl_v3(
    const robotweax_srt_telemetry_recorder *recorder, const robotweax_srt_telemetry_jsonl_export_config_v1 *config,
    const robotweax_srt_telemetry_jsonl_export_v3_options *options, const robotweax_srt_telemetry_jsonl_sink sink,
    void *user_data)
{
    if (recorder == nullptr || config == nullptr || options == nullptr || sink == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (config->struct_size < sizeof(*config) || config->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION ||
        options->struct_size < sizeof(*options) || options->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    const bool plain = options->group_row == ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_NONE;
    const bool member_row = options->group_row == ROBOTWEAX_SRT_TELEMETRY_RECORDING_ROW_MEMBER;
    const auto label_length = options->path_label_length;
    if (!valid_export_config(config) ||
        !valid_group_context(options->group_type, options->group_row, options->health_lines) ||
        options->reserved[0] != 0 || options->reserved[1] != 0 ||
        label_length > ROBOTWEAX_SRT_TELEMETRY_RECORDING_PATH_LABEL_CAPACITY || (label_length != 0 && !member_row) ||
        !std::all_of(options->path_label + label_length,
                     options->path_label + ROBOTWEAX_SRT_TELEMETRY_RECORDING_PATH_LABEL_CAPACITY,
                     [](const std::uint8_t byte) { return byte == 0; }) ||
        options->group_id < -1 || options->peer_group_id < -1 ||
        (plain && (options->group_id != -1 || options->peer_group_id != -1)) ||
        (options->session_ref != nullptr && !valid_session_ref(options->session_ref)))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    V3Header v3;
    try
    {
        if (options->session_ref != nullptr)
        {
            v3.session_ref = options->session_ref;
        }
        constexpr char hex[] = "0123456789abcdef";
        for (std::uint32_t i = 0; i < label_length; ++i)
        {
            v3.path_label += hex[options->path_label[i] >> 4];
            v3.path_label += hex[options->path_label[i] & 15];
        }
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    v3.group_id = options->group_id;
    v3.peer_group_id = options->peer_group_id;
    return export_grouped(*recorder, *config, options->group_type, options->group_row, options->health_lines != 0, &v3,
                          sink, user_data);
}
