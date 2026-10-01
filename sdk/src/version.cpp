#include <robotweax/srt_telemetry.h>

extern "C" const char *robotweax_srt_telemetry_version(void)
{
    return ROBOTWEAX_SRT_TELEMETRY_VERSION;
}

extern "C" const char *
robotweax_srt_telemetry_error_string(const robotweax_srt_telemetry_error error)
{
    switch (error)
    {
    case ROBOTWEAX_SRT_TELEMETRY_OK:
        return "success";
    case ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT:
        return "invalid argument";
    case ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH:
        return "telemetry ABI mismatch";
    case ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE:
        return "allocation failure";
    case ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE:
        return "provider sample failure";
    case ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE:
        return "telemetry sink failure";
    case ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE:
        return "invalid state for this call";
    case ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND:
        return "not found";
    case ROBOTWEAX_SRT_TELEMETRY_BUFFER_TOO_SMALL:
        return "buffer too small";
    case ROBOTWEAX_SRT_TELEMETRY_IO_FAILURE:
        return "I/O failure";
    }
    return "unknown telemetry error";
}
