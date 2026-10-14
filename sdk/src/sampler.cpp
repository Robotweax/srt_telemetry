#include <robotweax/srt_telemetry_sampler.h>

#include "export_internal.h"
#include "health_internal.h"
#include "history_internal.h"
#include "thread_name.h"
#include "standby_metadata_internal.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct AggregatorDeleter
{
    void operator()(robotweax_srt_telemetry_window_aggregator *aggregator) const noexcept
    {
        robotweax_srt_telemetry_window_aggregator_destroy(aggregator);
    }
};

struct HealthDeleter
{
    void operator()(robotweax_srt_telemetry_health_engine *engine) const noexcept
    {
        robotweax_srt_telemetry_health_engine_destroy(engine);
    }
};

struct Source
{
    std::uint64_t id = 0;
    /* on_error rate limiting: report a new error code at once, a repeated
     * one at most once per second. */
    robotweax_srt_telemetry_error last_error = ROBOTWEAX_SRT_TELEMETRY_OK;
    std::uint64_t last_error_report_nano = 0;
    robotweax_srt_telemetry_source_fn sample = nullptr;
    void *data = nullptr;
    robotweax_srt_telemetry_endpoint_role role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_UNKNOWN;
    std::vector<std::string> keys;
    std::vector<std::string> values;
    std::vector<robotweax_srt_telemetry_label_v1> labels;
    std::unique_ptr<robotweax_srt_telemetry_window_aggregator, AggregatorDeleter> windows;
    /* Event state: one SOURCE_FAILED per episode (or error change); the
     * episode ends with SOURCE_RECOVERED after one second without failures,
     * so a flapping source cannot flood the journal. */
    bool failed = false;
    robotweax_srt_telemetry_error failed_error = ROBOTWEAX_SRT_TELEMETRY_OK;
    std::uint64_t healthy_since_nano = 0;
    bool healthy = false;
    /* Quality flags of the previous shortest window: flag events are
     * emitted when a flag appears, not for every flagged window. */
    std::uint32_t previous_window_flags = 0;
    /* Latest state for snapshots and exporters. */
    std::array<bool, ROBOTWEAX_SRT_TELEMETRY_MAX_WINDOWS> has_window{};
    std::array<robotweax_srt_telemetry_window_summary_v1, ROBOTWEAX_SRT_TELEMETRY_MAX_WINDOWS> latest_window{};
    /* Steady-clock time each latest window was delivered (staleness). */
    std::array<std::uint64_t, ROBOTWEAX_SRT_TELEMETRY_MAX_WINDOWS> window_delivered_steady{};
    std::string prometheus_labels;
    bool has_latest_metrics = false;
    robotweax_srt_telemetry_metric_sample_v1 latest_metrics{};
    robotweax_srt_telemetry_standby_rtt_metadata_v1 standby_rtt_metadata{};
    std::uint64_t standby_rtt_updated_steady = 0;
    robotweax_srt_telemetry_rtt_metadata_v1 rtt_metadata{};
    std::uint64_t rtt_updated_steady = 0;
    robotweax_srt_telemetry_group_send_metadata_v1 group_send_metadata{};
    std::uint64_t group_send_updated_steady = 0;
    bool has_latest_raw = false;
    robotweax_srt_telemetry_raw_sample_v1 latest_raw{};
    /* When the lifetime counters started (OTLP cumulative start time): the
     * first sample, or the latest counter reset. */
    std::uint64_t counters_start_unix_nano = 0;
    bool has_latest_ext = false;
    robotweax_srt_telemetry_ext_sample_v1 latest_ext{};
    /* Health engine (null when health is off), its context and last status. */
    std::unique_ptr<robotweax_srt_telemetry_health_engine, HealthDeleter> health;
    bool has_health_context = false;
    robotweax_srt_telemetry_health_context_v1 health_context{};
    bool has_health_group = false;
    robotweax_srt_telemetry_health_group_v1 health_group{};
    bool has_health_status = false;
    robotweax_srt_telemetry_health_status_v1 health_status{};
    /* Worst verdict of the current shortest window, for the compact record;
     * UNKNOWN ranks between OK and WATCH, so a window is OK only if every
     * graded sample in it was. */
    bool window_has_health = false;
    robotweax_srt_telemetry_health_verdict window_verdict = ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN;
    robotweax_srt_telemetry_health_reason window_reason = ROBOTWEAX_SRT_TELEMETRY_REASON_NONE;
    /* COMPACT_GROUP_* bits of the current shortest window. */
    std::uint16_t window_group = 0;
    /* Ring of compact records (empty when history is off). */
    std::vector<robotweax_srt_telemetry_compact_record_v1> history;
    std::size_t history_head = 0;
    std::size_t history_size = 0;

    void history_push(const robotweax_srt_telemetry_compact_record_v1 &record)
    {
        if (history.empty())
        {
            return;
        }
        if (history_size < history.size())
        {
            history[(history_head + history_size) % history.size()] = record;
            history_size += 1;
        }
        else
        {
            history[history_head] = record;
            history_head = (history_head + 1) % history.size();
        }
    }

    robotweax_srt_telemetry_source_info_v1 info() const noexcept
    {
        robotweax_srt_telemetry_source_info_v1 result{};
        result.source_id = id;
        result.role = role;
        result.label_count = static_cast<std::uint32_t>(labels.size());
        result.labels = labels.empty() ? nullptr : labels.data();
        return result;
    }
};

/* Samplers whose callbacks are running on this thread, innermost last. A
 * call into any of them would deadlock on its mutex and is refused. Nested
 * samplers (a callback of A driving B) are tracked separately. */
constexpr std::size_t max_callback_depth = 16;
thread_local const void *callback_owners[max_callback_depth] = {};
thread_local std::size_t callback_depth = 0;

bool in_callback_of(const void *sampler)
{
    for (std::size_t index = 0; index < callback_depth; ++index)
    {
        if (callback_owners[index] == sampler)
        {
            return true;
        }
    }
    /* Deeper nesting than tracked: refuse every call to stay safe. */
    return callback_depth > max_callback_depth;
}

class CallbackScope final
{
  public:
    explicit CallbackScope(const void *sampler) noexcept
    {
        if (callback_depth < max_callback_depth)
        {
            callback_owners[callback_depth] = sampler;
        }
        ++callback_depth;
    }
    ~CallbackScope()
    {
        --callback_depth;
        if (callback_depth < max_callback_depth)
        {
            callback_owners[callback_depth] = nullptr;
        }
    }
    CallbackScope(const CallbackScope &) = delete;
    CallbackScope &operator=(const CallbackScope &) = delete;
};

template <typename T> bool valid_struct(const T *value)
{
    return value != nullptr && value->struct_size >= sizeof(T) &&
           value->abi_version == ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
}

bool valid_key(const char *key)
{
    if (key == nullptr)
    {
        return false;
    }
    const auto length = std::strlen(key);
    if (length == 0 || length >= ROBOTWEAX_SRT_TELEMETRY_LABEL_KEY_CAPACITY)
    {
        return false;
    }
    for (std::size_t index = 0; index < length; ++index)
    {
        const auto c = static_cast<unsigned char>(key[index]);
        const auto letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        const auto digit = c >= '0' && c <= '9';
        if (!(letter || (index > 0 && digit)))
        {
            return false;
        }
    }
    return true;
}

bool valid_value(const char *value)
{
    if (value == nullptr)
    {
        return false;
    }
    const auto length = std::strlen(value);
    if (length >= ROBOTWEAX_SRT_TELEMETRY_LABEL_VALUE_CAPACITY)
    {
        return false;
    }
    for (std::size_t index = 0; index < length; ++index)
    {
        const auto c = static_cast<unsigned char>(value[index]);
        if (c < 0x20 || c > 0x7e)
        {
            return false;
        }
    }
    return true;
}

} // namespace

struct JournalDeleter
{
    void operator()(robotweax_srt_telemetry_event_journal *journal) const noexcept
    {
        robotweax_srt_telemetry_event_journal_destroy(journal);
    }
};

