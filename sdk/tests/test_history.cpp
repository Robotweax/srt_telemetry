#include <robotweax/srt_telemetry_sampler.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

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

constexpr std::uint64_t second = UINT64_C(1'000'000'000);
constexpr std::uint64_t base_unix = UINT64_C(1'800'000'000) * second;

std::uint64_t bit(const robotweax_srt_telemetry_metric metric)
{
    return UINT64_C(1) << static_cast<unsigned>(metric);
}

void test_compact_record()
{
    robotweax_srt_telemetry_window_summary_v1 window{};
    window.struct_size = sizeof(window);
    window.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    window.window_nano = second;
    window.start_unix_nano = base_unix;
    window.sample_count = 10;
    window.quality_flags = ROBOTWEAX_SRT_TELEMETRY_WINDOW_SAMPLING_GAP;
    auto &m = window.metrics;
    m[ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND].rate = 5e6;
    m[ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS].mean = 0.02;
    m[ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS].max = 0.3;
    m[ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO].min = 0.25;
    m[ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO].mean = 0.9;
    m[ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_LOSS_PACKETS_PER_SECOND].total = 42.0;
    m[ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_DROP_PACKETS_PER_SECOND].total = 1e12;
    window.available_metrics = bit(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND) |
                               bit(ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS) |
                               bit(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO) |
                               bit(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_LOSS_PACKETS_PER_SECOND) |
                               bit(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_DROP_PACKETS_PER_SECOND);

    robotweax_srt_telemetry_compact_record_v1 record{};
    check(robotweax_srt_telemetry_compact_record_from_window(&window, &record) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "compact conversion");
    check(record.start_unix_nano == base_unix && record.window_milliseconds == 1'000 &&
              record.sample_count == 10 &&
              (record.quality_flags & ROBOTWEAX_SRT_TELEMETRY_WINDOW_SAMPLING_GAP) != 0,
          "window identity and flags");
    check(record.unique_sent_bits_per_second == 5e6F && record.rtt_max_seconds == 0.3F &&
              record.recovery_headroom_min == 0.25F && record.receiver_loss_packets == 42,
          "values carried over");
    check(record.receiver_drop_packets == UINT32_MAX, "counts saturate instead of wrapping");
    check((record.available & ROBOTWEAX_SRT_TELEMETRY_COMPACT_SENDER_LOSS) == 0 &&
              record.sender_loss_packets == 0,
          "missing metric stays unavailable");
    check((record.available & ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECEIVER_LOSS) != 0,
          "available metric is marked");
    static_assert(sizeof(robotweax_srt_telemetry_compact_record_v1) == 112);
}

robotweax_srt_telemetry_event_v1 host_event(const char *detail)
{
    robotweax_srt_telemetry_event_v1 event{};
    event.struct_size = sizeof(event);
    event.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    event.kind = ROBOTWEAX_SRT_TELEMETRY_EVENT_HOST;
    event.severity = ROBOTWEAX_SRT_TELEMETRY_SEVERITY_INFO;
    std::strncpy(event.detail, detail, sizeof(event.detail) - 1);
    return event;
}

void test_journal()
{
    robotweax_srt_telemetry_event_journal *journal = nullptr;
    check(robotweax_srt_telemetry_event_journal_create(4, &journal) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "journal creation");
    for (int index = 0; index < 6; ++index)
    {
        auto event = host_event(index == 5 ? "line\nbreak" : "event");
        event.code = index;
        std::uint64_t sequence = 0;
        static_cast<void>(robotweax_srt_telemetry_event_journal_push(journal, &event, &sequence));
        check(sequence == static_cast<std::uint64_t>(index + 1), "sequences increase by one");
    }
    check(robotweax_srt_telemetry_event_journal_size(journal) == 4 &&
              robotweax_srt_telemetry_event_journal_overwritten_count(journal) == 2,
          "ring keeps the newest four");
    robotweax_srt_telemetry_event_v1 events[8]{};
    std::size_t count = 0;
    static_cast<void>(robotweax_srt_telemetry_event_journal_read(journal, 0, events, 8, &count));
    check(count == 4 && events[0].sequence == 3 && events[3].sequence == 6,
          "read from the oldest retained");
    check(std::string_view(events[3].detail) == "line?break", "control characters are replaced");
    static_cast<void>(robotweax_srt_telemetry_event_journal_read(journal, 4, events, 8, &count));
    check(count == 2 && events[0].sequence == 5, "read after a sequence");
    static_cast<void>(robotweax_srt_telemetry_event_journal_read(journal, 6, events, 8, &count));
    check(count == 0, "nothing after the newest");
    static_cast<void>(robotweax_srt_telemetry_event_journal_read(journal, 0, events, 1, &count));
    check(count == 1 && events[0].sequence == 3, "capacity limits the copy");
    static_cast<void>(robotweax_srt_telemetry_event_journal_read(journal, 1, events, 8, &count));
    check(count == 4 && events[0].sequence == 3,
          "a cursor older than the oldest retained event reads from the oldest");

    auto invalid = host_event("bad");
    invalid.kind = static_cast<robotweax_srt_telemetry_event_kind>(999);
    check(robotweax_srt_telemetry_event_journal_push(journal, &invalid, nullptr) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "unknown kind rejected");
    invalid = host_event("bad");
    invalid.severity = static_cast<robotweax_srt_telemetry_event_severity>(0);
    check(robotweax_srt_telemetry_event_journal_push(journal, &invalid, nullptr) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "unknown severity rejected");

    auto full = host_event("");
    std::memset(full.detail, 'x', sizeof(full.detail)); /* no terminator */
    static_cast<void>(robotweax_srt_telemetry_event_journal_push(journal, &full, nullptr));
    auto trailing = host_event("");
    std::memset(trailing.detail, 0x7f, sizeof(trailing.detail));
    std::memcpy(trailing.detail, "ok", 3); /* garbage after the terminator */
    static_cast<void>(robotweax_srt_telemetry_event_journal_push(journal, &trailing, nullptr));
    static_cast<void>(robotweax_srt_telemetry_event_journal_read(journal, 6, events, 8, &count));
    check(count == 2 && std::strlen(events[0].detail) == sizeof(events[0].detail) - 1,
          "an unterminated detail is truncated and terminated");
    bool zeroed = count == 2 && std::string_view(events[1].detail) == "ok";
    for (std::size_t index = 3; zeroed && index < sizeof(events[1].detail); ++index)
    {
        zeroed = events[1].detail[index] == '\0';
    }
    check(zeroed, "bytes after the terminator are zeroed");
    robotweax_srt_telemetry_event_journal_destroy(journal);

    /* Concurrent writers and a reader: every sequence is unique and ordered. */
    static_cast<void>(robotweax_srt_telemetry_event_journal_create(1'000, &journal));
    std::vector<std::thread> writers;
    for (int thread = 0; thread < 4; ++thread)
    {
        writers.emplace_back(
            [journal]
            {
                for (int index = 0; index < 250; ++index)
                {
                    auto event = host_event("concurrent");
                    static_cast<void>(
                        robotweax_srt_telemetry_event_journal_push(journal, &event, nullptr));
                }
            });
    }
    std::vector<robotweax_srt_telemetry_event_v1> seen(1'000);
    for (auto &writer : writers)
    {
        writer.join();
    }
    static_cast<void>(
        robotweax_srt_telemetry_event_journal_read(journal, 0, seen.data(), seen.size(), &count));
    bool ordered = count == 1'000;
    for (std::size_t index = 0; ordered && index < count; ++index)
    {
        ordered = seen[index].sequence == index + 1;
    }
    check(ordered, "concurrent pushes keep contiguous sequences");
    robotweax_srt_telemetry_event_journal_destroy(journal);
}

struct FakeSource
{
    std::uint64_t packets = 0;
    std::uint64_t last = 0;
    bool fail = false;
    robotweax_srt_telemetry_delta_engine *engine = nullptr;
};

robotweax_srt_telemetry_error fake_sample(void *data, const std::uint64_t unix_nano,
                                          const std::uint64_t monotonic_nano,
                                          robotweax_srt_telemetry_source_output_v1 *output)
{
    auto &source = *static_cast<FakeSource *>(data);
    if (source.fail)
    {
        output->native_result = -7;
        return ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE;
    }
    if (source.last != 0)
    {
        source.packets += (monotonic_nano - source.last) / UINT64_C(1'000'000);
    }
    source.last = monotonic_nano;
    auto &raw = output->raw;
    raw.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    raw.observed_at_unix_nano = unix_nano;
    raw.sample_monotonic_nano = monotonic_nano;
    raw.available_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS;
    raw.unique_sent_wire_byte_total = source.packets * 1'000;
    raw.rtt_seconds = 0.03;
    return robotweax_srt_telemetry_delta_engine_update(source.engine, &raw, &output->delta);
}

std::vector<robotweax_srt_telemetry_event_v1> all_events(robotweax_srt_telemetry_sampler *sampler)
{
    std::vector<robotweax_srt_telemetry_event_v1> events(256);
    std::size_t count = 0;
    static_cast<void>(robotweax_srt_telemetry_sampler_journal_read(sampler, 0, events.data(),
                                                                   events.size(), &count, nullptr));
    events.resize(count);
    return events;
}

int count_kind(const std::vector<robotweax_srt_telemetry_event_v1> &events,
               const robotweax_srt_telemetry_event_kind kind)
{
    int total = 0;
    for (const auto &event : events)
    {
        total += event.kind == kind ? 1 : 0;
    }
    return total;
}

void test_sampler_history_and_events()
{
    robotweax_srt_telemetry_sampler_config_v1 config{};
    robotweax_srt_telemetry_sampler_config_default(&config);
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    static_cast<void>(robotweax_srt_telemetry_sampler_create(&config, &sampler));
    check(robotweax_srt_telemetry_sampler_record_event(sampler, nullptr) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "null event rejected");
    auto host = host_event("before journal");
    check(robotweax_srt_telemetry_sampler_record_event(sampler, &host) ==
              ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND,
          "no journal yet");
    check(robotweax_srt_telemetry_sampler_enable_history(sampler, 3) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_sampler_enable_journal(sampler, 64) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK,
          "enable history and journal");

    FakeSource source;
    static_cast<void>(robotweax_srt_telemetry_delta_engine_create(&source.engine));
    const robotweax_srt_telemetry_label_v1 labels[] = {{"site", "studio-2"}, {"channel", "news"}};
    robotweax_srt_telemetry_source_config_v1 source_cfg{};
    source_cfg.struct_size = sizeof(source_cfg);
    source_cfg.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    source_cfg.sample = &fake_sample;
    source_cfg.source_data = &source;
    source_cfg.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    source_cfg.labels = labels;
    source_cfg.label_count = 2;
    std::uint64_t id = 0;
    static_cast<void>(robotweax_srt_telemetry_sampler_add_source(sampler, &source_cfg, &id));

    std::uint64_t tick_index = 0;
    const auto tick = [&](const std::uint64_t ticks)
    {
        for (std::uint64_t index = 0; index < ticks; ++index, ++tick_index)
        {
            const auto offset = tick_index * second / 10;
            static_cast<void>(
                robotweax_srt_telemetry_sampler_tick(sampler, base_unix + offset, second + offset));
        }
    };
    tick(55); /* 5.5 s: five closed 1 s windows */
    robotweax_srt_telemetry_compact_record_v1 records[8]{};
    std::size_t count = 0;
    check(robotweax_srt_telemetry_sampler_history_read(sampler, id, 0, records, 8, &count) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              count == 3,
          "history keeps the newest three 1 s records");
    check(count == 3 && records[0].start_unix_nano == base_unix + 2 * second &&
              records[2].start_unix_nano == base_unix + 4 * second,
          "history in time order");
    check(count == 3 && std::abs(records[2].unique_sent_bits_per_second - 8e6F) < 1.0F &&
              records[2].window_milliseconds == 1'000,
          "history record holds the 1 s rate");
    check(robotweax_srt_telemetry_sampler_history_read(sampler, id, base_unix + 4 * second, records,
                                                       8, &count) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              count == 1,
          "history read since a time");
    check(robotweax_srt_telemetry_sampler_history_read(sampler, 999, 0, records, 8, &count) ==
              ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND,
          "unknown source");

    source.fail = true;
    tick(3);
    source.fail = false;
    tick(15); /* a clean window between the outage and the jump */
    /* A 3 s jump forward in both clocks: a sampling gap in the next window. */
    tick_index += 30;
    tick(11);
    check(robotweax_srt_telemetry_sampler_record_event(sampler, &host) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "host event recorded");
    static_cast<void>(robotweax_srt_telemetry_sampler_remove_source(sampler, id));

    const auto events = all_events(sampler);
    check(count_kind(events, ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_ADDED) == 1 &&
              std::string_view(events[0].detail) == "site=studio-2,channel=news" &&
              events[0].source_id == id,
          "SOURCE_ADDED carries the labels");
    check(count_kind(events, ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_FAILED) == 1 &&
              count_kind(events, ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_RECOVERED) == 1,
          "one failure and one recovery event for a three-tick outage");
    for (const auto &event : events)
    {
        if (event.kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_SAMPLING_GAP)
        {
            check(event.sample_monotonic_nano != 0 &&
                      event.observed_at_unix_nano > static_cast<std::uint64_t>(event.code),
                  "window events stamped at close time, window start in code");
        }
    }
    for (const auto &event : events)
    {
        if (event.kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_FAILED)
        {
            check(event.code == -7 && event.severity == ROBOTWEAX_SRT_TELEMETRY_SEVERITY_ERROR &&
                      event.current_value == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE,
                  "failure event carries error and native result");
        }
    }
    check(count_kind(events, ROBOTWEAX_SRT_TELEMETRY_EVENT_SAMPLING_GAP) == 2,
          "sampling gaps after the outage and after the jump, from the 1 s window only");
    check(count_kind(events, ROBOTWEAX_SRT_TELEMETRY_EVENT_CLOCK_STEP) == 0,
          "recovering from an outage is not a clock step");
    check(count_kind(events, ROBOTWEAX_SRT_TELEMETRY_EVENT_HOST) == 1 &&
              count_kind(events, ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_REMOVED) == 1,
          "host and removal events");
    bool ordered = true;
    for (std::size_t index = 1; index < events.size(); ++index)
    {
        ordered = ordered && events[index].sequence == events[index - 1].sequence + 1;
    }
    check(ordered, "journal sequences contiguous");
    check(std::string_view(robotweax_srt_telemetry_event_kind_name(
              ROBOTWEAX_SRT_TELEMETRY_EVENT_SAMPLING_GAP)) == "sampling_gap",
          "event kind names");

    /* Replacing or re-enabling the journal keeps sequences going, so a
     * reader's cursor stays valid. */
    const auto last_sequence = events.empty() ? 0 : events.back().sequence;
    std::size_t read_count = 0;
    check(robotweax_srt_telemetry_sampler_enable_journal(sampler, 0) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_sampler_journal_read(sampler, 0, nullptr, 0, &read_count,
                                                           nullptr) ==
                  ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND,
          "journal can be turned off");
    static_cast<void>(robotweax_srt_telemetry_sampler_enable_journal(sampler, 8));
    static_cast<void>(robotweax_srt_telemetry_sampler_record_event(sampler, &host));
    static_cast<void>(robotweax_srt_telemetry_sampler_enable_journal(sampler, 16));
    static_cast<void>(robotweax_srt_telemetry_sampler_record_event(sampler, &host));
    robotweax_srt_telemetry_event_v1 after[4]{};
    std::uint64_t overwritten = 99;
    check(robotweax_srt_telemetry_sampler_journal_read(sampler, last_sequence, after, 4,
                                                       &read_count, &overwritten) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              read_count == 1 && after[0].sequence == last_sequence + 2 && overwritten == 0,
          "sequences continue across journal replacement");
    auto authority = host;
    authority.kind = ROBOTWEAX_SRT_TELEMETRY_EVENT_SEND_AUTHORITY_CHANGED;
    std::snprintf(authority.detail, sizeof(authority.detail),
                  "observed; local_sender; revision=3; changes=2");
    check(robotweax_srt_telemetry_sampler_record_event(sampler, &authority) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "observed authority event accepted independently of host/activity kinds");
    check(std::string_view(robotweax_srt_telemetry_event_kind_name(authority.kind)) ==
              "send_authority_changed",
          "authority journal kind exports a distinct name");
    const auto observed_authority = all_events(sampler);
    check(observed_authority.back().kind == authority.kind &&
              std::string_view(observed_authority.back().detail).find("changes=2") !=
                  std::string_view::npos,
          "revision-gap disclosure retained rather than inventing intermediate transitions");
    robotweax_srt_telemetry_sampler_destroy(sampler);
    robotweax_srt_telemetry_delta_engine_destroy(source.engine);
}

struct Harness
{
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    FakeSource source;
    std::uint64_t id = 0;
    std::uint64_t tick_index = 0;

    Harness()
    {
        robotweax_srt_telemetry_sampler_config_v1 config{};
        robotweax_srt_telemetry_sampler_config_default(&config);
        static_cast<void>(robotweax_srt_telemetry_sampler_create(&config, &sampler));
        static_cast<void>(robotweax_srt_telemetry_sampler_enable_journal(sampler, 256));
        static_cast<void>(robotweax_srt_telemetry_delta_engine_create(&source.engine));
        robotweax_srt_telemetry_source_config_v1 source_cfg{};
        source_cfg.struct_size = sizeof(source_cfg);
        source_cfg.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        source_cfg.sample = &fake_sample;
        source_cfg.source_data = &source;
        static_cast<void>(robotweax_srt_telemetry_sampler_add_source(sampler, &source_cfg, &id));
    }
    ~Harness()
    {
        robotweax_srt_telemetry_sampler_destroy(sampler);
        robotweax_srt_telemetry_delta_engine_destroy(source.engine);
    }
    Harness(const Harness &) = delete;
    Harness &operator=(const Harness &) = delete;

    void tick(const std::uint64_t unix_nano)
    {
        const auto monotonic = second + tick_index++ * second / 10;
        static_cast<void>(robotweax_srt_telemetry_sampler_tick(sampler, unix_nano, monotonic));
    }
};

void test_flapping_source()
{
    Harness harness;
    for (std::uint64_t index = 0; index < 40; ++index)
    {
        harness.source.fail = index % 2 == 0;
        harness.tick(base_unix + index * second / 10);
    }
    auto events = all_events(harness.sampler);
    check(count_kind(events, ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_FAILED) == 1 &&
              count_kind(events, ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_RECOVERED) == 0,
          "a source failing every other tick is one open failure, not 40 events");
    harness.source.fail = false;
    for (std::uint64_t index = 40; index < 55; ++index)
    {
        harness.tick(base_unix + index * second / 10);
    }
    events = all_events(harness.sampler);
    check(count_kind(events, ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_RECOVERED) == 1,
          "recovery after one second without failures");
}

void test_backward_clock_step()
{
    Harness harness;
    for (std::uint64_t index = 0; index < 25; ++index)
    {
        harness.tick(base_unix + index * second / 10);
    }
    /* Wall clock steps back 10 s; the monotonic clock keeps going. */
    for (std::uint64_t index = 0; index < 30; ++index)
    {
        harness.tick(base_unix - 10 * second + (25 + index) * second / 10);
    }
    const auto events = all_events(harness.sampler);
    check(count_kind(events, ROBOTWEAX_SRT_TELEMETRY_EVENT_CLOCK_STEP) == 1,
          "one backward step is one CLOCK_STEP event");
}

} // namespace

int main()
{
    test_compact_record();
    test_journal();
    test_sampler_history_and_events();
    test_flapping_source();
    test_backward_clock_step();
    if (failures != 0)
    {
        std::cerr << failures << " history and event checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all history and event checks passed\n";
    return EXIT_SUCCESS;
}
