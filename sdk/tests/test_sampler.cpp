#include <robotweax/srt_telemetry_sampler.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
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

/* A fake sender: 1,000 unique packets/s of 1,316-byte payload plus 44 bytes
 * of headers, RTT 20 ms. It keeps its own lifetime counters. */
struct FakeSource
{
    std::uint64_t packets = 0;
    std::uint64_t last_monotonic = 0;
    bool fail = false;
    int calls = 0;
    robotweax_srt_telemetry_delta_engine *engine = nullptr;
};

robotweax_srt_telemetry_error fake_sample(void *data, const std::uint64_t unix_nano,
                                          const std::uint64_t monotonic_nano,
                                          robotweax_srt_telemetry_source_output_v1 *output)
{
    auto &source = *static_cast<FakeSource *>(data);
    source.calls += 1;
    if (source.fail)
    {
        output->native_result = -1;
        return ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE;
    }
    if (source.last_monotonic != 0 && monotonic_nano > source.last_monotonic)
    {
        source.packets += (monotonic_nano - source.last_monotonic) / UINT64_C(1'000'000);
    }
    source.last_monotonic = monotonic_nano;

    auto &raw = output->raw;
    raw.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
    raw.observed_at_unix_nano = unix_nano;
    raw.sample_monotonic_nano = monotonic_nano;
    raw.available_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS;
    raw.unique_sent_packet_total = source.packets;
    raw.unique_sent_wire_byte_total = source.packets * 1'360;
    raw.rtt_seconds = 0.02;
    return robotweax_srt_telemetry_delta_engine_update(source.engine, &raw, &output->delta);
}

struct Observed
{
    std::mutex mutex;
    std::map<std::uint64_t, int> samples;
    std::map<std::uint64_t, std::vector<robotweax_srt_telemetry_window_summary_v1>> windows;
    std::map<std::uint64_t, int> errors;
    std::map<std::uint64_t, std::string> site_label;
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    robotweax_srt_telemetry_error reentrant_result = ROBOTWEAX_SRT_TELEMETRY_OK;
};

void on_sample(void *user, const robotweax_srt_telemetry_source_info_v1 *source,
               const robotweax_srt_telemetry_source_output_v1 *, const robotweax_srt_telemetry_metric_sample_v1 *)
{
    auto &observed = *static_cast<Observed *>(user);
    std::lock_guard lock(observed.mutex);
    observed.samples[source->source_id] += 1;
    for (std::uint32_t index = 0; index < source->label_count; ++index)
    {
        if (std::string_view(source->labels[index].key) == "site")
        {
            observed.site_label[source->source_id] = source->labels[index].value;
        }
    }
    if (observed.sampler != nullptr)
    {
        observed.reentrant_result = robotweax_srt_telemetry_sampler_tick(observed.sampler, 0, 0);
    }
}

void on_window(void *user, const robotweax_srt_telemetry_source_info_v1 *source,
               const robotweax_srt_telemetry_window_summary_v1 *window)
{
    auto &observed = *static_cast<Observed *>(user);
    std::lock_guard lock(observed.mutex);
    observed.windows[source->source_id].push_back(*window);
}

void on_error(void *user, const robotweax_srt_telemetry_source_info_v1 *source,
              const robotweax_srt_telemetry_error, const std::int32_t)
{
    auto &observed = *static_cast<Observed *>(user);
    std::lock_guard lock(observed.mutex);
    observed.errors[source->source_id] += 1;
}

robotweax_srt_telemetry_sampler_config_v1 config_for(Observed &observed)
{
    robotweax_srt_telemetry_sampler_config_v1 config{};
    robotweax_srt_telemetry_sampler_config_default(&config);
    config.callbacks.user_data = &observed;
    config.callbacks.on_sample = &on_sample;
    config.callbacks.on_window = &on_window;
    config.callbacks.on_error = &on_error;
    return config;
}