struct robotweax_srt_telemetry_sampler
{
    robotweax_srt_telemetry_sampler_config_v1 config{};
    /* Shortest window: source of history records and window events. */
    std::uint64_t history_window_nano = 0;
    std::size_t history_capacity = 0;
    /* Swapped under `mutex`; used under journal_mutex. */
    std::mutex journal_mutex;
    std::uint64_t journal_next_sequence = 1; /* kept while the journal is off */
    std::unique_ptr<robotweax_srt_telemetry_event_journal, JournalDeleter> journal;
    /* Health, under `mutex`. */
    bool health_enabled = false;
    robotweax_srt_telemetry_health_callback health_callback = nullptr;
    void *health_user_data = nullptr;
    struct ThresholdOverride
    {
        bool set = false;
        double watch = 0;
        double at_risk = 0;
        double failing = 0;
    };
    std::array<std::array<ThresholdOverride, ROBOTWEAX_SRT_TELEMETRY_INDICATOR_COUNT>, 3> health_thresholds{};
    /* Unix time of the latest tick, for staleness of exported windows. */
    std::uint64_t last_tick_unix = 0;
    std::mutex mutex; /* sources, tick execution */
    std::map<std::uint64_t, std::unique_ptr<Source>> sources;
    std::uint64_t next_id = 1;

    std::mutex thread_mutex; /* thread lifecycle */
    std::condition_variable wake;
    bool stop_requested = false;
    bool running = false;  /* a thread exists and has not been claimed by stop */
    bool stopping = false; /* stop is joining the thread */
    std::thread thread;
};

