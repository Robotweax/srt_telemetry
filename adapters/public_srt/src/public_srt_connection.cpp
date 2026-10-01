#include <robotweax/srt_telemetry_public_srt_connection.h>

#if defined(ROBOTWEAX_SRT_TELEMETRY_USE_FLAT_SRT_HEADER)
#include <srt.h>
#else
#include <srt/srt.h>
#endif

#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

struct robotweax_srt_telemetry_public_srt_connection_monitor
{
    robotweax_srt_telemetry_public_srt_connection_config_v1 config{};
    bool has_previous = false;
    robotweax_srt_telemetry_connection_snapshot_v1 previous{};
};

namespace
{

using Snapshot = robotweax_srt_telemetry_connection_snapshot_v1;
using Config = robotweax_srt_telemetry_public_srt_connection_config_v1;

constexpr int srts_nonexist = 9;

bool valid_config(const Config *config)
{
    return config != nullptr && config->struct_size >= sizeof(Config) &&
           config->abi_version == ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION && config->socket >= 0 &&
           (config->provider == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT ||
            config->provider == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT) &&
           config->getsockstate != nullptr && config->getrejectreason != nullptr &&
           config->getsockflag != nullptr && config->capture_stream_id <= 1 &&
           config->reserved == 0;
}

bool valid_result(const robotweax_srt_telemetry_public_srt_connection_result_v1 *result)
{
    return result != nullptr &&
           result->struct_size >=
               sizeof(robotweax_srt_telemetry_public_srt_connection_result_v1) &&
           result->abi_version == ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
}

/* Providers return booleans as 1 byte, most options as 4 and bandwidths as 8.
 * The returned length decides the interpretation. */
bool read_integer(const Config &config, const int option, std::int64_t &value)
{
    unsigned char buffer[8] = {};
    int length = static_cast<int>(sizeof(buffer));
    if (config.getsockflag(config.socket, option, buffer, &length) != 0)
    {
        return false;
    }
    switch (length)
    {
    case 1:
        value = buffer[0] != 0 ? 1 : 0;
        return true;
    case 4:
    {
        std::int32_t narrow = 0;
        std::memcpy(&narrow, buffer, sizeof(narrow));
        value = narrow;
        return true;
    }
    case 8:
        std::memcpy(&value, buffer, sizeof(value));
        return true;
    default:
        return false;
    }
}

bool read_text(const Config &config, const int option, char (&destination)[ROBOTWEAX_SRT_TELEMETRY_CONN_TEXT_CAPACITY])
{
    char buffer[ROBOTWEAX_SRT_TELEMETRY_CONN_TEXT_CAPACITY] = {};
    int length = static_cast<int>(sizeof(buffer) - 1);
    if (config.getsockflag(config.socket, option, buffer, &length) != 0 || length < 0 ||
        static_cast<std::size_t>(length) >= sizeof(buffer) - 1)
    {
        return false; /* unavailable rather than silently truncated */
    }
    const auto size = static_cast<std::size_t>(length);
    std::memcpy(destination, buffer, size);
    destination[size] = '\0';
    return true;
}

void read_snapshot(const Config &config, Snapshot &snapshot)
{
    auto &available = snapshot.available_fields;
    snapshot.state = config.getsockstate(config.socket);
    if (snapshot.state > 0)
    {
        available |= ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_STATE;
    }
    const auto reject = config.getrejectreason(config.socket);
    if (reject >= 0)
    {
        snapshot.reject_reason = reject;
        available |= ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_REJECT_REASON;
    }
    if (snapshot.state == srts_nonexist)
    {
        return; /* options of a vanished socket are unavailable, not zero */
    }

    std::int64_t value = 0;
    const auto integer = [&](const int option, const std::uint64_t field, auto &destination)
    {
        if (read_integer(config, option, value))
        {
            destination = static_cast<std::remove_reference_t<decltype(destination)>>(value);
            available |= field;
        }
    };
    const auto milliseconds = [&](const int option, const std::uint64_t field, double &destination)
    {
        if (read_integer(config, option, value))
        {
            destination = static_cast<double>(value) / 1'000.0;
            available |= field;
        }
    };

    milliseconds(SRTO_RCVLATENCY, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_RECEIVER_LATENCY_SECONDS,
                 snapshot.receiver_latency_seconds);
    milliseconds(SRTO_PEERLATENCY, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PEER_LATENCY_SECONDS,
                 snapshot.peer_latency_seconds);
    integer(SRTO_OHEADBW, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_OVERHEAD_PERCENT,
            snapshot.overhead_percent);
    integer(SRTO_MAXBW, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_MAX_BANDWIDTH,
            snapshot.max_bandwidth_bytes_per_second);
    integer(SRTO_INPUTBW, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_INPUT_BANDWIDTH,
            snapshot.input_bandwidth_bytes_per_second);
    integer(SRTO_MININPUTBW, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_MIN_INPUT_BANDWIDTH,
            snapshot.min_input_bandwidth_bytes_per_second);
    integer(SRTO_LOSSMAXTTL, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_LOSS_MAX_TTL_PACKETS,
            snapshot.loss_max_ttl_packets);
    milliseconds(SRTO_SNDDROPDELAY, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_SENDER_DROP_DELAY,
                 snapshot.sender_drop_delay_seconds);
    integer(SRTO_TLPKTDROP, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_TOO_LATE_PACKET_DROP,
            snapshot.too_late_packet_drop);
    integer(SRTO_TSBPDMODE, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_TSBPD_MODE, snapshot.tsbpd_mode);
    integer(SRTO_NAKREPORT, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PERIODIC_NAK_REPORT,
            snapshot.periodic_nak_report);
    integer(SRTO_PAYLOADSIZE, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PAYLOAD_SIZE_BYTES,
            snapshot.payload_size_bytes);
    integer(SRTO_FC, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_FLOW_CONTROL_PACKETS,
            snapshot.flow_control_packets);
    integer(SRTO_PEERVERSION, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PEER_SRT_VERSION,
            snapshot.peer_srt_version);
    integer(SRTO_VERSION, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_LOCAL_SRT_VERSION,
            snapshot.local_srt_version);
    integer(SRTO_KMSTATE, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_KM_STATE, snapshot.km_state);
    integer(SRTO_SNDKMSTATE, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_SEND_KM_STATE,
            snapshot.send_km_state);
    integer(SRTO_RCVKMSTATE, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_RECEIVE_KM_STATE,
            snapshot.receive_km_state);
    integer(SRTO_PBKEYLEN, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_KEY_LENGTH_BYTES,
            snapshot.key_length_bytes);
    integer(SRTO_ENFORCEDENCRYPTION, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_ENFORCED_ENCRYPTION,
            snapshot.enforced_encryption);
    integer(SRTO_KMREFRESHRATE, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_KM_REFRESH_RATE_PACKETS,
            snapshot.km_refresh_rate_packets);
    integer(SRTO_KMPREANNOUNCE, ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_KM_PREANNOUNCE_PACKETS,
            snapshot.km_preannounce_packets);

    if (read_text(config, SRTO_PACKETFILTER, snapshot.packet_filter))
    {
        available |= ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PACKET_FILTER;
    }
    if (config.capture_stream_id != 0 && read_text(config, SRTO_STREAMID, snapshot.stream_id))
    {
        available |= ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_STREAM_ID;
    }
}

std::uint64_t changed_fields(const Snapshot &previous, const Snapshot &current)
{
    std::uint64_t changed =
        (previous.available_fields ^ current.available_fields);
    const auto both = previous.available_fields & current.available_fields;
    const auto differs = [&](const std::uint64_t field, const bool different)
    {
        if ((both & field) != 0 && different)
        {
            changed |= field;
        }
    };
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_STATE, previous.state != current.state);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_REJECT_REASON,
            previous.reject_reason != current.reject_reason);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_RECEIVER_LATENCY_SECONDS,
            previous.receiver_latency_seconds != current.receiver_latency_seconds);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PEER_LATENCY_SECONDS,
            previous.peer_latency_seconds != current.peer_latency_seconds);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_OVERHEAD_PERCENT,
            previous.overhead_percent != current.overhead_percent);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_MAX_BANDWIDTH,
            previous.max_bandwidth_bytes_per_second != current.max_bandwidth_bytes_per_second);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_INPUT_BANDWIDTH,
            previous.input_bandwidth_bytes_per_second != current.input_bandwidth_bytes_per_second);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_MIN_INPUT_BANDWIDTH,
            previous.min_input_bandwidth_bytes_per_second !=
                current.min_input_bandwidth_bytes_per_second);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_LOSS_MAX_TTL_PACKETS,
            previous.loss_max_ttl_packets != current.loss_max_ttl_packets);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_SENDER_DROP_DELAY,
            previous.sender_drop_delay_seconds != current.sender_drop_delay_seconds);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_TOO_LATE_PACKET_DROP,
            previous.too_late_packet_drop != current.too_late_packet_drop);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_TSBPD_MODE,
            previous.tsbpd_mode != current.tsbpd_mode);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PERIODIC_NAK_REPORT,
            previous.periodic_nak_report != current.periodic_nak_report);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PAYLOAD_SIZE_BYTES,
            previous.payload_size_bytes != current.payload_size_bytes);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_FLOW_CONTROL_PACKETS,
            previous.flow_control_packets != current.flow_control_packets);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PEER_SRT_VERSION,
            previous.peer_srt_version != current.peer_srt_version);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_LOCAL_SRT_VERSION,
            previous.local_srt_version != current.local_srt_version);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_KM_STATE, previous.km_state != current.km_state);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_SEND_KM_STATE,
            previous.send_km_state != current.send_km_state);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_RECEIVE_KM_STATE,
            previous.receive_km_state != current.receive_km_state);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_KEY_LENGTH_BYTES,
            previous.key_length_bytes != current.key_length_bytes);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_ENFORCED_ENCRYPTION,
            previous.enforced_encryption != current.enforced_encryption);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_KM_REFRESH_RATE_PACKETS,
            previous.km_refresh_rate_packets != current.km_refresh_rate_packets);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_KM_PREANNOUNCE_PACKETS,
            previous.km_preannounce_packets != current.km_preannounce_packets);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PACKET_FILTER,
            std::strcmp(previous.packet_filter, current.packet_filter) != 0);
    differs(ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_STREAM_ID,
            std::strcmp(previous.stream_id, current.stream_id) != 0);
    return changed;
}

