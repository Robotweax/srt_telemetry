#include <robotweax/srt_telemetry.h>

#include <stddef.h>
#include <string.h>

static int sink(void *user_data, const char *line, size_t line_size)
{
    (void)line;
    *(size_t *)user_data += line_size;
    return 0;
}

int main(void)
{
    robotweax_srt_telemetry_recorder *recorder = NULL;
    robotweax_srt_telemetry_jsonl_export_config_v1 config;
    robotweax_srt_telemetry_jsonl_export_v3_options options;
    size_t bytes = 0;
    int result = 0;
    if (strcmp(robotweax_srt_telemetry_version(), "0.1.0-alpha.0") != 0 ||
        robotweax_srt_telemetry_recorder_create(4, &recorder) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return 1;
    }
    memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config.stream_id = config.flow_id = config.provider_version = config.provider_revision = config.build_profile =
        "consumer";
    config.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    config.configured_provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    memset(&options, 0, sizeof(options));
    options.struct_size = sizeof(options);
    options.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    options.group_id = -1;
    options.peer_group_id = -1;
    options.session_ref = "consumer";
    /* An empty recording: the header alone. */
    if (robotweax_srt_telemetry_recorder_push_sample_v1(recorder, NULL, NULL, NULL, NULL) !=
            ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT ||
        robotweax_srt_telemetry_recorder_export_jsonl_v3(recorder, &config, &options, sink, &bytes) !=
            ROBOTWEAX_SRT_TELEMETRY_OK ||
        bytes == 0)
    {
        result = 2;
    }
    robotweax_srt_telemetry_recorder_destroy(recorder);
    return result;
}