namespace
{

std::uint64_t steady_nano()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::uint64_t system_nano()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

void journal_event(robotweax_srt_telemetry_sampler &sampler, const std::uint64_t source_id,
                   const robotweax_srt_telemetry_event_kind kind,
                   const robotweax_srt_telemetry_event_severity severity,
                   const std::uint64_t unix_nano, const std::uint64_t monotonic_nano,
                   const std::int64_t previous, const std::int64_t current, const std::int64_t code,
                   const char *detail)
{
    std::lock_guard lock(sampler.journal_mutex);
    if (!sampler.journal)
    {
        return;
    }
    robotweax_srt_telemetry_event_v1 event{};
    event.struct_size = sizeof(event);
    event.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    event.observed_at_unix_nano = unix_nano;
    event.sample_monotonic_nano = monotonic_nano;
    event.source_id = source_id;
    event.kind = kind;
    event.severity = severity;
    event.previous_value = previous;
    event.current_value = current;
    event.code = code;
    if (detail != nullptr)
    {
        std::snprintf(event.detail, sizeof(event.detail), "%s", detail);
    }
    static_cast<void>(robotweax_srt_telemetry_event_journal_push(sampler.journal.get(), &event, nullptr));
}

std::string label_text(const Source &source)
{
    std::string text;
    for (const auto &label : source.labels)
    {
        if (!text.empty())
        {
            text += ',';
        }
        text += label.key;
        text += '=';
        text += label.value;
    }
    return text;
}

struct WindowContext
{
    robotweax_srt_telemetry_sampler *sampler;
    Source *source;
    /* When the window was closed: events about it are stamped with this. */
    std::uint64_t unix_nano;
    std::uint64_t monotonic_nano;
};

int deliver_window(void *user_data, const robotweax_srt_telemetry_window_summary_v1 *summary)
{
    const auto &context = *static_cast<WindowContext *>(user_data);
    auto &sampler = *context.sampler;
    for (std::uint32_t w = 0; w < sampler.config.windows.window_count && w < ROBOTWEAX_SRT_TELEMETRY_MAX_WINDOWS; ++w)
    {
        if (sampler.config.windows.window_nano[w] == summary->window_nano)
        {
            context.source->latest_window[w] = *summary;
            context.source->has_window[w] = true;
            context.source->window_delivered_steady[w] = steady_nano();
        }
    }
    if (summary->window_nano == sampler.history_window_nano)
    {
        auto &source = *context.source;
        if (!source.history.empty())
        {
            robotweax_srt_telemetry_compact_record_v1 record{};
            if (robotweax_srt_telemetry_compact_record_from_window(summary, &record) ==
                ROBOTWEAX_SRT_TELEMETRY_OK)
            {
                if (source.window_has_health)
                {
                    record.health_verdict = static_cast<std::uint8_t>(source.window_verdict);
                    record.health_reason = static_cast<std::uint8_t>(source.window_reason);
                    record.group = source.window_group;
                }
                source.history_push(record);
            }
        }
        source.window_has_health = false;
        source.window_verdict = ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN;
        source.window_reason = ROBOTWEAX_SRT_TELEMETRY_REASON_NONE;
        source.window_group = 0;
        struct Flag
        {
            std::uint32_t bit;
            robotweax_srt_telemetry_event_kind kind;
            const char *detail;
        };
        static constexpr Flag flags[] = {
            {ROBOTWEAX_SRT_TELEMETRY_WINDOW_SAMPLING_GAP, ROBOTWEAX_SRT_TELEMETRY_EVENT_SAMPLING_GAP,
             "sampling interval longer than twice the period"},
            {ROBOTWEAX_SRT_TELEMETRY_WINDOW_CLOCK_STEP, ROBOTWEAX_SRT_TELEMETRY_EVENT_CLOCK_STEP,
             "wall clock stepped"},
            {ROBOTWEAX_SRT_TELEMETRY_WINDOW_COUNTER_RESET,
             ROBOTWEAX_SRT_TELEMETRY_EVENT_COUNTER_RESET, "SRT counter decreased"},
        };
        /* One event per episode: a backward clock step flags both the
         * window it closes and the next one; a long overload flags many. */
        const auto started = summary->quality_flags & ~source.previous_window_flags;
        source.previous_window_flags = summary->quality_flags;
        for (const auto &flag : flags)
        {
            if ((started & flag.bit) != 0)
            {
                journal_event(sampler, source.id, flag.kind,
                              ROBOTWEAX_SRT_TELEMETRY_SEVERITY_WARNING, context.unix_nano,
                              context.monotonic_nano, 0, 0,
                              static_cast<std::int64_t>(summary->start_unix_nano), flag.detail);
            }
        }
    }
    const auto &callbacks = sampler.config.callbacks;
    if (callbacks.on_window != nullptr)
    {
        const auto info = context.source->info();
        try
        {
            callbacks.on_window(callbacks.user_data, &info, summary);
        }
        catch (...)
        {
            /* callbacks must not throw; never let it cross the C boundary */
        }
    }
    return 0;
}

void report_error(robotweax_srt_telemetry_sampler &sampler, Source &source,
                  const robotweax_srt_telemetry_error error, const std::int32_t native,
                  const std::uint64_t monotonic_nano)
{
    constexpr std::uint64_t repeat_interval = UINT64_C(1'000'000'000);
    const auto repeated = error == source.last_error &&
                          monotonic_nano >= source.last_error_report_nano &&
                          monotonic_nano - source.last_error_report_nano < repeat_interval;
    source.last_error = error;
    if (repeated)
    {
        return;
    }
    source.last_error_report_nano = monotonic_nano;
    const auto &callbacks = sampler.config.callbacks;
    if (callbacks.on_error != nullptr)
    {
        const auto info = source.info();
        try
        {
            callbacks.on_error(callbacks.user_data, &info, error, native);
        }
        catch (...)
        {
        }
    }
}

void initialize_output(robotweax_srt_telemetry_source_output_v1 &output)
{
    std::memset(&output, 0, sizeof(output));
    output.struct_size = sizeof(output);
    output.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    output.raw.struct_size = sizeof(output.raw);
    output.raw.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    output.delta.struct_size = sizeof(output.delta);
    output.delta.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    output.ext.struct_size = sizeof(output.ext);
    output.ext.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    output.ext_delta.struct_size = sizeof(output.ext_delta);
    output.ext_delta.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
}

/* Caller holds sampler.mutex. */
robotweax_srt_telemetry_error create_health(robotweax_srt_telemetry_sampler &sampler, Source &source)
{
    robotweax_srt_telemetry_health_config_v1 config;
    robotweax_srt_telemetry_health_config_default(source.role, &config);
    robotweax_srt_telemetry_health_engine *engine = nullptr;
    const auto error = robotweax_srt_telemetry_health_engine_create(&config, &engine);
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return error;
    }
    source.health.reset(engine);
    const auto role = static_cast<std::size_t>(source.role);
    for (std::size_t i = 0; i < ROBOTWEAX_SRT_TELEMETRY_INDICATOR_COUNT; ++i)
    {
        const auto &o = sampler.health_thresholds[role][i];
        if (o.set)
        {
            static_cast<void>(robotweax_srt_telemetry_health_engine_set_thresholds(
                engine, static_cast<robotweax_srt_telemetry_health_indicator>(i), o.watch, o.at_risk,
                o.failing));
        }
    }
    if (source.has_health_context)
    {
        static_cast<void>(robotweax_srt_telemetry_health_engine_set_context(engine, &source.health_context));
    }
    if (source.has_health_group)
    {
        static_cast<void>(robotweax_srt_telemetry_health_engine_set_group(engine, &source.health_group));
    }
    source.has_health_status = false;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

robotweax_srt_telemetry_event_severity verdict_severity(const robotweax_srt_telemetry_health_verdict verdict)
{
    switch (verdict)
    {
    case ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING:
        return ROBOTWEAX_SRT_TELEMETRY_SEVERITY_ERROR;
    case ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH:
    case ROBOTWEAX_SRT_TELEMETRY_HEALTH_AT_RISK:
        return ROBOTWEAX_SRT_TELEMETRY_SEVERITY_WARNING;
    default:
        return ROBOTWEAX_SRT_TELEMETRY_SEVERITY_INFO;
    }
}

/* Caller holds sampler.mutex. The status is folded into the window
 * only after the window push (fold_health), so a window that closes on this
 * sample keeps its own worst verdict. The engine runs on the sampler's tick
 * clock, the same for good and failed samples. */
void update_health(robotweax_srt_telemetry_sampler &sampler, Source &source,
                   const robotweax_srt_telemetry_metric_sample_v1 &metrics, const std::uint64_t monotonic_nano)
{
    if (!source.health)
    {
        return;
    }
    auto timed = metrics;
    timed.sample_monotonic_nano = monotonic_nano;
    robotweax_srt_telemetry_health_status_v1 status{};
    if (robotweax_srt_telemetry_health_engine_push(source.health.get(), &timed, &status) !=
        ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        /* e.g. a host tick clock that went backwards: no grade. */
        std::memset(&status, 0, sizeof(status));
        status.struct_size = sizeof(status);
        status.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        status.observed_at_unix_nano = metrics.observed_at_unix_nano;
        status.sample_monotonic_nano = monotonic_nano;
        status.verdict = ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN;
        status.reason = ROBOTWEAX_SRT_TELEMETRY_REASON_SOURCE_ERROR;
        status.primary_indicator = -1;
    }
    const auto &previous = source.health_status;
    const bool first = !source.has_health_status;
    const bool verdict_changed = first || status.verdict != previous.verdict || status.reason != previous.reason;
    /* Suggested values move with the stream; report a change of 10 % or
     * more, not every sample. */
    const auto moved = [](const double a, const double b)
    { return std::abs(a - b) > 0.1 * std::max(std::abs(a), std::abs(b)); };
    const bool advice_changed = verdict_changed || status.knob != previous.knob ||
                                status.knob_direction != previous.knob_direction ||
                                moved(status.suggested_value, previous.suggested_value);
    if (verdict_changed && !(first && status.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN))
    {
        char detail[ROBOTWEAX_SRT_TELEMETRY_EVENT_DETAIL_CAPACITY];
        if (status.knob != ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE)
        {
            const char *direction = status.knob_direction == ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_INCREASE
                                        ? "increase"
                                    : status.knob_direction == ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_DECREASE
                                        ? "decrease"
                                        : "check";
            std::snprintf(detail, sizeof(detail), "%s %s: %s %s %.6g",
                          robotweax_srt_telemetry_health_verdict_name(status.verdict),
                          robotweax_srt_telemetry_health_reason_name(status.reason),
                          robotweax_srt_telemetry_health_knob_name(status.knob), direction,
                          status.suggested_value);
        }
        else
        {
            std::snprintf(detail, sizeof(detail), "%s %s",
                          robotweax_srt_telemetry_health_verdict_name(status.verdict),
                          robotweax_srt_telemetry_health_reason_name(status.reason));
        }
        journal_event(sampler, source.id, ROBOTWEAX_SRT_TELEMETRY_EVENT_VERDICT_CHANGED,
                      verdict_severity(status.verdict), metrics.observed_at_unix_nano,
                      metrics.sample_monotonic_nano, first ? 0 : previous.verdict, status.verdict,
                      status.reason, detail);
    }
    source.health_status = status;
    source.has_health_status = true;
    if (advice_changed && sampler.health_callback != nullptr)
    {
        const auto info = source.info();
        try
        {
            sampler.health_callback(sampler.health_user_data, &info, &status);
        }
        catch (...)
        {
        }
    }
}

/* Caller holds sampler.mutex. After the window push: this sample belongs to
 * the window that is now open. */
int fold_rank(const robotweax_srt_telemetry_health_verdict verdict)
{
    switch (verdict)
    {
    case ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK:
        return 1;
    case ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN:
        return 2;
    case ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH:
        return 3;
    case ROBOTWEAX_SRT_TELEMETRY_HEALTH_AT_RISK:
        return 4;
    default:
        return 5;
    }
}

void fold_health(Source &source)
{
    if (!source.has_health_status)
    {
        return;
    }
    const auto &status = source.health_status;
    if (!source.window_has_health || fold_rank(status.verdict) >= fold_rank(source.window_verdict))
    {
        source.window_has_health = true;
        source.window_verdict = status.verdict;
        source.window_reason = status.reason;
    }
    /* Groups: paths at the latest sample, capping at any, standby at the
     * latest. */
    constexpr std::uint16_t latest_bits = ROBOTWEAX_SRT_TELEMETRY_COMPACT_GROUP_ROW | 0x0fff |
                                          ROBOTWEAX_SRT_TELEMETRY_COMPACT_GROUP_STANDBY;
    auto group = static_cast<std::uint16_t>(source.window_group & ~latest_bits);
    if ((status.flags & ROBOTWEAX_SRT_TELEMETRY_HEALTH_STATUS_GROUP) != 0)
    {
        const auto usable = std::min<unsigned>(status.group_usable_paths, 63u);
        const auto expected = std::min<unsigned>(status.group_expected_paths, 63u);
        group |= static_cast<std::uint16_t>(ROBOTWEAX_SRT_TELEMETRY_COMPACT_GROUP_ROW | usable | (expected << 6));
    }
    if ((status.flags & ROBOTWEAX_SRT_TELEMETRY_HEALTH_STATUS_CAPPED) != 0)
    {
        group |= ROBOTWEAX_SRT_TELEMETRY_COMPACT_GROUP_CAPPED;
    }
    if ((status.flags & ROBOTWEAX_SRT_TELEMETRY_HEALTH_STATUS_STANDBY) != 0)
    {
        group |= ROBOTWEAX_SRT_TELEMETRY_COMPACT_GROUP_STANDBY;
    }
    source.window_group = group;
}

/* Caller holds sampler.mutex. Called for every failed sample. */
void source_failed(robotweax_srt_telemetry_sampler &sampler, Source &source,
                   const robotweax_srt_telemetry_error error, const std::int32_t native,
                   const std::uint64_t unix_nano, const std::uint64_t monotonic_nano)
{
    if (!source.failed || error != source.failed_error)
    {
        journal_event(sampler, source.id, ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_FAILED,
                      ROBOTWEAX_SRT_TELEMETRY_SEVERITY_ERROR, unix_nano, monotonic_nano,
                      source.failed ? source.failed_error : ROBOTWEAX_SRT_TELEMETRY_OK, error,
                      native, robotweax_srt_telemetry_error_string(error));
    }
    source.failed = true;
    source.failed_error = error;
    source.healthy = false;
    report_error(sampler, source, error, native, monotonic_nano);
    /* Keep the windows moving: an empty sample marked SOURCE_ERROR closes
     * windows on time, so an outage shows as flagged windows, not silence. */
    robotweax_srt_telemetry_metric_sample_v1 empty{};
    empty.struct_size = sizeof(empty);
    empty.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    empty.observed_at_unix_nano = unix_nano;
    empty.sample_monotonic_nano = monotonic_nano;
    empty.quality_flags = ROBOTWEAX_SRT_TELEMETRY_METRIC_SAMPLE_SOURCE_ERROR;
    update_health(sampler, source, empty, monotonic_nano);
    WindowContext context{&sampler, &source, unix_nano, monotonic_nano};
    static_cast<void>(robotweax_srt_telemetry_window_aggregator_push(
        source.windows.get(), &empty, &deliver_window, &context));
    fold_health(source);
}

/* Caller holds sampler.mutex. */
void sample_source(robotweax_srt_telemetry_sampler &sampler, Source &source,
                   const std::uint64_t unix_nano, const std::uint64_t monotonic_nano)
{
    robotweax_srt_telemetry_source_output_v1 output;
    initialize_output(output);
    robotweax_srt_telemetry_error error = ROBOTWEAX_SRT_TELEMETRY_OK;
    try
    {
        error = source.sample(source.data, unix_nano, monotonic_nano, &output);
    }
    catch (...)
    {
        error = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE;
    }
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        source_failed(sampler, source, error, output.native_result, unix_nano, monotonic_nano);
        return;
    }

    robotweax_srt_telemetry_metric_sample_v1 metrics{};
    metrics.struct_size = sizeof(metrics);
    metrics.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    const auto has_ext = output.has_ext != 0;
    error = robotweax_srt_telemetry_metrics_compute(&output.raw, has_ext ? &output.ext : nullptr,
                                                    &output.delta,
                                                    has_ext ? &output.ext_delta : nullptr, &metrics);
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        /* Unusable output counts as a failed sample. */
        source_failed(sampler, source, error, output.native_result, unix_nano, monotonic_nano);
        return;
    }

