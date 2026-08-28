#ifndef ROBOTWEAX_SRT_TELEMETRY_HPP
#define ROBOTWEAX_SRT_TELEMETRY_HPP

#include <robotweax/srt_telemetry.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace robotweax::srt::telemetry
{

class DeltaEngine final
{
  public:
    DeltaEngine()
    {
        const auto error = robotweax_srt_telemetry_delta_engine_create(&engine_);
        if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            throw std::runtime_error(robotweax_srt_telemetry_error_string(error));
        }
    }

    ~DeltaEngine()
    {
        robotweax_srt_telemetry_delta_engine_destroy(engine_);
    }

    DeltaEngine(const DeltaEngine &) = delete;
    DeltaEngine &operator=(const DeltaEngine &) = delete;

    DeltaEngine(DeltaEngine &&other) noexcept : engine_(std::exchange(other.engine_, nullptr)) {}

    DeltaEngine &operator=(DeltaEngine &&other) noexcept
    {
        if (this != &other)
        {
            robotweax_srt_telemetry_delta_engine_destroy(engine_);
            engine_ = std::exchange(other.engine_, nullptr);
        }
        return *this;
    }

    void reset() noexcept
    {
        robotweax_srt_telemetry_delta_engine_reset(engine_);
    }

    [[nodiscard]] robotweax_srt_telemetry_delta_v1
    update(const robotweax_srt_telemetry_raw_sample_v1 &sample)
    {
        robotweax_srt_telemetry_delta_v1 delta{};
        delta.struct_size = sizeof(delta);
        delta.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        const auto error = robotweax_srt_telemetry_delta_engine_update(engine_, &sample, &delta);
        if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            throw std::runtime_error(robotweax_srt_telemetry_error_string(error));
        }
        return delta;
    }

  private:
    robotweax_srt_telemetry_delta_engine *engine_ = nullptr;
};

class BoundedRecorder final
{
  public:
    explicit BoundedRecorder(const std::size_t capacity)
    {
        const auto error = robotweax_srt_telemetry_recorder_create(capacity, &recorder_);
        if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            throw std::runtime_error(robotweax_srt_telemetry_error_string(error));
        }
    }

    ~BoundedRecorder()
    {
        robotweax_srt_telemetry_recorder_destroy(recorder_);
    }

    BoundedRecorder(const BoundedRecorder &) = delete;
    BoundedRecorder &operator=(const BoundedRecorder &) = delete;

    BoundedRecorder(BoundedRecorder &&other) noexcept
        : recorder_(std::exchange(other.recorder_, nullptr))
    {
    }

    BoundedRecorder &operator=(BoundedRecorder &&other) noexcept
    {
        if (this != &other)
        {
            robotweax_srt_telemetry_recorder_destroy(recorder_);
            recorder_ = std::exchange(other.recorder_, nullptr);
        }
        return *this;
    }

    void push(const robotweax_srt_telemetry_raw_sample_v1 &raw,
              const robotweax_srt_telemetry_delta_v1 &delta)
    {
        const auto error = robotweax_srt_telemetry_recorder_push(recorder_, &raw, &delta);
        if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            throw std::runtime_error(robotweax_srt_telemetry_error_string(error));
        }
    }

    [[nodiscard]] robotweax_srt_telemetry_record_v1 read(const std::size_t index) const
    {
        robotweax_srt_telemetry_record_v1 record{};
        record.struct_size = sizeof(record);
        record.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        const auto error = robotweax_srt_telemetry_recorder_read(recorder_, index, &record);
        if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            throw std::runtime_error(robotweax_srt_telemetry_error_string(error));
        }
        return record;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return robotweax_srt_telemetry_recorder_size(recorder_);
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return robotweax_srt_telemetry_recorder_capacity(recorder_);
    }

    [[nodiscard]] std::uint64_t overwritten_count() const noexcept
    {
        return robotweax_srt_telemetry_recorder_overwritten_count(recorder_);
    }

    void clear() noexcept
    {
        robotweax_srt_telemetry_recorder_clear(recorder_);
    }

  private:
    robotweax_srt_telemetry_recorder *recorder_ = nullptr;
};

} // namespace robotweax::srt::telemetry

#endif
