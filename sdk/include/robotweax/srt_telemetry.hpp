#ifndef ROBOTWEAX_SRT_TELEMETRY_HPP
#define ROBOTWEAX_SRT_TELEMETRY_HPP

#include <robotweax/srt_telemetry.h>

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

} // namespace robotweax::srt::telemetry

#endif
