#include "counter_engine.hpp"
#include <new>
#include <robotweax/srt_telemetry.h>
namespace numeric = robotweax_srt_telemetry_internal::counters;
struct robotweax_srt_telemetry_delta_engine
{
    numeric::State state;
};
namespace
{
template <class T> bool valid(const T *value)
{
    return value && value->struct_size >= sizeof(T) &&
           value->abi_version == ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
}
} // namespace
extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_delta_engine_create(robotweax_srt_telemetry_delta_engine **engine)
{
    if (!engine)
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    *engine = new (std::nothrow) robotweax_srt_telemetry_delta_engine{};
    return *engine ? ROBOTWEAX_SRT_TELEMETRY_OK : ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
}
extern "C" void
robotweax_srt_telemetry_delta_engine_destroy(robotweax_srt_telemetry_delta_engine *engine)
{
    delete engine;
}
extern "C" void
robotweax_srt_telemetry_delta_engine_reset(robotweax_srt_telemetry_delta_engine *engine)
{
    if (engine)
        *engine = {};
}
extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_delta_engine_update(robotweax_srt_telemetry_delta_engine *engine,
                                            const robotweax_srt_telemetry_raw_sample_v1 *sample,
                                            robotweax_srt_telemetry_delta_v1 *delta)
{
    if (!engine || !sample || !delta)
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    if (!valid(sample) || !valid(delta))
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    numeric::update(
        engine->state,
        numeric::point(*sample, static_cast<int>(sample->provider), sample->sample_monotonic_nano),
        nullptr, *delta);
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}
extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_delta_engine_update_ext(
    robotweax_srt_telemetry_delta_engine *engine,
    const robotweax_srt_telemetry_raw_sample_v1 *sample,
    const robotweax_srt_telemetry_ext_sample_v1 *ext_sample,
    robotweax_srt_telemetry_delta_v1 *delta, robotweax_srt_telemetry_ext_delta_v1 *ext_delta)
{
    if (!engine || !sample || !ext_sample || !delta || !ext_delta)
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    if (!valid(sample) || !valid(delta) || !valid(ext_sample) || !valid(ext_delta))
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    const auto ext = numeric::ext_point(*ext_sample);
    numeric::update(
        engine->state,
        numeric::point(*sample, static_cast<int>(sample->provider), sample->sample_monotonic_nano),
        &ext, *delta, ext_delta);
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}
