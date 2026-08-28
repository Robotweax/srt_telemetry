#include <robotweax/srt_telemetry.h>

#include <limits>
#include <mutex>
#include <new>
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
