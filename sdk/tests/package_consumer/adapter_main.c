#include <robotweax/srt_telemetry_public_srt.h>
#include <robotweax/srt_telemetry_public_srt_group.h>

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

static int failing_group_data(int32_t group, struct SRT_SocketGroupData_ *members, size_t *count)
{
    (void)group;
    (void)members;
    (void)count;
    return -1;
}

static int no_name(int32_t socket, struct sockaddr *name, int *length)
{
    (void)socket;
    (void)name;
    (void)length;
    return -1;
}

static int broken_state(int32_t socket)
{
    (void)socket;
    return 6;
}

/* The group header compiles as C and its session links from the package. */
static int group_session_check(void)
{
    static robotweax_srt_telemetry_public_srt_group_result_v1 result;
    robotweax_srt_telemetry_public_srt_group_config_v1 config = {0};
    robotweax_srt_telemetry_public_srt_group_session *session = NULL;
    robotweax_srt_telemetry_error error;
    config.struct_size = sizeof(config);
    config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config.group = (int32_t)((UINT32_C(1) << 30) | 1u);
    config.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
    config.capabilities = ROBOTWEAX_SRT_TELEMETRY_CAP_GROUP_MEMBERS_V1;
    config.bistats = failing_sample;
    config.group_data = failing_group_data;
    config.getsockname = no_name;
    config.getsockstate = broken_state;
    config.group_type = ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP;
    if (robotweax_srt_telemetry_public_srt_group_session_create(&config, &session) !=
        ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return 0;
    }
    result.struct_size = sizeof(result);
    if (robotweax_srt_telemetry_public_srt_group_session_set_path_data_v1(session, NULL) !=
        ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        robotweax_srt_telemetry_public_srt_group_session_destroy(session);
        return 0;
    }
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    error = robotweax_srt_telemetry_public_srt_group_session_sample(session, 1, 1, &result);
    robotweax_srt_telemetry_public_srt_group_session_destroy(session);
    return error == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE && result.group_state == 6 &&
           result.group_data_native_result == -1;
}

int main(void)
{
    if (!group_session_check())
    {
        return 3;
    }

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