    source.last_error = ROBOTWEAX_SRT_TELEMETRY_OK;
    if (source.failed)
    {
        if (!source.healthy)
        {
            source.healthy = true;
            source.healthy_since_nano = monotonic_nano;
        }
        constexpr std::uint64_t settle = UINT64_C(1'000'000'000);
        if (monotonic_nano >= source.healthy_since_nano &&
            monotonic_nano - source.healthy_since_nano >= settle)
        {
            journal_event(sampler, source.id, ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_RECOVERED,
                          ROBOTWEAX_SRT_TELEMETRY_SEVERITY_INFO, unix_nano, monotonic_nano,
                          source.failed_error, ROBOTWEAX_SRT_TELEMETRY_OK, 0,
                          "source sampling again for 1 s");
            source.failed = false;
            source.failed_error = ROBOTWEAX_SRT_TELEMETRY_OK;
        }
    }

    update_health(sampler, source, metrics, monotonic_nano);
    source.latest_metrics = metrics;
    source.has_latest_metrics = true;
    if (!source.has_latest_raw || (metrics.quality_flags & ROBOTWEAX_SRT_TELEMETRY_QUALITY_COUNTER_RESET) != 0)
    {
        /* The SRT counters count since the socket connected, not since this
         * source was added: start them at the connection when the provider
         * reports its age, so OTLP consumers do not book the whole history
         * into the first interval. */
        source.counters_start_unix_nano = unix_nano;
        if (output.has_ext != 0 &&
            (output.ext.available_fields & ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_CONNECTION_AGE_SECONDS) != 0 &&
            std::isfinite(output.ext.connection_age_seconds) && output.ext.connection_age_seconds > 0.0)
        {
            const double age_nano = output.ext.connection_age_seconds * 1e9;
            source.counters_start_unix_nano =
                age_nano >= static_cast<double>(unix_nano) ? 1 : unix_nano - static_cast<std::uint64_t>(age_nano);
        }
    }
    source.latest_raw = output.raw;
    source.has_latest_raw = true;
    source.has_latest_ext = output.has_ext != 0;
    if (source.has_latest_ext)
    {
        source.latest_ext = output.ext;
    }
    const auto &callbacks = sampler.config.callbacks;
    if (callbacks.on_sample != nullptr)
    {
        const auto info = source.info();
        try
        {
            callbacks.on_sample(callbacks.user_data, &info, &output, &metrics);
        }
        catch (...)
        {
        }
    }
    WindowContext context{&sampler, &source, unix_nano, monotonic_nano};
    static_cast<void>(robotweax_srt_telemetry_window_aggregator_push(
        source.windows.get(), &metrics, &deliver_window, &context));
    fold_health(source);
}

/* Caller holds sampler.mutex. */
void tick_locked(robotweax_srt_telemetry_sampler &sampler, const std::uint64_t unix_nano,
                 const std::uint64_t monotonic_nano)
{
    const CallbackScope scope(&sampler);
    sampler.last_tick_unix = unix_nano;
    for (auto &entry : sampler.sources)
    {
        sample_source(sampler, *entry.second, unix_nano, monotonic_nano);
    }
}

/* Caller holds sampler.mutex. */
void flush_locked(robotweax_srt_telemetry_sampler &sampler, Source &source)
{
    const CallbackScope scope(&sampler);
    WindowContext context{&sampler, &source, system_nano(), steady_nano()};
    static_cast<void>(robotweax_srt_telemetry_window_aggregator_flush(source.windows.get(),
                                                                      &deliver_window, &context));
}

std::uint64_t now_unix_nano()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

std::uint64_t to_nano(const std::chrono::steady_clock::time_point point)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(point.time_since_epoch()).count());
}

void run_thread(robotweax_srt_telemetry_sampler &sampler)
{
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(sampler.config.period_nano);
    const auto origin = clock::now();
    auto next = origin;
    while (true)
    {
        {
            std::unique_lock lock(sampler.thread_mutex);
            if (sampler.wake.wait_until(lock, next, [&] { return sampler.stop_requested; }))
            {
                return;
            }
        }
        {
            std::lock_guard lock(sampler.mutex);
            tick_locked(sampler, now_unix_nano(), to_nano(clock::now()));
        }
        next += period;
        const auto now = clock::now();
        if (next <= now)
        {
            /* Overrun: skip to the next period boundary instead of bunching. */
            const auto behind = (now - origin) / period;
            next = origin + (behind + 1) * period;
        }
    }
}

} // namespace

