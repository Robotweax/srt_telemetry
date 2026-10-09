#ifndef ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_PATH_DATA_HPP
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_PATH_DATA_HPP

#include <robotweax/srt_telemetry_public_srt_group.h>
#include <cstddef>

/* Private completion of the public, versioned record when the build header
 * predates the extension or belongs to Haivision. This is the published v1
 * Robotweax record contract, not a provider implementation. No SRT symbol is
 * linked. Hosts inject an exactly typed getter from their selected provider. */
#if !defined(ROBOTWEAX_SRT_PATHID_NEGOTIATED)
struct ROBOTWEAX_SRT_GroupPathDataV1_
{
    int32_t member_id;
    uint32_t flags;
    uint32_t identifier_length;
    uint8_t identifier[32];
};
#endif

static_assert(sizeof(ROBOTWEAX_SRT_GroupPathDataV1_) == 44);
static_assert(offsetof(ROBOTWEAX_SRT_GroupPathDataV1_, identifier) == 12);

#endif