bool connected(const Snapshot &snapshot)
{
    return (snapshot.available_fields & ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_STATE) != 0 &&
           snapshot.state == ROBOTWEAX_SRT_TELEMETRY_CONNECTION_STATE_CONNECTED;
}

bool rejected(const Snapshot &snapshot)
{
    return (snapshot.available_fields & ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_REJECT_REASON) != 0 &&
           snapshot.reject_reason != 0;
}

std::uint64_t derive_events(const Snapshot &previous, const Snapshot &current,
                            const std::uint64_t changed)
{
    /* A socket that closes makes every option unreadable. That availability
     * loss stays in changed_fields, but it is not a renegotiation, so the
     * settings, KM and version events use value changes only. */
    const auto value_changed =
        changed & previous.available_fields & current.available_fields;
    std::uint64_t events = 0;
    if ((changed & ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_STATE) != 0)
    {
        events |= ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_STATE_CHANGED;
    }
    if (!connected(previous) && connected(current))
    {
        events |= ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_CONNECTED;
    }
    if (connected(previous) && !connected(current))
    {
        events |= ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_DISCONNECTED;
    }
    if ((changed & ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_REJECT_REASON) != 0 && rejected(current))
    {
        events |= ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_REJECTED;
    }
    if ((value_changed & ROBOTWEAX_SRT_TELEMETRY_CONN_KM_FIELDS) != 0)
    {
        events |= ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_KM_STATE_CHANGED;
    }
    if ((value_changed & ROBOTWEAX_SRT_TELEMETRY_CONN_SETTINGS_FIELDS) != 0)
    {
        events |= ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_SETTINGS_CHANGED;
    }
    if ((value_changed & ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PEER_SRT_VERSION) != 0)
    {
        events |= ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_PEER_VERSION_CHANGED;
    }
    return events;
}

} // namespace

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_public_srt_connection_monitor_create(
    const robotweax_srt_telemetry_public_srt_connection_config_v1 *config,
    robotweax_srt_telemetry_public_srt_connection_monitor **monitor)
{
    if (monitor == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *monitor = nullptr;
    if (!valid_config(config))
    {
        return config != nullptr && config->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION
                   ? ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH
                   : ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    auto *created = new (std::nothrow) robotweax_srt_telemetry_public_srt_connection_monitor{};
    if (created == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    created->config = *config;
    *monitor = created;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" void robotweax_srt_telemetry_public_srt_connection_monitor_destroy(
    robotweax_srt_telemetry_public_srt_connection_monitor *monitor)
{
    delete monitor;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_public_srt_connection_monitor_poll(
    robotweax_srt_telemetry_public_srt_connection_monitor *monitor,
    const std::uint64_t observed_at_unix_nano, const std::uint64_t sample_monotonic_nano,
    robotweax_srt_telemetry_public_srt_connection_result_v1 *result)
{
    if (monitor == nullptr || result == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (!valid_result(result))
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }

    std::memset(result, 0, sizeof(*result));
    result->struct_size = sizeof(*result);
    result->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    auto &snapshot = result->snapshot;
    snapshot.struct_size = sizeof(snapshot);
    snapshot.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    snapshot.observed_at_unix_nano = observed_at_unix_nano;
    snapshot.sample_monotonic_nano = sample_monotonic_nano;
    read_snapshot(monitor->config, snapshot);

    if (!monitor->has_previous)
    {
        result->events = ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_FIRST_SNAPSHOT;
        if (connected(snapshot))
        {
            result->events |= ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_CONNECTED;
        }
        if (rejected(snapshot))
        {
            result->events |= ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_REJECTED;
        }
        result->previous_state = 0;
        result->previous_km_state = 0;
    }
    else
    {
        const auto &previous = monitor->previous;
        result->changed_fields = changed_fields(previous, snapshot);
        result->events = derive_events(previous, snapshot, result->changed_fields);
        result->previous_state = previous.state;
        result->previous_km_state = previous.km_state;
    }
    monitor->previous = snapshot;
    monitor->has_previous = true;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_public_srt_connection_events(
    const robotweax_srt_telemetry_public_srt_connection_result_v1 *result,
    const std::uint64_t source_id, robotweax_srt_telemetry_event_v1 *events,
    const std::size_t capacity, std::size_t *count)
{
    if (result == nullptr || count == nullptr || (events == nullptr && capacity > 0))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *count = 0;
    if (!valid_result(result))
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    const auto &snapshot = result->snapshot;
    std::size_t produced = 0;
    const auto emit = [&](const robotweax_srt_telemetry_event_kind kind,
                          const robotweax_srt_telemetry_event_severity severity,
                          const std::int64_t previous, const std::int64_t current,
                          const std::int64_t code, const char *detail)
    {
        if (produced >= capacity)
        {
            return;
        }
        auto &event = events[produced++];
        std::memset(&event, 0, sizeof(event));
        event.struct_size = sizeof(event);
        event.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        event.observed_at_unix_nano = snapshot.observed_at_unix_nano;
        event.sample_monotonic_nano = snapshot.sample_monotonic_nano;
        event.source_id = source_id;
        event.kind = kind;
        event.severity = severity;
        event.previous_value = previous;
        event.current_value = current;
        event.code = code;
        std::strncpy(event.detail, detail, sizeof(event.detail) - 1);
    };
    const auto events_mask = result->events;
    if ((events_mask & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_CONNECTED) != 0)
    {
        emit(ROBOTWEAX_SRT_TELEMETRY_EVENT_CONNECTED, ROBOTWEAX_SRT_TELEMETRY_SEVERITY_INFO,
             result->previous_state, snapshot.state, 0, "connected");
    }
    if ((events_mask & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_DISCONNECTED) != 0)
    {
        emit(ROBOTWEAX_SRT_TELEMETRY_EVENT_DISCONNECTED, ROBOTWEAX_SRT_TELEMETRY_SEVERITY_WARNING,
             result->previous_state, snapshot.state, 0, "connection lost");
    }
    if ((events_mask & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_REJECTED) != 0)
    {
        emit(ROBOTWEAX_SRT_TELEMETRY_EVENT_REJECTED, ROBOTWEAX_SRT_TELEMETRY_SEVERITY_ERROR, 0,
             snapshot.reject_reason, 0, "connection rejected");
    }
    if ((events_mask & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_KM_STATE_CHANGED) != 0)
    {
        const auto bad = snapshot.km_state >= 3;
        emit(ROBOTWEAX_SRT_TELEMETRY_EVENT_KM_STATE_CHANGED,
             bad ? ROBOTWEAX_SRT_TELEMETRY_SEVERITY_WARNING : ROBOTWEAX_SRT_TELEMETRY_SEVERITY_INFO,
             result->previous_km_state, snapshot.km_state, 0,
             bad ? "encryption key problem" : "encryption state changed");
    }
    if ((events_mask & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_SETTINGS_CHANGED) != 0)
    {
        emit(ROBOTWEAX_SRT_TELEMETRY_EVENT_SETTINGS_CHANGED, ROBOTWEAX_SRT_TELEMETRY_SEVERITY_INFO, 0,
             0, static_cast<std::int64_t>(result->changed_fields &
                                          ROBOTWEAX_SRT_TELEMETRY_CONN_SETTINGS_FIELDS),
             "negotiated settings changed");
    }
    if ((events_mask & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_PEER_VERSION_CHANGED) != 0)
    {
        emit(ROBOTWEAX_SRT_TELEMETRY_EVENT_PEER_VERSION_CHANGED,
             ROBOTWEAX_SRT_TELEMETRY_SEVERITY_INFO, 0, snapshot.peer_srt_version, 0,
             "peer SRT version changed");
    }
    *count = produced;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}
