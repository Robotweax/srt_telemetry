#include <robotweax/srt_telemetry_sampler.h>

#include <algorithm>
#include <chrono>
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
    /* Failure state for SOURCE_FAILED / SOURCE_RECOVERED events. */
    bool failed = false;
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
    /* Swapped under `mutex`; record_event reads it under journal_mutex. */
    std::mutex journal_mutex;
    std::unique_ptr<robotweax_srt_telemetry_event_journal, JournalDeleter> journal;
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
        std::strncpy(event.detail, detail, sizeof(event.detail) - 1);
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
};

int deliver_window(void *user_data, const robotweax_srt_telemetry_window_summary_v1 *summary)
{
    const auto &context = *static_cast<WindowContext *>(user_data);
    auto &sampler = *context.sampler;
    if (summary->window_nano == sampler.history_window_nano)
    {
        auto &source = *context.source;
        if (!source.history.empty())
        {
            robotweax_srt_telemetry_compact_record_v1 record{};
            if (robotweax_srt_telemetry_compact_record_from_window(summary, &record) ==
                ROBOTWEAX_SRT_TELEMETRY_OK)
            {
                source.history_push(record);
            }
        }
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
        for (const auto &flag : flags)
        {
            if ((summary->quality_flags & flag.bit) != 0)
            {
                journal_event(sampler, context.source->id, flag.kind,
                              ROBOTWEAX_SRT_TELEMETRY_SEVERITY_WARNING, summary->start_unix_nano, 0,
                              0, 0, 0, flag.detail);
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
        if (!source.failed || error != source.last_error)
        {
            journal_event(sampler, source.id, ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_FAILED,
                          ROBOTWEAX_SRT_TELEMETRY_SEVERITY_ERROR, unix_nano, monotonic_nano,
                          source.failed ? source.last_error : ROBOTWEAX_SRT_TELEMETRY_OK, error,
                          output.native_result, robotweax_srt_telemetry_error_string(error));
        }
        source.failed = true;
        report_error(sampler, source, error, output.native_result, monotonic_nano);
        /* Keep the windows moving: an empty sample marked SOURCE_ERROR closes
         * windows on time, so an outage shows as flagged windows, not silence. */
        robotweax_srt_telemetry_metric_sample_v1 failed{};
        failed.struct_size = sizeof(failed);
        failed.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        failed.observed_at_unix_nano = unix_nano;
        failed.sample_monotonic_nano = monotonic_nano;
        failed.quality_flags = ROBOTWEAX_SRT_TELEMETRY_METRIC_SAMPLE_SOURCE_ERROR;
        WindowContext context{&sampler, &source};
        static_cast<void>(robotweax_srt_telemetry_window_aggregator_push(
            source.windows.get(), &failed, &deliver_window, &context));
        return;
    }
    if (source.failed)
    {
        journal_event(sampler, source.id, ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_RECOVERED,
                      ROBOTWEAX_SRT_TELEMETRY_SEVERITY_INFO, unix_nano, monotonic_nano,
                      source.last_error, ROBOTWEAX_SRT_TELEMETRY_OK, 0, "source sampling again");
        source.failed = false;
    }
    source.last_error = ROBOTWEAX_SRT_TELEMETRY_OK;

    robotweax_srt_telemetry_metric_sample_v1 metrics{};
    metrics.struct_size = sizeof(metrics);
    metrics.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    const auto has_ext = output.has_ext != 0;
    error = robotweax_srt_telemetry_metrics_compute(&output.raw, has_ext ? &output.ext : nullptr,
                                                    &output.delta,
                                                    has_ext ? &output.ext_delta : nullptr, &metrics);
    if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        report_error(sampler, source, error, output.native_result, monotonic_nano);
        return;
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
    WindowContext context{&sampler, &source};
    static_cast<void>(robotweax_srt_telemetry_window_aggregator_push(
        source.windows.get(), &metrics, &deliver_window, &context));
}

/* Caller holds sampler.mutex. */
void tick_locked(robotweax_srt_telemetry_sampler &sampler, const std::uint64_t unix_nano,
                 const std::uint64_t monotonic_nano)
{
    const CallbackScope scope(&sampler);
    for (auto &entry : sampler.sources)
    {
        sample_source(sampler, *entry.second, unix_nano, monotonic_nano);
    }
}

/* Caller holds sampler.mutex. */
void flush_locked(robotweax_srt_telemetry_sampler &sampler, Source &source)
{
    const CallbackScope scope(&sampler);
    WindowContext context{&sampler, &source};
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
    if (source->sample == nullptr || source->label_count > ROBOTWEAX_SRT_TELEMETRY_MAX_LABELS ||
        (source->label_count > 0 && source->labels == nullptr))
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
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
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
        sampler->thread = std::thread([sampler] { run_thread(*sampler); });
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
    sampler->journal.reset(journal);
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_event_journal *
robotweax_srt_telemetry_sampler_journal(robotweax_srt_telemetry_sampler *sampler)
{
    if (sampler == nullptr)
    {
        return nullptr;
    }
    std::lock_guard lock(sampler->journal_mutex);
    return sampler->journal.get();
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
