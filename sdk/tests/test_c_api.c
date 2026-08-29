#include <robotweax/srt_telemetry.h>

#include <stdint.h>
#include <string.h>

static int accept_jsonl(void *user_data, const char *line, size_t line_size)
{
    size_t *line_count = (size_t *)user_data;
    if (line == NULL || line_size == 0 || line[line_size - 1] != '\n')
    {
        return 1;
    }
    *line_count += 1;
    return 0;
}

static int reject_jsonl(void *user_data, const char *line, size_t line_size)
{
    (void)user_data;
    (void)line;
    (void)line_size;
    return 1;
}

int main(void)
{
    robotweax_srt_telemetry_delta_engine *engine = NULL;
    if (strcmp(robotweax_srt_telemetry_version(), "0.1.0-alpha.0") != 0)
    {
        return 1;
    }
    if (strcmp(robotweax_srt_telemetry_error_string(ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE),
               "telemetry sink failure") != 0)
    {
        return 2;
    }
    if (robotweax_srt_telemetry_delta_engine_create(&engine) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return 3;
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
        return 4;
    }
    if ((delta.quality_flags & ROBOTWEAX_SRT_TELEMETRY_QUALITY_FIRST_SAMPLE) == 0)
    {
        robotweax_srt_telemetry_delta_engine_destroy(engine);
        return 5;
    }

    robotweax_srt_telemetry_recorder *recorder = NULL;
    if (robotweax_srt_telemetry_recorder_create(2, &recorder) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        robotweax_srt_telemetry_delta_engine_destroy(engine);
        return 6;
    }
    if (robotweax_srt_telemetry_recorder_push(recorder, &sample, &delta) !=
            ROBOTWEAX_SRT_TELEMETRY_OK ||
        robotweax_srt_telemetry_recorder_size(recorder) != 1)
    {
        robotweax_srt_telemetry_recorder_destroy(recorder);
        robotweax_srt_telemetry_delta_engine_destroy(engine);
        return 7;
    }

    robotweax_srt_telemetry_jsonl_export_config_v1 export_config = {0};
    export_config.struct_size = sizeof(export_config);
    export_config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    export_config.stream_id = "c-api-sender";
    export_config.flow_id = "c-api-flow";
    export_config.provider_version = "1.5.5";
    export_config.provider_revision = "0123456789abcdef";
    export_config.build_profile = "release-test";
    export_config.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    export_config.configured_provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
    size_t line_count = 0;
    if (robotweax_srt_telemetry_recorder_export_jsonl(recorder, &export_config, accept_jsonl,
                                                      &line_count) != ROBOTWEAX_SRT_TELEMETRY_OK ||
        line_count != 2)
    {
        robotweax_srt_telemetry_recorder_destroy(recorder);
        robotweax_srt_telemetry_delta_engine_destroy(engine);
        return 8;
    }
    if (robotweax_srt_telemetry_recorder_export_jsonl(recorder, &export_config, reject_jsonl,
                                                      NULL) != ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE)
    {
        robotweax_srt_telemetry_recorder_destroy(recorder);
        robotweax_srt_telemetry_delta_engine_destroy(engine);
        return 9;
    }

    export_config.flow_id = NULL;
    if (robotweax_srt_telemetry_recorder_export_jsonl(recorder, &export_config, accept_jsonl,
                                                      &line_count) !=
        ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT)
    {
        robotweax_srt_telemetry_recorder_destroy(recorder);
        robotweax_srt_telemetry_delta_engine_destroy(engine);
        return 10;
    }
    export_config.flow_id = "c-api-flow";
    export_config.struct_size = sizeof(export_config) - 1;
    if (robotweax_srt_telemetry_recorder_export_jsonl(recorder, &export_config, accept_jsonl,
                                                      &line_count) !=
        ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH)
    {
        robotweax_srt_telemetry_recorder_destroy(recorder);
        robotweax_srt_telemetry_delta_engine_destroy(engine);
        return 11;
    }

    robotweax_srt_telemetry_recorder_destroy(recorder);
    robotweax_srt_telemetry_delta_engine_destroy(engine);
    return 0;
}
