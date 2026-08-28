#include <robotweax/srt_telemetry_public_srt.h>

#include <stdint.h>

struct CBytePerfMon
{
    int unused;
};

static int failing_sample(int32_t socket, struct CBytePerfMon *statistics, int clear,
                          int instantaneous)
{
    (void)socket;
    (void)statistics;
    return clear == 0 && instantaneous == 1 ? -1 : -2;
}

int main(void)
{
    robotweax_srt_telemetry_public_srt_config_v1 config = {0};
    config.struct_size = sizeof(config);
    config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config.socket = 1;
    config.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
    config.bistats = failing_sample;

    robotweax_srt_telemetry_public_srt_session *session = NULL;
    if (robotweax_srt_telemetry_public_srt_session_create(&config, &session) !=
        ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return 1;
    }

    robotweax_srt_telemetry_public_srt_result_v1 result = {0};
    result.struct_size = sizeof(result);
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    const robotweax_srt_telemetry_error error =
        robotweax_srt_telemetry_public_srt_session_sample(session, 1, 1, &result);
    robotweax_srt_telemetry_public_srt_session_destroy(session);

    return error == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE && result.native_result == -1
               ? 0
               : 2;
}
