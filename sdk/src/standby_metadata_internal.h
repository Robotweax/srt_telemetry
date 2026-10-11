#ifndef ROBOTWEAX_SRT_TELEMETRY_STANDBY_METADATA_INTERNAL_H
#define ROBOTWEAX_SRT_TELEMETRY_STANDBY_METADATA_INTERNAL_H

#include <robotweax/srt_telemetry_sampler.h>

namespace robotweax_srt_telemetry_internal
{
inline bool valid_standby_metadata(const robotweax_srt_telemetry_standby_rtt_metadata_v1 &m)
{
    if (m.struct_size < sizeof(m) || m.abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION ||
        m.availability > 2 || m.flags > 31 || m.outcome > 7 || m.reserved[0] || m.reserved[1])
        return false;
    if (m.availability != 2)
        return m.flags == 0 && m.outcome == 0 && m.rtt_microseconds == 0 && m.samples == 0 &&
               m.age_microseconds == UINT64_MAX && (m.availability != 0 || m.native_result == 0);
    if (m.native_result != 0)
        return false;
    const bool negotiated = (m.flags & 1) != 0, admitted = (m.flags & 2) != 0;
    const bool observed = (m.flags & 4) != 0, fresh = (m.flags & 8) != 0;
    if (!negotiated)
        return m.flags == 0 && m.outcome == 0 && m.rtt_microseconds == 0 && m.samples == 0 &&
               m.age_microseconds == UINT64_MAX;
    if (!admitted)
        return (m.flags & ~17u) == 0 && m.outcome == 7 && m.rtt_microseconds == 0 &&
               m.samples == 0 && m.age_microseconds == UINT64_MAX;
    return m.outcome != 7 && observed == (m.samples != 0) &&
           observed == (m.rtt_microseconds != 0) &&
           (observed || m.age_microseconds == UINT64_MAX) &&
           fresh == (observed && m.age_microseconds <= 3'000'000) && (m.outcome != 2 || observed);
}
} // namespace robotweax_srt_telemetry_internal
#endif
