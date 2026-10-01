#include <robotweax/srt_telemetry_public_srt_connection.h>

#if __has_include(<srt/srt.h>)
#include <srt/srt.h>
#else
#include <srt.h>
#endif

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace
{

int failures = 0;

void check(const bool condition, const std::string_view message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

/* Scripted fake socket. Booleans come back as 1 byte, bandwidths as 8 and
 * everything else as 4, like the real providers. */
struct FakeSocket
{
    int state = SRTS_CONNECTED;
    int reject = 0;
    int latency_ms = 120;
    int peer_latency_ms = 120;
    int km_state = 0;
    int send_km_state = 0;
    int receive_km_state = 0;
    int key_length = 0;
    int peer_version = 0x010505;
    std::int64_t max_bw = -1;
    int drop_delay_ms = 0;
    std::string filter;
    std::string stream_id = "customer-42/channel-7";
    bool options_fail = false;
    int stream_id_reads = 0;
} fake;

int fake_state(int32_t)
{
    return fake.state;
}

int fake_reject(int32_t)
{
    return fake.reject;
}

int write_int(void *value, int *length, const int source)
{
    if (*length < 4)
    {
        return -1;
    }
    std::memcpy(value, &source, 4);
    *length = 4;
    return 0;
}

int write_bool(void *value, int *length, const bool source)
{
    *static_cast<unsigned char *>(value) = source ? 1 : 0;
    *length = 1;
    return 0;
}

int write_int64(void *value, int *length, const std::int64_t source)
{
    if (*length < 8)
    {
        return -1;
    }
    std::memcpy(value, &source, 8);
    *length = 8;
    return 0;
}

int write_text(void *value, int *length, const std::string &source)
{
    if (*length < static_cast<int>(source.size()))
    {
        return -1;
    }
    std::memcpy(value, source.data(), source.size());
    *length = static_cast<int>(source.size());
    return 0;
}

int fake_flag(int32_t, const int option, void *value, int *length)
{
    if (fake.options_fail)
    {
        return -1;
    }
    switch (option)
    {
    case SRTO_RCVLATENCY:
        return write_int(value, length, fake.latency_ms);
    case SRTO_PEERLATENCY:
        return write_int(value, length, fake.peer_latency_ms);
    case SRTO_OHEADBW:
        return write_int(value, length, 25);
    case SRTO_MAXBW:
        return write_int64(value, length, fake.max_bw);
    case SRTO_INPUTBW:
    case SRTO_MININPUTBW:
        return write_int64(value, length, 0);
    case SRTO_LOSSMAXTTL:
        return write_int(value, length, 0);
    case SRTO_SNDDROPDELAY:
        return write_int(value, length, fake.drop_delay_ms);
    case SRTO_TLPKTDROP:
    case SRTO_TSBPDMODE:
    case SRTO_NAKREPORT:
    case SRTO_ENFORCEDENCRYPTION:
        return write_bool(value, length, true);
    case SRTO_PAYLOADSIZE:
        return write_int(value, length, 1'316);
    case SRTO_FC:
        return write_int(value, length, 25'600);
    case SRTO_PEERVERSION:
        return write_int(value, length, fake.peer_version);
    case SRTO_VERSION:
        return write_int(value, length, 0x010505);
    case SRTO_KMSTATE:
        return write_int(value, length, fake.km_state);
    case SRTO_SNDKMSTATE:
        return write_int(value, length, fake.send_km_state);
    case SRTO_RCVKMSTATE:
        return write_int(value, length, fake.receive_km_state);
    case SRTO_PBKEYLEN:
        return write_int(value, length, fake.key_length);
    case SRTO_KMREFRESHRATE:
    case SRTO_KMPREANNOUNCE:
        return write_int(value, length, 0);
    case SRTO_PACKETFILTER:
        return write_text(value, length, fake.filter);
    case SRTO_STREAMID:
        ++fake.stream_id_reads;
        return write_text(value, length, fake.stream_id);
    default:
        return -1;
    }
}

robotweax_srt_telemetry_public_srt_connection_config_v1 config(const bool capture_stream_id)
{
    robotweax_srt_telemetry_public_srt_connection_config_v1 value{};
    value.struct_size = sizeof(value);
    value.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    value.socket = 7;
    value.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
    value.getsockstate = &fake_state;
    value.getrejectreason = &fake_reject;
    value.getsockflag = &fake_flag;
    value.capture_stream_id = capture_stream_id ? 1u : 0u;
    return value;
}

robotweax_srt_telemetry_public_srt_connection_result_v1 empty_result()
{
    robotweax_srt_telemetry_public_srt_connection_result_v1 result{};
    result.struct_size = sizeof(result);
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    return result;
}

robotweax_srt_telemetry_public_srt_connection_result_v1
poll(robotweax_srt_telemetry_public_srt_connection_monitor *monitor, const std::uint64_t t)
{
    auto result = empty_result();
    check(robotweax_srt_telemetry_public_srt_connection_monitor_poll(monitor, t, t, &result) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "poll succeeds");
    return result;
}

void test_snapshot_mapping()
{
    fake = {};
    fake.filter = "fec,arq:onreq,cols:4,layout:staircase,rows:2";
    fake.drop_delay_ms = -1;
    auto cfg = config(false);
    robotweax_srt_telemetry_public_srt_connection_monitor *monitor = nullptr;
    check(robotweax_srt_telemetry_public_srt_connection_monitor_create(&cfg, &monitor) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "monitor creation");
    const auto result = poll(monitor, 1);
    const auto &s = result.snapshot;
    check((result.events & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_FIRST_SNAPSHOT) != 0 &&
              (result.events & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_CONNECTED) != 0,
          "first poll of a connected socket");
    check(result.changed_fields == 0, "no change mask on the first poll");
    check(s.state == SRTS_CONNECTED && s.reject_reason == 0, "state and reject reason");
    check(std::abs(s.receiver_latency_seconds - 0.12) < 1e-12 &&
              std::abs(s.peer_latency_seconds - 0.12) < 1e-12,
          "latencies in seconds");
    check(s.max_bandwidth_bytes_per_second == -1, "8-byte MAXBW keeps its -1 (unlimited)");
    check(s.too_late_packet_drop == 1 && s.tsbpd_mode == 1, "1-byte booleans");
    check(s.sender_drop_delay_seconds < 0.0, "negative sender drop delay means drop off");
    check(s.peer_srt_version == 0x010505u, "peer version");
    check(std::string_view(s.packet_filter) == fake.filter, "packet filter text");
    check((s.available_fields & ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_STREAM_ID) == 0 &&
              s.stream_id[0] == '\0' && fake.stream_id_reads == 0,
          "stream ID is never read unless the host opts in");
    const auto expected = (UINT64_C(1) << 26) - 1 & ~ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_STREAM_ID;
    check(s.available_fields == expected, "every other field is available");
    robotweax_srt_telemetry_public_srt_connection_monitor_destroy(monitor);

    cfg = config(true);
    check(robotweax_srt_telemetry_public_srt_connection_monitor_create(&cfg, &monitor) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "monitor with stream-ID capture");
    const auto with_id = poll(monitor, 1);
    check(std::string_view(with_id.snapshot.stream_id) == fake.stream_id &&
              (with_id.snapshot.available_fields & ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_STREAM_ID) !=
                  0,
          "stream ID when the host opts in");
    robotweax_srt_telemetry_public_srt_connection_monitor_destroy(monitor);
}

void test_events()
{
    fake = {};
    auto cfg = config(false);
    robotweax_srt_telemetry_public_srt_connection_monitor *monitor = nullptr;
    static_cast<void>(robotweax_srt_telemetry_public_srt_connection_monitor_create(&cfg, &monitor));
    fake.state = SRTS_CONNECTING;
    const auto first = poll(monitor, 1);
    check((first.events & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_CONNECTED) == 0,
          "connecting is not connected");

    fake.state = SRTS_CONNECTED;
    fake.km_state = 1;
    const auto second = poll(monitor, 2);
    check((second.events & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_CONNECTED) != 0 &&
              (second.events & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_STATE_CHANGED) != 0 &&
              second.previous_state == SRTS_CONNECTING,
          "connected event with previous state");
    check((second.events & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_KM_STATE_CHANGED) != 0,
          "KM state change");

    const auto quiet = poll(monitor, 3);
    check(quiet.changed_fields == 0 && quiet.events == 0, "no change, no events");

    fake.latency_ms = 500;
    fake.max_bw = 1'250'000;
    const auto settings = poll(monitor, 4);
    check(settings.changed_fields ==
              (ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_RECEIVER_LATENCY_SECONDS |
               ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_MAX_BANDWIDTH) &&
              settings.events == ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_SETTINGS_CHANGED,
          "settings change is reported precisely");

    fake.peer_version = 0x010507;
    const auto upgraded = poll(monitor, 5);
    check(upgraded.events == (ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_PEER_VERSION_CHANGED),
          "peer version change");

    fake.state = SRTS_BROKEN;
    fake.reject = SRT_REJ_PEER;
    const auto broken = poll(monitor, 5);
    check((broken.events & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_REJECTED) != 0,
          "a reject reason appearing later is reported");
    check((broken.events & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_DISCONNECTED) != 0,
          "leaving CONNECTED is a disconnect");

    fake.state = SRTS_NONEXIST;
    fake.options_fail = true;
    const auto gone = poll(monitor, 6);
    check(gone.snapshot.state == SRTS_NONEXIST &&
              gone.snapshot.available_fields == (ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_STATE |
                                                 ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_REJECT_REASON),
          "a vanished socket keeps state but no options");
    check((gone.changed_fields & ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_RECEIVER_LATENCY_SECONDS) != 0,
          "losing availability counts as a change");
    check((gone.events & (ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_SETTINGS_CHANGED |
                          ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_KM_STATE_CHANGED |
                          ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_PEER_VERSION_CHANGED)) == 0,
          "teardown is not reported as a renegotiation");
    check((gone.events & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_STATE_CHANGED) != 0,
          "teardown is a state change");
    robotweax_srt_telemetry_public_srt_connection_monitor_destroy(monitor);
}

void test_rejection()
{
    fake = {};
    fake.state = SRTS_BROKEN;
    fake.reject = SRT_REJ_BADSECRET;
    auto cfg = config(false);
    robotweax_srt_telemetry_public_srt_connection_monitor *monitor = nullptr;
    static_cast<void>(robotweax_srt_telemetry_public_srt_connection_monitor_create(&cfg, &monitor));
    const auto first = poll(monitor, 1);
    check((first.events & ROBOTWEAX_SRT_TELEMETRY_CONN_EVENT_REJECTED) != 0 &&
              first.snapshot.reject_reason == SRT_REJ_BADSECRET,
          "a rejection seen on the first poll is reported");
    robotweax_srt_telemetry_public_srt_connection_monitor_destroy(monitor);
}

void test_validation()
{
    robotweax_srt_telemetry_public_srt_connection_monitor *monitor = nullptr;
    auto cfg = config(false);
    cfg.getsockflag = nullptr;
    check(robotweax_srt_telemetry_public_srt_connection_monitor_create(&cfg, &monitor) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "all injected functions are required");
    cfg = config(false);
    cfg.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_UNKNOWN;
    check(robotweax_srt_telemetry_public_srt_connection_monitor_create(&cfg, &monitor) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "provider must be known");
    cfg = config(false);
    cfg.reserved = 1;
    check(robotweax_srt_telemetry_public_srt_connection_monitor_create(&cfg, &monitor) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "reserved must be zero");
    cfg = config(false);
    cfg.abi_version = 99;
    check(robotweax_srt_telemetry_public_srt_connection_monitor_create(&cfg, &monitor) ==
              ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH,
          "ABI mismatch");
    check(monitor == nullptr, "failed creation leaves no monitor");

    fake = {};
    cfg = config(false);
    static_cast<void>(robotweax_srt_telemetry_public_srt_connection_monitor_create(&cfg, &monitor));
    auto result = empty_result();
    result.struct_size = 8;
    check(robotweax_srt_telemetry_public_srt_connection_monitor_poll(monitor, 1, 1, &result) ==
              ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH,
          "truncated result is an ABI mismatch");
    robotweax_srt_telemetry_public_srt_connection_monitor_destroy(monitor);
}

void test_text_is_bounded()
{
    fake = {};
    fake.filter = std::string(2'000, 'x');
    auto cfg = config(false);
    robotweax_srt_telemetry_public_srt_connection_monitor *monitor = nullptr;
    static_cast<void>(robotweax_srt_telemetry_public_srt_connection_monitor_create(&cfg, &monitor));
    const auto result = poll(monitor, 1);
    check((result.snapshot.available_fields & ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_PACKET_FILTER) ==
              0,
          "an option longer than the buffer is unavailable, never truncated silently");
    robotweax_srt_telemetry_public_srt_connection_monitor_destroy(monitor);

    fake.filter = std::string(ROBOTWEAX_SRT_TELEMETRY_CONN_TEXT_CAPACITY - 2, 'y');
    static_cast<void>(robotweax_srt_telemetry_public_srt_connection_monitor_create(&cfg, &monitor));
    const auto longest = poll(monitor, 1);
    check(std::string_view(longest.snapshot.packet_filter).size() ==
              ROBOTWEAX_SRT_TELEMETRY_CONN_TEXT_CAPACITY - 2,
          "the longest text that fits is kept whole");
    robotweax_srt_telemetry_public_srt_connection_monitor_destroy(monitor);
}

void test_journal_events()
{
    fake = {};
    fake.state = SRTS_CONNECTING;
    auto cfg = config(false);
    robotweax_srt_telemetry_public_srt_connection_monitor *monitor = nullptr;
    static_cast<void>(robotweax_srt_telemetry_public_srt_connection_monitor_create(&cfg, &monitor));
    robotweax_srt_telemetry_event_v1 events[8]{};
    std::size_t count = 0;

    const auto first = poll(monitor, 1);
    check(robotweax_srt_telemetry_public_srt_connection_events(&first, 42, events, 8, &count) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              count == 0,
          "a first poll while connecting yields no events");

    fake.state = SRTS_CONNECTED;
    fake.km_state = SRT_KM_S_BADSECRET;
    fake.latency_ms = 250;
    const auto second = poll(monitor, 2);
    static_cast<void>(
        robotweax_srt_telemetry_public_srt_connection_events(&second, 42, events, 8, &count));
    check(count == 3 && events[0].kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_CONNECTED &&
              events[0].previous_value == SRTS_CONNECTING &&
              events[0].current_value == SRTS_CONNECTED && events[0].source_id == 42 &&
              events[0].observed_at_unix_nano == 2,
          "connected event with states, source and time");
    check(count == 3 && events[1].kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_KM_STATE_CHANGED &&
              events[1].severity == ROBOTWEAX_SRT_TELEMETRY_SEVERITY_WARNING &&
              events[1].current_value == SRT_KM_S_BADSECRET,
          "a bad-secret KM state is a warning");
    check(count == 3 && events[2].kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_SETTINGS_CHANGED &&
              (events[2].code & ROBOTWEAX_SRT_TELEMETRY_CONN_FIELD_RECEIVER_LATENCY_SECONDS) != 0,
          "settings event names the changed fields");

    fake.state = SRTS_BROKEN;
    fake.reject = SRT_REJ_PEER;
    const auto third = poll(monitor, 3);
    static_cast<void>(
        robotweax_srt_telemetry_public_srt_connection_events(&third, 42, events, 1, &count));
    check(count == 1 && events[0].kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_DISCONNECTED &&
              events[0].severity == ROBOTWEAX_SRT_TELEMETRY_SEVERITY_WARNING,
          "capacity limits the output; disconnect comes first");
    static_cast<void>(
        robotweax_srt_telemetry_public_srt_connection_events(&third, 42, events, 8, &count));
    check(count == 2 && events[1].kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_REJECTED &&
              events[1].current_value == SRT_REJ_PEER,
          "rejection event carries the reason");
    robotweax_srt_telemetry_public_srt_connection_monitor_destroy(monitor);
}

void test_health_context()
{
    fake = {};
    fake.state = SRTS_CONNECTING;
    auto cfg = config(false);
    robotweax_srt_telemetry_public_srt_connection_monitor *monitor = nullptr;
    static_cast<void>(robotweax_srt_telemetry_public_srt_connection_monitor_create(&cfg, &monitor));
    robotweax_srt_telemetry_health_context_v1 context;
    robotweax_srt_telemetry_health_context_init(&context);

    auto result = poll(monitor, 1);
    check(robotweax_srt_telemetry_public_srt_health_context(&result, &context) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              context.connection == ROBOTWEAX_SRT_TELEMETRY_CONNECTION_CONNECTING,
          "health context: connecting");
    fake.state = SRTS_CONNECTED;
    result = poll(monitor, 2);
    static_cast<void>(robotweax_srt_telemetry_public_srt_health_context(&result, &context));
    check(context.connection == ROBOTWEAX_SRT_TELEMETRY_CONNECTION_CONNECTED &&
              (context.flags & ROBOTWEAX_SRT_TELEMETRY_HEALTH_CONTEXT_BANDWIDTH_SETTINGS) != 0 &&
              context.overhead_percent == 25 && context.max_bandwidth_bytes_per_second == -1,
          "health context: connected, with OHEADBW and MAXBW");
    /* Haivision SRT reports OPENED after a broken connection. */
    fake.state = SRTS_OPENED;
    result = poll(monitor, 3);
    static_cast<void>(robotweax_srt_telemetry_public_srt_health_context(&result, &context));
    check(context.connection == ROBOTWEAX_SRT_TELEMETRY_CONNECTION_LOST &&
              context.overhead_percent == 25,
          "health context: OPENED after connected is lost; settings kept");
    robotweax_srt_telemetry_health_context_v1 fresh;
    robotweax_srt_telemetry_health_context_init(&fresh);
    fake.state = SRTS_BROKEN;
    result = poll(monitor, 4);
    static_cast<void>(robotweax_srt_telemetry_public_srt_health_context(&result, &fresh));
    check(fresh.connection == ROBOTWEAX_SRT_TELEMETRY_CONNECTION_LOST, "health context: broken is lost");
    check(robotweax_srt_telemetry_public_srt_health_context(nullptr, &fresh) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "health context: null result");
    robotweax_srt_telemetry_public_srt_connection_monitor_destroy(monitor);
}

} // namespace

int main()
{
    test_snapshot_mapping();
    test_events();
    test_rejection();
    test_validation();
    test_text_is_bounded();
    test_journal_events();
    test_health_context();
    if (failures != 0)
    {
        std::cerr << failures << " connection monitor checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all connection monitor checks passed\n";
    return EXIT_SUCCESS;
}
