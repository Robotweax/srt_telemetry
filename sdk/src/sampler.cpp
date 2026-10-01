#include <robotweax/srt_telemetry_sampler.h>

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
    robotweax_srt_telemetry_source_fn sample = nullptr;
    void *data = nullptr;
    robotweax_srt_telemetry_endpoint_role role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_UNKNOWN;
    std::vector<std::string> keys;
    std::vector<std::string> values;
    std::vector<robotweax_srt_telemetry_label_v1> labels;
    std::unique_ptr<robotweax_srt_telemetry_window_aggregator, AggregatorDeleter> windows;

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

/* Set while this thread runs sampler callbacks, to refuse re-entrant calls
 * that would otherwise deadlock on the sampler mutex. */
thread_local const void *callback_owner = nullptr;

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

struct robotweax_srt_telemetry_sampler
{
    robotweax_srt_telemetry_sampler_config_v1 config{};
    std::mutex mutex; /* sources, tick execution */
    std::map<std::uint64_t, std::unique_ptr<Source>> sources;
    std::uint64_t next_id = 1;

    std::mutex thread_mutex; /* thread lifecycle */
    std::condition_variable wake;
    bool stop_requested = false;
    bool running = false;
    std::thread thread;
};

namespace
{

struct WindowContext
{
    robotweax_srt_telemetry_sampler *sampler;
    const Source *source;
};

int deliver_window(void *user_data, const robotweax_srt_telemetry_window_summary_v1 *summary)
{
    const auto &context = *static_cast<WindowContext *>(user_data);
    const auto &callbacks = context.sampler->config.callbacks;
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

void report_error(robotweax_srt_telemetry_sampler &sampler, const Source &source,
                  const robotweax_srt_telemetry_error error, const std::int32_t native)
{
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
        report_error(sampler, source, error, output.native_result);
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
        report_error(sampler, source, error, output.native_result);
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
    callback_owner = &sampler;
    for (auto &entry : sampler.sources)
    {
        sample_source(sampler, *entry.second, unix_nano, monotonic_nano);
    }
    callback_owner = nullptr;
}

/* Caller holds sampler.mutex. */
void flush_locked(robotweax_srt_telemetry_sampler &sampler, Source &source)
{
    callback_owner = &sampler;
    WindowContext context{&sampler, &source};
    static_cast<void>(robotweax_srt_telemetry_window_aggregator_flush(source.windows.get(),
                                                                      &deliver_window, &context));
    callback_owner = nullptr;
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
    *sampler = created;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" void robotweax_srt_telemetry_sampler_destroy(robotweax_srt_telemetry_sampler *sampler)
{
    if (sampler == nullptr || callback_owner == sampler)
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
    if (callback_owner == sampler)
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
    created->id = sampler->next_id++;
    *source_id = created->id;
    try
    {
        sampler->sources.emplace(created->id, std::move(created));
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
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
    if (callback_owner == sampler)
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
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" std::size_t
robotweax_srt_telemetry_sampler_source_count(robotweax_srt_telemetry_sampler *sampler)
{
    if (sampler == nullptr || callback_owner == sampler)
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
    if (callback_owner == sampler)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    {
        std::lock_guard thread_lock(sampler->thread_mutex);
        if (sampler->running)
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
    if (callback_owner == sampler)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    std::lock_guard lock(sampler->thread_mutex);
    if (sampler->running)
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
    if (callback_owner == sampler)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    }
    std::thread thread;
    {
        std::lock_guard lock(sampler->thread_mutex);
        if (!sampler->running)
        {
            return ROBOTWEAX_SRT_TELEMETRY_OK;
        }
        sampler->stop_requested = true;
        thread = std::move(sampler->thread);
    }
    sampler->wake.notify_all();
    thread.join();
    std::lock_guard lock(sampler->thread_mutex);
    sampler->running = false;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}
