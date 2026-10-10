#ifndef ROBOTWEAX_SRT_TELEMETRY_EXPORT_INTERNAL_H
#define ROBOTWEAX_SRT_TELEMETRY_EXPORT_INTERNAL_H

#include <robotweax/srt_telemetry_export.h>
#include <robotweax/srt_telemetry_health.h>
#include <robotweax/srt_telemetry_sampler.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/* Library-internal: the sampler copies each source's state under its lock
 * into these views; the renderers format them without any lock held. */
namespace robotweax_srt_telemetry_internal
{

struct ExportSource
{
    std::uint64_t id = 0;
    robotweax_srt_telemetry_endpoint_role role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_UNKNOWN;
    std::vector<std::pair<std::string, std::string>> labels;
    /* Prebuilt when the source was added; built on demand if empty. */
    std::string prometheus_labels;
    bool failed = false;
    bool has_window = false;
    bool window_stale = false;
    robotweax_srt_telemetry_window_summary_v1 window{};
    bool has_metrics = false;
    robotweax_srt_telemetry_metric_sample_v1 metrics{};
    robotweax_srt_telemetry_rtt_metadata_v1 rtt_metadata{};
    double rtt_capture_age_seconds = 0;
    bool has_raw = false;
    robotweax_srt_telemetry_raw_sample_v1 raw{};
    bool has_ext = false;
    robotweax_srt_telemetry_ext_sample_v1 ext{};
    bool has_health = false;
    robotweax_srt_telemetry_health_status_v1 health{};
    robotweax_srt_telemetry_connection_condition connection = ROBOTWEAX_SRT_TELEMETRY_CONNECTION_UNREPORTED;
    /* When the lifetime counters started: the first sample, or the latest
     * counter reset (OTLP cumulative start time). */
    std::uint64_t counters_start_unix_nano = 0;
};

struct ExportView
{
    std::uint64_t generated_unix_nano = 0;
    std::uint64_t window_nano = 0;
    std::uint32_t flags = 0;
    std::vector<ExportSource> sources;
    /* OTLP resource attributes from the host. */
    std::vector<std::pair<std::string, std::string>> resource;
};

std::string prometheus_labels(std::uint64_t id, robotweax_srt_telemetry_endpoint_role role,
                              const std::vector<std::pair<std::string, std::string>> &labels);
std::string render_prometheus(const ExportView &view);
std::string render_json(const ExportView &view);
std::string render_otlp_json(const ExportView &view);
std::vector<std::pair<std::string, std::string>>
exporter_labels(const std::vector<std::pair<std::string, std::string>> &labels);

} // namespace robotweax_srt_telemetry_internal

#endif
