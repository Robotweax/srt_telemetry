#ifndef ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_INTERNAL_HPP
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_INTERNAL_HPP

/* Counter mapping shared by the statistics session and the group session.
 * Not installed. */

#include <robotweax/srt_telemetry_public_srt.h>

#if defined(ROBOTWEAX_SRT_TELEMETRY_USE_FLAT_SRT_HEADER)
#include <srt.h>
#else
#include <srt/srt.h>
#endif

#include <cstdint>

namespace robotweax_srt_telemetry_detail
{

/* Extended candidate fields of a provider (each provider qualifies only its
 * own mbpsBandwidth interpretation). */
std::uint64_t provider_ext_candidates(robotweax_srt_telemetry_provider provider);

/* Fills raw from one srt_bistats result, limited to config.qualified_fields. */
void map_raw(const SRT_TRACEBSTATS &statistics,
             const robotweax_srt_telemetry_public_srt_config_v2 &config,
             std::uint64_t observed_at_unix_nano, std::uint64_t sample_monotonic_nano,
             robotweax_srt_telemetry_raw_sample_v1 &raw);

/* Fills ext from one srt_bistats result, limited to
 * config.qualified_ext_fields. */
void map_ext(const SRT_TRACEBSTATS &statistics,
             const robotweax_srt_telemetry_public_srt_config_v2 &config,
             robotweax_srt_telemetry_ext_sample_v1 &ext);

} // namespace robotweax_srt_telemetry_detail

#endif
