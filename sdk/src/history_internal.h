#ifndef ROBOTWEAX_SRT_TELEMETRY_HISTORY_INTERNAL_H
#define ROBOTWEAX_SRT_TELEMETRY_HISTORY_INTERNAL_H

#include <robotweax/srt_telemetry_history.h>

#include <cstdint>

/* Library-internal: lets the sampler replace its journal without restarting
 * sequence numbers, so readers' cursors stay valid. */
namespace robotweax_srt_telemetry_internal
{

std::uint64_t journal_next_sequence(const robotweax_srt_telemetry_event_journal &journal);

/* Only valid on an empty journal. */
void journal_continue_from(robotweax_srt_telemetry_event_journal &journal,
                           std::uint64_t next_sequence);

} // namespace robotweax_srt_telemetry_internal

#endif