extern "C" void
robotweax_srt_telemetry_sampler_config_default(robotweax_srt_telemetry_sampler_config_v1 *config)
{
    if (config == nullptr)
    {
        return;
    }
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config->period_nano = UINT64_C(100'000'000);
    robotweax_srt_telemetry_window_config_default(&config->windows);
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_create(
    const robotweax_srt_telemetry_sampler_config_v1 *config,
    robotweax_srt_telemetry_sampler **sampler)
{
    if (sampler == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *sampler = nullptr;
    if (config == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (!valid_struct(config) || !valid_struct(&config->windows))
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    if (config->period_nano == 0 || config->windows.expected_period_nano != config->period_nano)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    /* Validate the window configuration once by building a throwaway. */
    robotweax_srt_telemetry_window_aggregator *probe = nullptr;
    const auto window_error = robotweax_srt_telemetry_window_aggregator_create(&config->windows, &probe);
    robotweax_srt_telemetry_window_aggregator_destroy(probe);
    if (window_error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return window_error;
    }

    auto *created = new (std::nothrow) robotweax_srt_telemetry_sampler{};
    if (created == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    created->config = *config;
    created->history_window_nano = config->windows.window_nano[0];
    for (std::uint32_t index = 1; index < config->windows.window_count; ++index)
    {
        created->history_window_nano =
            std::min(created->history_window_nano, config->windows.window_nano[index]);
    }
    *sampler = created;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" void robotweax_srt_telemetry_sampler_destroy(robotweax_srt_telemetry_sampler *sampler)
{
    if (sampler == nullptr || in_callback_of(sampler))
    {
        return; /* a call from a callback would deadlock; refused */
    }
    static_cast<void>(robotweax_srt_telemetry_sampler_stop(sampler));
    {
        std::lock_guard lock(sampler->mutex);
        for (auto &entry : sampler->sources)
        {
            flush_locked(*sampler, *entry.second);
        }
        sampler->sources.clear();
    }
    delete sampler;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_sampler_add_source(robotweax_srt_telemetry_sampler *sampler,
                                           const robotweax_srt_telemetry_source_config_v1 *source,
                                           std::uint64_t *source_id)
{
    if (sampler == nullptr || source == nullptr || source_id == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    if (!valid_struct(source))
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    const auto role = static_cast<std::int64_t>(source->role);
    if (source->sample == nullptr || source->label_count > ROBOTWEAX_SRT_TELEMETRY_MAX_LABELS ||
        (source->label_count > 0 && source->labels == nullptr) ||
        role < ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_UNKNOWN || role > ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    for (std::uint32_t index = 0; index < source->label_count; ++index)
    {
        const auto &label = source->labels[index];
        if (!valid_key(label.key) || !valid_value(label.value))
        {
            return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
        }
        for (std::uint32_t other = 0; other < index; ++other)
        {
            if (std::strcmp(source->labels[other].key, label.key) == 0)
            {
                return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT; /* duplicate key */
            }
        }
    }

    std::unique_ptr<Source> created;
    try
    {
        created = std::make_unique<Source>();
        created->sample = source->sample;
        created->data = source->source_data;
        created->role = source->role;
        created->keys.reserve(source->label_count);
        created->values.reserve(source->label_count);
        for (std::uint32_t index = 0; index < source->label_count; ++index)
        {
            created->keys.emplace_back(source->labels[index].key);
            created->values.emplace_back(source->labels[index].value);
        }
        for (std::size_t index = 0; index < created->keys.size(); ++index)
        {
            created->labels.push_back({created->keys[index].c_str(), created->values[index].c_str()});
        }
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    robotweax_srt_telemetry_window_aggregator *windows = nullptr;
    const auto error =
        robotweax_srt_telemetry_window_aggregator_create(&sampler->config.windows, &windows);
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return error;
    }
    created->windows.reset(windows);

    std::lock_guard lock(sampler->mutex);
    std::string labels;
    try
    {
        created->history.resize(sampler->history_capacity);
        labels = label_text(*created);
        std::vector<std::pair<std::string, std::string>> pairs;
        for (std::size_t l = 0; l < created->keys.size(); ++l)
        {
            pairs.emplace_back(created->keys[l], created->values[l]);
        }
        created->prometheus_labels =
            robotweax_srt_telemetry_internal::prometheus_labels(sampler->next_id, created->role, pairs);
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    if (sampler->health_enabled)
    {
        const auto health_error = create_health(*sampler, *created);
        if (health_error != ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            return health_error;
        }
    }
    const auto id = sampler->next_id;
    created->id = id;
    try
    {
        sampler->sources.emplace(id, std::move(created));
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    sampler->next_id += 1;
    *source_id = id;
    journal_event(*sampler, id, ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_ADDED,
                  ROBOTWEAX_SRT_TELEMETRY_SEVERITY_INFO, system_nano(), steady_nano(), 0, 0,
                  source->role, labels.c_str());
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_sampler_remove_source(robotweax_srt_telemetry_sampler *sampler,
                                              const std::uint64_t source_id)
{
    if (sampler == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    std::lock_guard lock(sampler->mutex);
    const auto found = sampler->sources.find(source_id);
    if (found == sampler->sources.end())
    {
        return ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND;
    }
    flush_locked(*sampler, *found->second);
    sampler->sources.erase(found);
    journal_event(*sampler, source_id, ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_REMOVED,
                  ROBOTWEAX_SRT_TELEMETRY_SEVERITY_INFO, system_nano(), steady_nano(), 0, 0, 0,
                  nullptr);
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" std::size_t
robotweax_srt_telemetry_sampler_source_count(robotweax_srt_telemetry_sampler *sampler)
{
    if (sampler == nullptr || in_callback_of(sampler))
    {
        return 0;
    }
    std::lock_guard lock(sampler->mutex);
    return sampler->sources.size();
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_sampler_tick(robotweax_srt_telemetry_sampler *sampler,
                                     const std::uint64_t observed_at_unix_nano,
                                     const std::uint64_t sample_monotonic_nano)
{
    if (sampler == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    {
        std::lock_guard thread_lock(sampler->thread_mutex);
        if (sampler->running || sampler->stopping)
        {
            return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
        }
    }
    std::lock_guard lock(sampler->mutex);
    tick_locked(*sampler, observed_at_unix_nano, sample_monotonic_nano);
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_sampler_start(robotweax_srt_telemetry_sampler *sampler)
{
    if (sampler == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    std::lock_guard lock(sampler->thread_mutex);
    if (sampler->running || sampler->stopping)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    sampler->stop_requested = false;
    try
    {
        sampler->thread = std::thread([sampler] {
            robotweax_srt_telemetry_internal::name_current_thread("srt-tel-sampler");
            run_thread(*sampler);
        });
    }
    catch (...)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    sampler->running = true;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_sampler_stop(robotweax_srt_telemetry_sampler *sampler)
{
    if (sampler == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    std::thread thread;
    {
        std::lock_guard lock(sampler->thread_mutex);
        if (!sampler->running)
        {
            /* Not running, or another stop already owns the join. */
            return ROBOTWEAX_SRT_TELEMETRY_OK;
        }
        sampler->running = false;
        sampler->stopping = true;
        sampler->stop_requested = true;
        thread = std::move(sampler->thread);
    }
    sampler->wake.notify_all();
    if (thread.joinable())
    {
        try
        {
            thread.join();
        }
        catch (...)
        {
        }
    }
    std::lock_guard lock(sampler->thread_mutex);
    sampler->stopping = false;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_sampler_enable_history(robotweax_srt_telemetry_sampler *sampler,
                                               const std::size_t records_per_source)
{
    if (sampler == nullptr || records_per_source > (std::size_t{1} << 24))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    std::lock_guard lock(sampler->mutex);
    try
    {
        for (auto &entry : sampler->sources)
        {
            auto &source = *entry.second;
            std::vector<robotweax_srt_telemetry_compact_record_v1> ring(records_per_source);
            source.history.swap(ring);
            source.history_head = 0;
            source.history_size = 0;
        }
    }
    catch (const std::bad_alloc &)
    {
        for (auto &entry : sampler->sources)
        {
            entry.second->history.clear();
            entry.second->history.shrink_to_fit();
            entry.second->history_size = 0;
        }
        sampler->history_capacity = 0;
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    sampler->history_capacity = records_per_source;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_history_read(
    robotweax_srt_telemetry_sampler *sampler, const std::uint64_t source_id,
    const std::uint64_t since_start_unix_nano, robotweax_srt_telemetry_compact_record_v1 *records,
    const std::size_t capacity, std::size_t *count)
{
    if (sampler == nullptr || count == nullptr || (records == nullptr && capacity > 0))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *count = 0;
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    std::lock_guard lock(sampler->mutex);
    const auto found = sampler->sources.find(source_id);
    if (found == sampler->sources.end())
    {
        return ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND;
    }
    const auto &source = *found->second;
    std::size_t copied = 0;
    for (std::size_t index = 0; index < source.history_size && copied < capacity; ++index)
    {
        const auto &record = source.history[(source.history_head + index) % source.history.size()];
        if (record.start_unix_nano >= since_start_unix_nano)
        {
            records[copied++] = record;
        }
    }
    *count = copied;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_sampler_enable_journal(robotweax_srt_telemetry_sampler *sampler,
                                               const std::size_t capacity)
{
    if (sampler == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    robotweax_srt_telemetry_event_journal *journal = nullptr;
    if (capacity > 0)
    {
        const auto error = robotweax_srt_telemetry_event_journal_create(capacity, &journal);
        if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            return error;
        }
    }
    std::lock_guard lock(sampler->mutex);
    std::lock_guard journal_lock(sampler->journal_mutex);
    if (sampler->journal)
    {
        sampler->journal_next_sequence =
            robotweax_srt_telemetry_internal::journal_next_sequence(*sampler->journal);
    }
    if (journal != nullptr)
    {
        robotweax_srt_telemetry_internal::journal_continue_from(*journal,
                                                                sampler->journal_next_sequence);
    }
    sampler->journal.reset(journal);
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_journal_read(
    robotweax_srt_telemetry_sampler *sampler, const std::uint64_t after_sequence,
    robotweax_srt_telemetry_event_v1 *events, const std::size_t capacity, std::size_t *count,
    std::uint64_t *overwritten)
{
    if (sampler == nullptr || count == nullptr || (events == nullptr && capacity > 0))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *count = 0;
    std::lock_guard lock(sampler->journal_mutex);
    if (!sampler->journal)
    {
        return ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND;
    }
    if (overwritten != nullptr)
    {
        *overwritten = robotweax_srt_telemetry_event_journal_overwritten_count(sampler->journal.get());
    }
    return robotweax_srt_telemetry_event_journal_read(sampler->journal.get(), after_sequence, events,
                                                      capacity, count);
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_sampler_record_event(robotweax_srt_telemetry_sampler *sampler,
                                             const robotweax_srt_telemetry_event_v1 *event)
{
    if (sampler == nullptr || event == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    std::lock_guard lock(sampler->journal_mutex);
    if (!sampler->journal)
    {
        return ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND;
    }
    return robotweax_srt_telemetry_event_journal_push(sampler->journal.get(), event, nullptr);
}

/* ------------------------------------------------------------------ health */

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_sampler_enable_health(robotweax_srt_telemetry_sampler *sampler, const int enabled)
{
    if (sampler == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    std::lock_guard lock(sampler->mutex);
    if (enabled == 0)
    {
        for (auto &entry : sampler->sources)
        {
            entry.second->health.reset();
            entry.second->has_health_status = false;
            entry.second->window_has_health = false;
        }
        sampler->health_enabled = false;
        return ROBOTWEAX_SRT_TELEMETRY_OK;
    }
    for (auto &entry : sampler->sources)
    {
        if (!entry.second->health)
        {
            const auto error = create_health(*sampler, *entry.second);
            if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
            {
                for (auto &other : sampler->sources)
                {
                    other.second->health.reset();
                    other.second->has_health_status = false;
                }
                sampler->health_enabled = false;
                return error;
            }
        }
    }
    sampler->health_enabled = true;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_set_health_callback(
    robotweax_srt_telemetry_sampler *sampler, const robotweax_srt_telemetry_health_callback callback,
    void *user_data)
{
    if (sampler == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    std::lock_guard lock(sampler->mutex);
    sampler->health_callback = callback;
    sampler->health_user_data = user_data;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_set_health_thresholds(
    robotweax_srt_telemetry_sampler *sampler, const robotweax_srt_telemetry_endpoint_role role,
    const robotweax_srt_telemetry_health_indicator indicator, const double watch, const double at_risk,
    const double failing)
{
    const auto role_index = static_cast<std::int64_t>(role);
    const auto indicator_index = static_cast<std::int64_t>(indicator);
    if (sampler == nullptr || role_index < ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER ||
        role_index > ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER || indicator_index < 0 ||
        indicator_index >= ROBOTWEAX_SRT_TELEMETRY_INDICATOR_COUNT)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    /* Validate the ordering with a scratch engine of that role. */
    robotweax_srt_telemetry_health_config_v1 config;
    robotweax_srt_telemetry_health_config_default(role, &config);
    robotweax_srt_telemetry_health_engine *scratch = nullptr;
    auto error = robotweax_srt_telemetry_health_engine_create(&config, &scratch);
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return error;
    }
    error = robotweax_srt_telemetry_health_engine_set_thresholds(scratch, indicator, watch, at_risk, failing);
    robotweax_srt_telemetry_health_engine_destroy(scratch);
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return error;
    }
    std::lock_guard lock(sampler->mutex);
    sampler->health_thresholds[static_cast<std::size_t>(role_index)][static_cast<std::size_t>(indicator_index)] = {
        true, watch, at_risk, failing};
    for (auto &entry : sampler->sources)
    {
        if (entry.second->health && entry.second->role == role)
        {
            static_cast<void>(robotweax_srt_telemetry_health_engine_set_thresholds(
                entry.second->health.get(), indicator, watch, at_risk, failing));
        }
    }
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_set_health_context(
    robotweax_srt_telemetry_sampler *sampler, const std::uint64_t source_id,
    const robotweax_srt_telemetry_health_context_v1 *context)
{
    if (sampler == nullptr || context == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    /* Validate with a scratch engine so the rules live in one place. */
    robotweax_srt_telemetry_health_config_v1 config;
    robotweax_srt_telemetry_health_config_default(ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_UNKNOWN, &config);
    robotweax_srt_telemetry_health_engine *scratch = nullptr;
    auto error = robotweax_srt_telemetry_health_engine_create(&config, &scratch);
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return error;
    }
    error = robotweax_srt_telemetry_health_engine_set_context(scratch, context);
    robotweax_srt_telemetry_health_engine_destroy(scratch);
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return error;
    }
    std::lock_guard lock(sampler->mutex);
    const auto found = sampler->sources.find(source_id);
    if (found == sampler->sources.end())
    {
        return ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND;
    }
    auto &source = *found->second;
    source.health_context = *context;
    source.health_context.struct_size = sizeof(source.health_context);
    source.has_health_context = true;
    if (source.health)
    {
        static_cast<void>(robotweax_srt_telemetry_health_engine_set_context(source.health.get(), &source.health_context));
    }
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_set_health_group(
    robotweax_srt_telemetry_sampler *sampler, const std::uint64_t source_id,
    const robotweax_srt_telemetry_health_group_v1 *group)
{
    if (sampler == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    if (group != nullptr)
    {
        const auto error = robotweax_srt_telemetry_internal::validate_health_group(*group);
        if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            return error;
        }
    }
    std::lock_guard lock(sampler->mutex);
    const auto found = sampler->sources.find(source_id);
    if (found == sampler->sources.end())
    {
        return ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND;
    }
    auto &source = *found->second;
    source.has_health_group = group != nullptr;
    if (group != nullptr)
    {
        source.health_group = *group;
        source.health_group.struct_size = sizeof(source.health_group);
    }
    if (source.health)
    {
        static_cast<void>(robotweax_srt_telemetry_health_engine_set_group(
            source.health.get(), source.has_health_group ? &source.health_group : nullptr));
    }
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_sampler_health_read(robotweax_srt_telemetry_sampler *sampler, const std::uint64_t source_id,
                                            robotweax_srt_telemetry_health_status_v1 *status)
{
    if (sampler == nullptr || status == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    std::lock_guard lock(sampler->mutex);
    const auto found = sampler->sources.find(source_id);
    if (found == sampler->sources.end())
    {
        return ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND;
    }
    if (!sampler->health_enabled)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    const auto &source = *found->second;
    if (source.has_health_status)
    {
        *status = source.health_status;
        return ROBOTWEAX_SRT_TELEMETRY_OK;
    }
    std::memset(status, 0, sizeof(*status));
    status->struct_size = sizeof(*status);
    status->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    status->verdict = ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN;
    status->reason = ROBOTWEAX_SRT_TELEMETRY_REASON_NO_DATA;
    status->primary_indicator = -1;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

/* ------------------------------------------------------------- snapshots */

namespace
{

/* Index of a configured window; window_nano 0 picks 10 s if configured,
 * else the shortest. */
bool window_index(const robotweax_srt_telemetry_sampler &sampler, const std::uint64_t window_nano, std::size_t &index)
{
    const auto &w = sampler.config.windows;
    const auto count = std::min<std::uint32_t>(w.window_count, ROBOTWEAX_SRT_TELEMETRY_MAX_WINDOWS);
    if (window_nano != 0)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            if (w.window_nano[i] == window_nano)
            {
                index = i;
                return true;
            }
        }
        return false;
    }
    bool found = false;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (w.window_nano[i] == UINT64_C(10'000'000'000))
        {
            index = i;
            return true;
        }
        if (!found || w.window_nano[i] < w.window_nano[index])
        {
            index = i;
            found = true;
        }
    }
    return found;
}

} // namespace

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_source_ids(
    robotweax_srt_telemetry_sampler *sampler, std::uint64_t *ids, const std::size_t capacity, std::size_t *count)
{
    if (sampler == nullptr || count == nullptr || (ids == nullptr && capacity > 0))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *count = 0;
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    std::lock_guard lock(sampler->mutex);
    std::size_t copied = 0;
    for (const auto &entry : sampler->sources)
    {
        if (copied == capacity)
        {
            break;
        }
        ids[copied++] = entry.first;
    }
    *count = copied;
    return copied < sampler->sources.size() ? ROBOTWEAX_SRT_TELEMETRY_BUFFER_TOO_SMALL : ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_source_labels(
    robotweax_srt_telemetry_sampler *sampler, const std::uint64_t source_id, robotweax_srt_telemetry_endpoint_role *role,
    robotweax_srt_telemetry_label_copy_v1 *labels, const std::size_t capacity, std::size_t *count)
{
    if (sampler == nullptr || count == nullptr || (labels == nullptr && capacity > 0))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *count = 0;
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    std::lock_guard lock(sampler->mutex);
    const auto found = sampler->sources.find(source_id);
    if (found == sampler->sources.end())
    {
        return ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND;
    }
    const auto &source = *found->second;
    if (role != nullptr)
    {
        *role = source.role;
    }
    std::size_t copied = 0;
    for (; copied < capacity && copied < source.keys.size(); ++copied)
    {
        /* Validated at add_source: both fit their capacity with the NUL. */
        std::snprintf(labels[copied].key, sizeof(labels[copied].key), "%s", source.keys[copied].c_str());
        std::snprintf(labels[copied].value, sizeof(labels[copied].value), "%s", source.values[copied].c_str());
    }
    *count = copied;
    return copied < source.keys.size() ? ROBOTWEAX_SRT_TELEMETRY_BUFFER_TOO_SMALL : ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_window_read(
    robotweax_srt_telemetry_sampler *sampler, const std::uint64_t source_id, const std::uint64_t window_nano,
    robotweax_srt_telemetry_window_summary_v1 *summary)
{
    if (sampler == nullptr || summary == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    std::lock_guard lock(sampler->mutex);
    std::size_t index = 0;
    if (!window_index(*sampler, window_nano, index))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    const auto found = sampler->sources.find(source_id);
    if (found == sampler->sources.end() || !found->second->has_window[index])
    {
        return ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND;
    }
    *summary = found->second->latest_window[index];
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_latest_metrics(
    robotweax_srt_telemetry_sampler *sampler, const std::uint64_t source_id,
    robotweax_srt_telemetry_metric_sample_v1 *metrics)
{
    if (sampler == nullptr || metrics == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    std::lock_guard lock(sampler->mutex);
    const auto found = sampler->sources.find(source_id);
    if (found == sampler->sources.end() || !found->second->has_latest_metrics)
    {
        return ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND;
    }
    *metrics = found->second->latest_metrics;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

namespace
{

robotweax_srt_telemetry_error render_impl(robotweax_srt_telemetry_sampler *sampler,
                                          const robotweax_srt_telemetry_render_options_v1 *options,
                                          std::vector<std::pair<std::string, std::string>> resource,
                                          const robotweax_srt_telemetry_render_writer writer, void *user_data)
{
    if (sampler == nullptr || options == nullptr || writer == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (options->struct_size < sizeof(*options) || options->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    const auto format = static_cast<std::int64_t>(options->format);
    constexpr std::uint32_t known_flags =
        ROBOTWEAX_SRT_TELEMETRY_RENDER_NO_INDICATORS | ROBOTWEAX_SRT_TELEMETRY_RENDER_NO_COUNTERS;
    if ((format != ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS && format != ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON &&
         format != ROBOTWEAX_SRT_TELEMETRY_RENDER_OTLP_JSON) ||
        (options->flags & ~known_flags) != 0)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (in_callback_of(sampler))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    std::string text;
    try
    {
        robotweax_srt_telemetry_internal::ExportView view;
        view.flags = options->flags;
        view.resource = std::move(resource);
        {
            std::lock_guard lock(sampler->mutex);
            std::size_t index = 0;
            if (!window_index(*sampler, options->window_nano, index))
            {
                return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
            }
            view.window_nano = sampler->config.windows.window_nano[index];
            const auto now_steady = steady_nano();
            view.generated_unix_nano = sampler->last_tick_unix != 0 ? sampler->last_tick_unix : system_nano();
            view.sources.reserve(sampler->sources.size());
            for (const auto &entry : sampler->sources)
            {
                const auto &source = *entry.second;
                robotweax_srt_telemetry_internal::ExportSource out;
                out.id = source.id;
                out.role = source.role;
                for (std::size_t l = 0; l < source.keys.size(); ++l)
                {
                    out.labels.emplace_back(source.keys[l], source.values[l]);
                }
                out.prometheus_labels = source.prometheus_labels;
                out.failed = source.failed;
                out.has_window = source.has_window[index];
                if (out.has_window)
                {
                    out.window = source.latest_window[index];
                    /* No newer window for two more lengths: the source no
                     * longer ticks. Steady clock, so independent of host
                     * timestamps and clock steps. */
                    out.window_stale = now_steady > source.window_delivered_steady[index] + 3 * view.window_nano;
                }
                out.has_metrics = source.has_latest_metrics;
                out.metrics = source.latest_metrics;
                out.has_raw = source.has_latest_raw;
                out.raw = source.latest_raw;
                out.standby_rtt_metadata = source.standby_rtt_metadata;
                out.standby_rtt_capture_age_seconds =
                    static_cast<double>(now_steady - source.standby_rtt_updated_steady) / 1e9;
                out.rtt_metadata = source.rtt_metadata;
                out.rtt_capture_age_seconds =
                    static_cast<double>(now_steady - source.rtt_updated_steady) / 1e9;
                out.group_send_metadata = source.group_send_metadata;
                out.group_send_capture_age_seconds =
                    static_cast<double>(now_steady - source.group_send_updated_steady) / 1e9;
                out.has_ext = source.has_latest_ext;
                out.ext = source.latest_ext;
                out.has_health = source.health != nullptr && source.has_health_status;
                out.health = source.health_status;
                out.connection = source.has_health_context ? source.health_context.connection
                                                           : ROBOTWEAX_SRT_TELEMETRY_CONNECTION_UNREPORTED;
                out.counters_start_unix_nano = source.counters_start_unix_nano;
                view.sources.push_back(std::move(out));
            }
        }
        text = format == ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS ? robotweax_srt_telemetry_internal::render_prometheus(view)
               : format == ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON  ? robotweax_srt_telemetry_internal::render_json(view)
                                                               : robotweax_srt_telemetry_internal::render_otlp_json(view);
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    /* The writer runs without the sampler lock: it may call the sampler. */
    try
    {
        return writer(user_data, text.data(), text.size()) == 0 ? ROBOTWEAX_SRT_TELEMETRY_OK
                                                                : ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE;
    }
    catch (...)
    {
        return ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE;
    }
}

namespace
{

struct BufferWriter
{
    char *buffer;
    std::size_t capacity;
    std::size_t length = 0;
};

int buffer_write(void *user, const char *data, const std::size_t size)
{
    auto &w = *static_cast<BufferWriter *>(user);
    if (w.buffer != nullptr && w.length + size < w.capacity)
    {
        std::memcpy(w.buffer + w.length, data, size);
    }
    w.length += size;
    return 0;
}

} // namespace

} // namespace

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_render_to(
    robotweax_srt_telemetry_sampler *sampler, const robotweax_srt_telemetry_render_options_v1 *options,
    const robotweax_srt_telemetry_render_writer writer, void *user_data)
{
    return render_impl(sampler, options, {}, writer, user_data);
}

namespace
{
/* Well-formed UTF-8 without control characters: OTLP/JSON strings must be
 * valid UTF-8, and a resource attribute has no use for control characters. */
bool utf8_text(const char *text)
{
    const auto *p = reinterpret_cast<const unsigned char *>(text);
    while (*p != 0)
    {
        const unsigned c = *p;
        if (c < 0x20 || c == 0x7f)
        {
            return false;
        }
        int extra = 0;
        std::uint32_t code = 0;
        if (c < 0x80)
        {
            ++p;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf)
        {
            extra = 1;
            code = c & 0x1f;
        }
        else if (c >= 0xe0 && c <= 0xef)
        {
            extra = 2;
            code = c & 0x0f;
        }
        else if (c >= 0xf0 && c <= 0xf4)
        {
            extra = 3;
            code = c & 0x07;
        }
        else
        {
            return false;
        }
        ++p;
        for (int k = 0; k < extra; ++k, ++p)
        {
            if ((*p & 0xc0) != 0x80)
            {
                return false;
            }
            code = (code << 6) | (*p & 0x3f);
        }
        /* Overlong forms, surrogates, beyond U+10FFFF, and C1 controls. */
        if ((extra == 2 && code < 0x800) || (extra == 3 && (code < 0x10000 || code > 0x10ffff)) ||
            (code >= 0xd800 && code <= 0xdfff) || (code >= 0x80 && code <= 0x9f))
        {
            return false;
        }
    }
    return true;
}
} // namespace

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_render_otlp_to(
    robotweax_srt_telemetry_sampler *sampler, const robotweax_srt_telemetry_render_options_v1 *options,
    const robotweax_srt_telemetry_label_v1 *resource_attributes, const std::size_t resource_attribute_count,
    const robotweax_srt_telemetry_render_writer writer, void *user_data)
{
    if (options == nullptr || options->format != ROBOTWEAX_SRT_TELEMETRY_RENDER_OTLP_JSON ||
        resource_attribute_count > 32 || (resource_attribute_count > 0 && resource_attributes == nullptr))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    std::vector<std::pair<std::string, std::string>> resource;
    try
    {
        for (std::size_t i = 0; i < resource_attribute_count; ++i)
        {
            const auto &a = resource_attributes[i];
            if (a.key == nullptr || a.value == nullptr || a.key[0] == '\0' || strnlen(a.key, 256) > 255 ||
                strnlen(a.value, 256) > 255 || !utf8_text(a.key) || !utf8_text(a.value))
            {
                return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
            }
            for (const auto &[key, value] : resource)
            {
                if (key == a.key)
                {
                    return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT; /* duplicate key */
                }
            }
            resource.emplace_back(a.key, a.value);
        }
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    return render_impl(sampler, options, std::move(resource), writer, user_data);
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_render(
    robotweax_srt_telemetry_sampler *sampler, const robotweax_srt_telemetry_render_options_v1 *options, char *buffer,
    const std::size_t capacity, std::size_t *length)
{
    if (length == nullptr || (buffer == nullptr && capacity > 0))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *length = 0;
    BufferWriter writer{buffer, capacity};
    const auto error = robotweax_srt_telemetry_sampler_render_to(sampler, options, &buffer_write, &writer);
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return error;
    }
    *length = writer.length;
    if (writer.length + 1 > capacity)
    {
        return ROBOTWEAX_SRT_TELEMETRY_BUFFER_TOO_SMALL;
    }
    buffer[writer.length] = '\0';
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_set_rtt_metadata_v1(
    robotweax_srt_telemetry_sampler *sampler, const std::uint64_t source_id,
    const robotweax_srt_telemetry_rtt_metadata_v1 *metadata)
{
    if (sampler == nullptr)
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    if (in_callback_of(sampler))
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    if (metadata != nullptr &&
        (metadata->struct_size < sizeof(*metadata) ||
         metadata->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION ||
         metadata->availability > 2 || metadata->flags > 7 || metadata->last_update_source > 2 ||
         metadata->reserved[0] != 0 || metadata->reserved[1] != 0 ||
         (metadata->availability != 2 &&
          (metadata->flags != 0 || metadata->last_update_source != 0)) ||
         (metadata->availability == 2 &&
          (metadata->native_result != 0 ||
           ((metadata->flags & 1) == 0 &&
            (metadata->flags != 0 || metadata->last_update_source != 0)) ||
           ((metadata->flags & 1) != 0 && metadata->last_update_source == 0) ||
           (((metadata->flags & 2) != 0) !=
            (metadata->local_ackack_age_microseconds != UINT64_MAX)) ||
           (((metadata->flags & 4) != 0) !=
            (metadata->peer_estimate_age_microseconds != UINT64_MAX)) ||
           (metadata->last_update_source == 1 && (metadata->flags & 2) == 0) ||
           (metadata->last_update_source == 2 && (metadata->flags & 4) == 0)))))
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    std::lock_guard lock(sampler->mutex);
    const auto found = sampler->sources.find(source_id);
    if (found == sampler->sources.end())
        return ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND;
    found->second->rtt_metadata =
        metadata == nullptr ? robotweax_srt_telemetry_rtt_metadata_v1{} : *metadata;
    found->second->rtt_updated_steady = steady_nano();
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_sampler_set_group_send_metadata_v1(
    robotweax_srt_telemetry_sampler *sampler, const std::uint64_t source_id,
    const robotweax_srt_telemetry_group_send_metadata_v1 *metadata)
{
    if (sampler == nullptr)
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    if (in_callback_of(sampler))
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    if (metadata != nullptr &&
        (metadata->struct_size < sizeof(*metadata) ||
         metadata->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION ||
         metadata->availability > 2 || metadata->state > 1 || metadata->reserved != 0 ||
         std::memchr(metadata->active_path, 0, sizeof(metadata->active_path)) == nullptr ||
         std::memchr(metadata->probe_path, 0, sizeof(metadata->probe_path)) == nullptr ||
         (metadata->availability != 2 &&
          (metadata->state != 0 || metadata->group_generation != 0 ||
           metadata->active_generation != 0 || metadata->probe_generation != 0 ||
           metadata->authority_revision != 0 || metadata->active_path[0] ||
           metadata->probe_path[0])) ||
         (metadata->availability == 2 &&
          (metadata->native_result != 0 || metadata->group_generation == 0 ||
           (metadata->state == 1) != (metadata->active_member >= 0) ||
           (metadata->active_member >= 0) != (metadata->active_generation != 0) ||
           (metadata->probe_member >= 0) != (metadata->probe_generation != 0) ||
           (metadata->active_member >= 0) != (metadata->active_age_microseconds != UINT64_MAX) ||
           (metadata->probe_member >= 0) != (metadata->probe_age_microseconds != UINT64_MAX) ||
           metadata->active_member < -1 || metadata->probe_member < -1))))
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    std::lock_guard lock(sampler->mutex);
    const auto found = sampler->sources.find(source_id);
    if (found == sampler->sources.end())
        return ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND;
    found->second->group_send_metadata =
        metadata == nullptr ? robotweax_srt_telemetry_group_send_metadata_v1{} : *metadata;
    found->second->group_send_updated_steady = steady_nano();
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_sampler_set_standby_rtt_metadata_v1(
    robotweax_srt_telemetry_sampler *sampler, const std::uint64_t source_id,
    const robotweax_srt_telemetry_standby_rtt_metadata_v1 *metadata)
{
    if (sampler == nullptr)
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    if (in_callback_of(sampler))
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    if (metadata != nullptr && !robotweax_srt_telemetry_internal::valid_standby_metadata(*metadata))
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    std::lock_guard lock(sampler->mutex);
    const auto found = sampler->sources.find(source_id);
    if (found == sampler->sources.end())
        return ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND;
    found->second->standby_rtt_metadata =
        metadata == nullptr ? robotweax_srt_telemetry_standby_rtt_metadata_v1{} : *metadata;
    found->second->standby_rtt_updated_steady = steady_nano();
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}
