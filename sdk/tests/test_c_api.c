#include <robotweax/srt_telemetry.h>

#include <stdint.h>
#include <string.h>

int main(void)
{
    robotweax_srt_telemetry_delta_engine *engine = NULL;
    if (strcmp(robotweax_srt_telemetry_version(), "0.1.0-alpha.0") != 0)
    {
        return 1;
    }
    if (robotweax_srt_telemetry_delta_engine_create(&engine) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return 2;
    }

    robotweax_srt_telemetry_raw_sample_v1 sample = {0};
    sample.struct_size = sizeof(sample);
    sample.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    sample.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
    sample.sample_monotonic_nano = UINT64_C(1);

    robotweax_srt_telemetry_delta_v1 delta = {0};
    delta.struct_size = sizeof(delta);
    delta.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    if (robotweax_srt_telemetry_delta_engine_update(engine, &sample, &delta) !=
        ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        robotweax_srt_telemetry_delta_engine_destroy(engine);
        return 3;
    }
    if ((delta.quality_flags & ROBOTWEAX_SRT_TELEMETRY_QUALITY_FIRST_SAMPLE) == 0)
    {
        robotweax_srt_telemetry_delta_engine_destroy(engine);
        return 4;
    }

    robotweax_srt_telemetry_delta_engine_destroy(engine);
    return 0;
}
