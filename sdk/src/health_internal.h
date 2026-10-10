#ifndef ROBOTWEAX_SRT_TELEMETRY_HEALTH_INTERNAL_H
#define ROBOTWEAX_SRT_TELEMETRY_HEALTH_INTERNAL_H

#include <robotweax/srt_telemetry_health.h>

namespace robotweax_srt_telemetry_internal
{

/* The rules of health_engine_set_group for a non-null group, without an
 * engine: OK, ABI_MISMATCH or INVALID_ARGUMENT. */
robotweax_srt_telemetry_error validate_health_group(const robotweax_srt_telemetry_health_group_v1 &group);

} // namespace robotweax_srt_telemetry_internal

#endif