robotweax_srt_telemetry_source_config_v1 source_config(FakeSource &source,
                                                       const robotweax_srt_telemetry_label_v1 *labels,
                                                       const std::uint32_t count)
{
    robotweax_srt_telemetry_source_config_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config.sample = &fake_sample;
    config.source_data = &source;
    config.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    config.labels = labels;
    config.label_count = count;
    return config;
}

void test_manual_ticks()
{
    Observed observed;
    auto config = config_for(observed);
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    check(robotweax_srt_telemetry_sampler_create(&config, &sampler) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "sampler creation");

    FakeSource a;
    FakeSource b;
    static_cast<void>(robotweax_srt_telemetry_delta_engine_create(&a.engine));
    static_cast<void>(robotweax_srt_telemetry_delta_engine_create(&b.engine));
    std::string site = "studio-2";
    const robotweax_srt_telemetry_label_v1 labels[] = {{"site", site.c_str()}, {"channel", "news"}};
    auto config_a = source_config(a, labels, 2);
    auto config_b = source_config(b, nullptr, 0);
    std::uint64_t id_a = 0;
    std::uint64_t id_b = 0;
    check(robotweax_srt_telemetry_sampler_add_source(sampler, &config_a, &id_a) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_sampler_add_source(sampler, &config_b, &id_b) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              id_a != id_b,
          "two sources with distinct IDs");
    site = "overwritten"; /* labels must have been copied */

    for (int tick = 0; tick < 25; ++tick)
    {
        const auto offset = static_cast<std::uint64_t>(tick) * second / 10;
        check(robotweax_srt_telemetry_sampler_tick(sampler, base_unix + offset, second + offset) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK,
              "manual tick");
    }
    check(observed.samples[id_a] == 25 && observed.samples[id_b] == 25, "every source every tick");
    check(observed.site_label[id_a] == "studio-2", "labels reach callbacks, copied at add time");
    check(observed.windows[id_a].size() == 2, "two closed 1 s windows per source");
    if (observed.windows[id_a].size() == 2)
    {
        const auto &window = observed.windows[id_a][1];
        const auto &rate =
            window.metrics[ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_PACKETS_PER_SECOND];
        check(std::abs(rate.rate - 1'000.0) < 1e-6 && rate.sample_count == 10,
              "windowed packet rate from the fake's counters");
        const auto &bits = window.metrics[ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND];
        check(std::abs(bits.rate - 1'000.0 * 1'360 * 8) < 1e-3, "windowed bit rate");
    }

    /* Errors are reported per source and do not stop other sources. */
    a.fail = true;
    static_cast<void>(robotweax_srt_telemetry_sampler_tick(sampler, base_unix + 26 * second / 10,
                                                           second + 26 * second / 10));
    check(observed.errors[id_a] == 1 && observed.errors[id_b] == 0 && observed.samples[id_b] == 26,
          "one failing source is reported, the other is sampled");

    /* Removal flushes the source's open windows and stops sampling it. */
    const auto windows_before = observed.windows[id_a].size();
    check(robotweax_srt_telemetry_sampler_remove_source(sampler, id_a) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "remove source");
    check(observed.windows[id_a].size() == windows_before + 3 &&
              (observed.windows[id_a].back().quality_flags &
               ROBOTWEAX_SRT_TELEMETRY_WINDOW_PARTIAL) != 0,
          "removal flushes 1 s, 10 s and 60 s windows as PARTIAL");
    const auto calls = a.calls;
    static_cast<void>(robotweax_srt_telemetry_sampler_tick(sampler, base_unix + 27 * second / 10,
                                                           second + 27 * second / 10));
    check(a.calls == calls, "a removed source is never called again");
    check(robotweax_srt_telemetry_sampler_remove_source(sampler, id_a) ==
              ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND,
          "removing twice reports NOT_FOUND");
    check(robotweax_srt_telemetry_sampler_source_count(sampler) == 1, "one source left");

    /* Re-entrant calls from a callback are refused instead of deadlocking. */
    observed.sampler = sampler;
    static_cast<void>(robotweax_srt_telemetry_sampler_tick(sampler, base_unix + 28 * second / 10,
                                                           second + 28 * second / 10));
    check(observed.reentrant_result == ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE,
          "re-entrant tick from a callback is INVALID_STATE");
    observed.sampler = nullptr;

    const auto windows_b = observed.windows[id_b].size();
    robotweax_srt_telemetry_sampler_destroy(sampler);
    check(observed.windows[id_b].size() == windows_b + 3, "destroy flushes remaining sources");
    robotweax_srt_telemetry_delta_engine_destroy(a.engine);
    robotweax_srt_telemetry_delta_engine_destroy(b.engine);
}

void test_validation()
{
    Observed observed;
    auto config = config_for(observed);
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    config.period_nano = second / 20; /* disagrees with windows.expected_period_nano */
    check(robotweax_srt_telemetry_sampler_create(&config, &sampler) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "period must match the window period");
    config = config_for(observed);
    config.windows.window_nano[0] = 7 * second;
    check(robotweax_srt_telemetry_sampler_create(&config, &sampler) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "window configuration is validated at creation");
    config = config_for(observed);
    static_cast<void>(robotweax_srt_telemetry_sampler_create(&config, &sampler));

    FakeSource source;
    std::uint64_t id = 0;
    const robotweax_srt_telemetry_label_v1 bad_key[] = {{"1site", "x"}};
    const robotweax_srt_telemetry_label_v1 bad_value[] = {{"site", "line\nbreak"}};
    const robotweax_srt_telemetry_label_v1 duplicate[] = {{"site", "a"}, {"site", "b"}};
    for (const auto *labels : {bad_key, bad_value})
    {
        auto source_cfg = source_config(source, labels, 1);
        check(robotweax_srt_telemetry_sampler_add_source(sampler, &source_cfg, &id) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
              "invalid label rejected");
    }
    auto dup_cfg = source_config(source, duplicate, 2);
    check(robotweax_srt_telemetry_sampler_add_source(sampler, &dup_cfg, &id) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "duplicate label keys rejected");
    const robotweax_srt_telemetry_label_v1 many[ROBOTWEAX_SRT_TELEMETRY_MAX_LABELS + 1] = {
        {"a", "1"}, {"b", "2"}, {"c", "3"}, {"d", "4"}, {"e", "5"},
        {"f", "6"}, {"g", "7"}, {"h", "8"}, {"i", "9"}};
    auto too_many = source_config(source, many, ROBOTWEAX_SRT_TELEMETRY_MAX_LABELS + 1);
    check(robotweax_srt_telemetry_sampler_add_source(sampler, &too_many, &id) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "too many labels rejected");
    check(robotweax_srt_telemetry_sampler_source_count(sampler) == 0, "nothing was added");
    robotweax_srt_telemetry_sampler_destroy(sampler);
}

void test_thread()
{
    Observed observed;
    auto config = config_for(observed);
    config.period_nano = second / 50; /* 20 ms */
    config.windows.expected_period_nano = config.period_nano;
    config.windows.window_count = 1;
    config.windows.window_nano[0] = second / 10;
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    check(robotweax_srt_telemetry_sampler_create(&config, &sampler) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "threaded sampler creation");
    FakeSource source;
    static_cast<void>(robotweax_srt_telemetry_delta_engine_create(&source.engine));
    auto source_cfg = source_config(source, nullptr, 0);
    std::uint64_t id = 0;
    static_cast<void>(robotweax_srt_telemetry_sampler_add_source(sampler, &source_cfg, &id));

    check(robotweax_srt_telemetry_sampler_start(sampler) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "thread start");
    check(robotweax_srt_telemetry_sampler_start(sampler) == ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE,
          "second start refused");
    check(robotweax_srt_telemetry_sampler_tick(sampler, base_unix, second) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE,
          "manual tick refused while the thread runs");
    const auto started = std::chrono::steady_clock::now();
    /* Let it sample for 400 ms, longer if the machine is busy (shared CI
     * runners can stall a thread): the lower bound waits for 10 samples
     * for up to 10 s; the upper bound below still holds the period. */
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    for (int i = 0; i < 960; ++i)
    {
        {
            std::unique_lock lock(observed.mutex);
            if (observed.samples[id] >= 10)
            {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    /* Remove from another thread while sampling: must not race. */
    const auto remove_started = std::chrono::steady_clock::now();
    check(robotweax_srt_telemetry_sampler_remove_source(sampler, id) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "remove while running");
    const auto remove_took = std::chrono::steady_clock::now() - remove_started;
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto calls = source.calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    check(source.calls == calls, "no sampling after remove returned");

    const auto stop_started = std::chrono::steady_clock::now();
    check(robotweax_srt_telemetry_sampler_stop(sampler) == ROBOTWEAX_SRT_TELEMETRY_OK, "stop");
    const auto stop_took = std::chrono::steady_clock::now() - stop_started;
    check(robotweax_srt_telemetry_sampler_stop(sampler) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "stop is idempotent");

    std::unique_lock lock(observed.mutex);
    const auto samples = observed.samples[id];
    /* At least 10 samples, and at most about one per 20 ms of elapsed
     * time: never bunched beyond that. */
    const auto upper = static_cast<int>(elapsed / std::chrono::milliseconds(20)) + 2;
    check(samples >= 10 && samples <= upper, "thread samples at the configured period");
    check(!observed.windows[id].empty(), "thread emits windows");
    check(remove_took < std::chrono::milliseconds(1'000) &&
              stop_took < std::chrono::milliseconds(1'000),
          "remove and stop return promptly");
    std::cout << "THREAD {\"samples\":" << samples << ",\"windows\":" << observed.windows[id].size()
              << "}\n";
    /* Never hold a lock that callbacks take while calling into the sampler. */
    lock.unlock();
    robotweax_srt_telemetry_sampler_destroy(sampler);
    robotweax_srt_telemetry_delta_engine_destroy(source.engine);
}

/* A callback of sampler A drives sampler B, then calls back into A. */
struct Nested
{
    robotweax_srt_telemetry_sampler *a = nullptr;
    robotweax_srt_telemetry_sampler *b = nullptr;
    robotweax_srt_telemetry_error b_tick = ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    robotweax_srt_telemetry_error a_again = ROBOTWEAX_SRT_TELEMETRY_OK;
};

void nested_on_sample(void *user, const robotweax_srt_telemetry_source_info_v1 *,
                      const robotweax_srt_telemetry_source_output_v1 *,
                      const robotweax_srt_telemetry_metric_sample_v1 *)
{
    auto &nested = *static_cast<Nested *>(user);
    if (nested.b != nullptr)
    {
        nested.b_tick = robotweax_srt_telemetry_sampler_tick(nested.b, base_unix, second);
        nested.a_again = robotweax_srt_telemetry_sampler_tick(nested.a, base_unix, second);
    }
}

void test_nested_samplers()
{
    Nested nested;
    robotweax_srt_telemetry_sampler_config_v1 config{};
    robotweax_srt_telemetry_sampler_config_default(&config);
    config.callbacks.user_data = &nested;
    config.callbacks.on_sample = &nested_on_sample;
    static_cast<void>(robotweax_srt_telemetry_sampler_create(&config, &nested.a));
    robotweax_srt_telemetry_sampler_config_v1 plain{};
    robotweax_srt_telemetry_sampler_config_default(&plain);
    static_cast<void>(robotweax_srt_telemetry_sampler_create(&plain, &nested.b));

    FakeSource source;
    static_cast<void>(robotweax_srt_telemetry_delta_engine_create(&source.engine));
    auto source_cfg = source_config(source, nullptr, 0);
    std::uint64_t id = 0;
    static_cast<void>(robotweax_srt_telemetry_sampler_add_source(nested.a, &source_cfg, &id));
    static_cast<void>(robotweax_srt_telemetry_sampler_tick(nested.a, base_unix, second));
    check(nested.b_tick == ROBOTWEAX_SRT_TELEMETRY_OK,
          "a callback of one sampler may drive another sampler");
    check(nested.a_again == ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE,
          "after driving another sampler, re-entering the first is still refused");
    auto *b = nested.b;
    nested.b = nullptr; /* stop the callback from driving B during destroy */
    robotweax_srt_telemetry_sampler_destroy(nested.a);
    robotweax_srt_telemetry_sampler_destroy(b);
    robotweax_srt_telemetry_delta_engine_destroy(source.engine);
}

void test_stop_races_and_restart()
{
    Observed observed;
    auto config = config_for(observed);
    config.period_nano = second / 100;
    config.windows.expected_period_nano = config.period_nano;
    config.windows.window_count = 1;
    config.windows.window_nano[0] = second / 10;
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    static_cast<void>(robotweax_srt_telemetry_sampler_create(&config, &sampler));
    FakeSource source;
    static_cast<void>(robotweax_srt_telemetry_delta_engine_create(&source.engine));
    auto source_cfg = source_config(source, nullptr, 0);
    std::uint64_t id = 0;
    static_cast<void>(robotweax_srt_telemetry_sampler_add_source(sampler, &source_cfg, &id));

    for (int round = 0; round < 20; ++round)
    {
        check(robotweax_srt_telemetry_sampler_start(sampler) == ROBOTWEAX_SRT_TELEMETRY_OK,
              "start after stop");
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        std::atomic<int> ok{0};
        std::vector<std::thread> stoppers;
        for (int index = 0; index < 4; ++index)
        {
            stoppers.emplace_back(
                [&]
                {
                    if (robotweax_srt_telemetry_sampler_stop(sampler) == ROBOTWEAX_SRT_TELEMETRY_OK)
                    {
                        ok.fetch_add(1);
                    }
                });
        }
        for (auto &stopper : stoppers)
        {
            stopper.join();
        }
        check(ok.load() == 4, "concurrent stops all succeed without crashing");
    }
    check(source.calls > 20, "restarted thread keeps sampling");
    robotweax_srt_telemetry_sampler_destroy(sampler);
    robotweax_srt_telemetry_delta_engine_destroy(source.engine);
}

void test_failing_source_windows()
{
    Observed observed;
    auto config = config_for(observed);
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    static_cast<void>(robotweax_srt_telemetry_sampler_create(&config, &sampler));
    FakeSource source;
    static_cast<void>(robotweax_srt_telemetry_delta_engine_create(&source.engine));
    source.fail = true;
    auto source_cfg = source_config(source, nullptr, 0);
    std::uint64_t id = 0;
    static_cast<void>(robotweax_srt_telemetry_sampler_add_source(sampler, &source_cfg, &id));
    for (int tick = 0; tick < 35; ++tick) /* 3.5 s of failures */
    {
        const auto offset = static_cast<std::uint64_t>(tick) * second / 10;
        static_cast<void>(
            robotweax_srt_telemetry_sampler_tick(sampler, base_unix + offset, second + offset));
    }
    check(observed.windows[id].size() == 3, "a failing source still closes its 1 s windows");
    bool flagged = !observed.windows[id].empty();
    for (const auto &window : observed.windows[id])
    {
        flagged = flagged &&
                  (window.quality_flags & ROBOTWEAX_SRT_TELEMETRY_WINDOW_SOURCE_ERROR) != 0 &&
                  window.available_metrics == 0;
    }
    check(flagged, "outage windows are flagged SOURCE_ERROR and carry no metrics");
    check(observed.errors[id] == 4, "a repeating error is reported at most once per second");
    robotweax_srt_telemetry_sampler_destroy(sampler);
    robotweax_srt_telemetry_delta_engine_destroy(source.engine);
}

} // namespace

int main()
{
    test_manual_ticks();
    test_validation();
    test_thread();
    test_nested_samplers();
    test_stop_races_and_restart();
    test_failing_source_windows();
    if (failures != 0)
    {
        std::cerr << failures << " sampler checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all sampler checks passed\n";
    return EXIT_SUCCESS;
}
