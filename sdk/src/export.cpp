#include "export_internal.h"

#include <array>
#include <charconv>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace
{

using robotweax_srt_telemetry_internal::ExportSource;
using robotweax_srt_telemetry_internal::ExportView;

struct MetricName
{
    const char *otel;
    const char *prometheus;
    const char *scope;
    const char *help;
};

/* Schema version 1. Order follows robotweax_srt_telemetry_metric. */
constexpr std::array<MetricName, ROBOTWEAX_SRT_TELEMETRY_METRIC_DEFINED_COUNT> names = {{
    {"srt.sender.sent.bitrate", "srt_sender_sent_bits_per_second", "sender",
     "Bits sent on the wire, original and retransmitted."},
    {"srt.sender.unique_sent.bitrate", "srt_sender_unique_sent_bits_per_second", "sender",
     "Bits of original (first-time) packets sent."},
    {"srt.sender.retransmitted.bitrate", "srt_sender_retransmitted_bits_per_second", "sender",
     "Bits of retransmitted packets sent."},
    {"srt.receiver.received.bitrate", "srt_receiver_received_bits_per_second", "receiver",
     "Bits received on the wire, including duplicates and retransmissions."},
    {"srt.receiver.unique_received.bitrate", "srt_receiver_unique_received_bits_per_second", "receiver",
     "Bits of distinct packets received."},
    {"srt.sender.unique_sent.packet_rate", "srt_sender_unique_sent_packets_per_second", "sender",
     "Original packets sent."},
    {"srt.receiver.unique_received.packet_rate", "srt_receiver_unique_received_packets_per_second", "receiver",
     "Distinct packets received."},
    {"srt.sender.loss_reported.packet_rate", "srt_sender_loss_reported_packets_per_second", "sender",
     "Packets the receiver reported lost (NAK)."},
    {"srt.receiver.lost.packet_rate", "srt_receiver_lost_packets_per_second", "receiver",
     "Packets detected missing at the receiver (before repair)."},
    {"srt.sender.retransmitted.packet_rate", "srt_sender_retransmitted_packets_per_second", "sender",
     "Packets retransmitted."},
    {"srt.receiver.retransmitted.packet_rate", "srt_receiver_retransmitted_packets_per_second", "receiver",
     "Retransmitted packets received."},
    {"srt.sender.dropped.packet_rate", "srt_sender_dropped_packets_per_second", "sender",
     "Packets the sender dropped as too late to send (not network loss)."},
    {"srt.receiver.dropped.packet_rate", "srt_receiver_dropped_packets_per_second", "receiver",
     "Packets never delivered to the application (skipped), including undecryptable ones."},
    {"srt.receiver.undecryptable.packet_rate", "srt_receiver_undecryptable_packets_per_second", "receiver",
     "Packets that could not be decrypted."},
    {"srt.receiver.belated.packet_rate", "srt_receiver_belated_packets_per_second", "receiver",
     "Packets that arrived after their play time."},
    {"srt.receiver.fec_recovered.packet_rate", "srt_receiver_fec_recovered_packets_per_second", "receiver",
     "Packets rebuilt by the packet filter (FEC)."},
    {"srt.receiver.fec_unrecovered.packet_rate", "srt_receiver_fec_unrecovered_packets_per_second", "receiver",
     "Packets the packet filter (FEC) could not rebuild."},
    {"srt.receiver.nak_sent.packet_rate", "srt_receiver_nak_sent_packets_per_second", "receiver",
     "Loss reports (NAK packets) sent."},
    {"srt.sender.nak_received.packet_rate", "srt_sender_nak_received_packets_per_second", "sender",
     "Loss reports (NAK packets) received."},
    {"srt.connection.rtt", "srt_connection_rtt_seconds", "connection", "Smoothed round-trip time."},
    {"srt.sender.buffer.duration", "srt_sender_buffer_seconds", "sender", "Time span of data in the send buffer."},
    {"srt.receiver.buffer.duration", "srt_receiver_buffer_seconds", "receiver",
     "Time span of data in the receive buffer."},
    {"srt.receiver.latency", "srt_receiver_latency_seconds", "receiver", "Receiver latency (TSBPD delay)."},
    {"srt.sender.peer_latency", "srt_sender_peer_latency_seconds", "sender",
     "Latency the receiver applies, as negotiated."},
    {"srt.sender.flight_size", "srt_sender_flight_size_packets", "sender", "Packets sent and not yet acknowledged."},
    {"srt.sender.flow_window", "srt_sender_flow_window_packets", "sender",
     "Free receive buffer the peer advertises, in packets."},
    {"srt.receiver.buffer.available", "srt_receiver_buffer_available_bytes", "receiver",
     "Free space in the receive buffer."},
    {"srt.sender.packet_send_period", "srt_sender_packet_send_period_seconds", "sender",
     "Interval between packets set by the sender's pacing."},
    {"srt.connection.link_capacity_estimate", "srt_connection_link_capacity_estimate_bits_per_second",
     "connection",
     "SRT's link capacity estimate from probe packet pairs (mbpsBandwidth; Haivision SRT). Follows the arrival rate "
     "on a healthy path."},
    {"srt.receiver.recovery_headroom", "srt_receiver_recovery_headroom_ratio", "receiver",
     "Receive buffer span / latency: time left to repair a gap, normalised; about 1 when nothing is missing."},
    {"srt.sender.flow_window.utilization", "srt_sender_flow_window_utilization_ratio", "sender",
     "Flight size / flow window: near 1 when the peer stops taking data."},
    {"srt.sender.buffer.drop_threshold_utilization", "srt_sender_buffer_drop_threshold_utilization_ratio", "sender",
     "Send buffer span / too-late drop threshold: near 1 just before sender drops."},
}};

struct CounterName
{
    bool ext;
    std::uint64_t field;
    std::uint64_t (*value)(const ExportSource &);
    const char *otel;
    const char *prometheus;
    const char *scope;
    const char *help;
};

#define RAW_COUNTER(member) [](const ExportSource &s) -> std::uint64_t { return s.raw.member; }
#define EXT_COUNTER(member) [](const ExportSource &s) -> std::uint64_t { return s.ext.member; }

/* Lifetime totals since the connection started (the adapter never clears
 * the provider's statistics). Bytes include IP/UDP/SRT headers. */
const std::array<CounterName, 18> counters = {{
    {false, ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL, RAW_COUNTER(sent_packet_total),
     "srt.sender.sent.packets", "srt_sender_sent_packets_total", "sender",
     "Packets sent, original and retransmitted."},
    {false, ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL, RAW_COUNTER(sent_wire_byte_total),
     "srt.sender.sent.bytes", "srt_sender_sent_bytes_total", "sender",
     "Bytes sent, original and retransmitted, with IP/UDP/SRT headers."},
    {false, ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL, RAW_COUNTER(unique_sent_packet_total),
     "srt.sender.unique_sent.packets", "srt_sender_unique_sent_packets_total", "sender", "Original packets sent."},
    {false, ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL, RAW_COUNTER(unique_sent_wire_byte_total),
     "srt.sender.unique_sent.bytes", "srt_sender_unique_sent_bytes_total", "sender",
     "Bytes of original packets sent, with headers."},
    {false, ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_PACKET_TOTAL, RAW_COUNTER(retransmitted_packet_total),
     "srt.sender.retransmitted.packets", "srt_sender_retransmitted_packets_total", "sender",
     "Packets retransmitted."},
    {false, ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_LOSS_PACKET_TOTAL, RAW_COUNTER(sender_loss_packet_total),
     "srt.sender.loss_reported.packets", "srt_sender_loss_reported_packets_total", "sender",
     "Packets reported lost by the receiver."},
    {false, ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_DROP_PACKET_TOTAL, RAW_COUNTER(sender_drop_packet_total),
     "srt.sender.dropped.packets", "srt_sender_dropped_packets_total", "sender",
     "Packets dropped by the sender as too late."},
    {true, ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_NAK_PACKET_TOTAL, EXT_COUNTER(received_nak_packet_total),
     "srt.sender.nak_received.packets", "srt_sender_nak_received_packets_total", "sender",
     "Loss reports (NAK packets) received."},
    {false, ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_PACKET_TOTAL, RAW_COUNTER(received_packet_total),
     "srt.receiver.received.packets", "srt_receiver_received_packets_total", "receiver",
     "Packets received, including duplicates and retransmissions."},
    {false, ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_WIRE_BYTE_TOTAL, RAW_COUNTER(received_wire_byte_total),
     "srt.receiver.received.bytes", "srt_receiver_received_bytes_total", "receiver",
     "Bytes received, with IP/UDP/SRT headers."},
    {false, ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL, RAW_COUNTER(unique_received_packet_total),
     "srt.receiver.unique_received.packets", "srt_receiver_unique_received_packets_total", "receiver",
     "Distinct packets received."},
    {false, ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL,
     RAW_COUNTER(unique_received_wire_byte_total), "srt.receiver.unique_received.bytes",
     "srt_receiver_unique_received_bytes_total", "receiver", "Bytes of distinct packets received, with headers."},
    {false, ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_LOSS_PACKET_TOTAL, RAW_COUNTER(receiver_loss_packet_total),
     "srt.receiver.lost.packets", "srt_receiver_lost_packets_total", "receiver",
     "Packets detected missing (before repair)."},
    {false, ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_DROP_PACKET_TOTAL, RAW_COUNTER(receiver_drop_packet_total),
     "srt.receiver.dropped.packets", "srt_receiver_dropped_packets_total", "receiver",
     "Packets never delivered to the application."},
    {false, ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_UNDECRYPTABLE_PACKET_TOTAL,
     RAW_COUNTER(receiver_undecryptable_packet_total), "srt.receiver.undecryptable.packets",
     "srt_receiver_undecryptable_packets_total", "receiver", "Packets that could not be decrypted."},
    {true, ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_RETRANSMITTED_PACKET_SINCE_CLEAR,
     EXT_COUNTER(received_retransmitted_packet_since_clear), "srt.receiver.retransmitted.packets",
     "srt_receiver_retransmitted_packets_total", "receiver", "Retransmitted packets received."},
    {true, ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_BELATED_PACKET_SINCE_CLEAR, EXT_COUNTER(belated_packet_since_clear),
     "srt.receiver.belated.packets", "srt_receiver_belated_packets_total", "receiver",
     "Packets that arrived after their play time."},
    {true, ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SENT_NAK_PACKET_TOTAL, EXT_COUNTER(sent_nak_packet_total),
     "srt.receiver.nak_sent.packets", "srt_receiver_nak_sent_packets_total", "receiver",
     "Loss reports (NAK packets) sent."},
}};

#undef RAW_COUNTER
#undef EXT_COUNTER

bool counter_available(const ExportSource &s, const CounterName &c)
{
    return c.ext ? (s.has_ext && (s.ext.available_fields & c.field) != 0)
                 : (s.has_raw && (s.raw.available_fields & c.field) != 0);
}

struct Flag
{
    std::uint32_t bit;
    const char *name;
};
constexpr std::array<Flag, 6> window_flags = {{
    {ROBOTWEAX_SRT_TELEMETRY_WINDOW_PARTIAL, "partial"},
    {ROBOTWEAX_SRT_TELEMETRY_WINDOW_SAMPLING_GAP, "sampling_gap"},
    {ROBOTWEAX_SRT_TELEMETRY_WINDOW_CLOCK_STEP, "clock_step"},
    {ROBOTWEAX_SRT_TELEMETRY_WINDOW_COUNTER_RESET, "counter_reset"},
    {ROBOTWEAX_SRT_TELEMETRY_WINDOW_BASELINE, "baseline"},
    {ROBOTWEAX_SRT_TELEMETRY_WINDOW_SOURCE_ERROR, "source_error"},
}};

const char *role_name(const robotweax_srt_telemetry_endpoint_role role)
{
    switch (role)
    {
    case ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER:
        return "sender";
    case ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER:
        return "receiver";
    default:
        return "unknown";
    }
}

const char *connection_name(const robotweax_srt_telemetry_connection_condition c)
{
    switch (c)
    {
    case ROBOTWEAX_SRT_TELEMETRY_CONNECTION_CONNECTING:
        return "connecting";
    case ROBOTWEAX_SRT_TELEMETRY_CONNECTION_CONNECTED:
        return "connected";
    case ROBOTWEAX_SRT_TELEMETRY_CONNECTION_LOST:
        return "lost";
    default:
        return "unreported";
    }
}

const char *direction_name(const robotweax_srt_telemetry_knob_direction d)
{
    switch (d)
    {
    case ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_INCREASE:
        return "increase";
    case ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_DECREASE:
        return "decrease";
    case ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_CHECK:
        return "check";
    default:
        return "none";
    }
}

/* A source exports the metrics of its own role and of the connection;
 * a source of unknown role exports everything. */
bool in_scope(const ExportSource &source, const char *scope)
{
    if (source.role == ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_UNKNOWN || std::strcmp(scope, "connection") == 0)
    {
        return true;
    }
    return std::strcmp(scope, role_name(source.role)) == 0;
}

/* Shortest round-trip text, independent of the C locale; false for NaN
 * and infinities. */
bool number(std::string &out, const double value)
{
    if (!std::isfinite(value))
    {
        return false;
    }
    char text[40];
    const auto result = std::to_chars(text, text + sizeof(text), value);
    out.append(text, result.ptr);
    return true;
}

void integer(std::string &out, const std::uint64_t value)
{
    char text[24];
    const auto result = std::to_chars(text, text + sizeof(text), value);
    out.append(text, result.ptr);
}

/* ------------------------------------------------------------- Prometheus */

void prometheus_escape(std::string &out, const std::string_view value)
{
    for (const char c : value)
    {
        if (c == '\\')
        {
            out += "\\\\";
        }
        else if (c == '"')
        {
            out += "\\\"";
        }
        else if (c == '\n')
        {
            out += "\\n";
        }
        else
        {
            out += c;
        }
    }
}

bool reserved_label(const std::string_view key)
{
    return key == "source_id" || key == "role" || key == "stat" || key == "flag" || key == "indicator" ||
           key == "verdict" || key == "reason" || key == "knob" || key == "direction" || key == "job" ||
           key == "instance" || key.substr(0, 2) == "__";
}

} // namespace

/* A host key that collides with an exporter label, or with another host
 * label after renaming, gets "host_" prefixes until it is unique. The same
 * names serve Prometheus labels and OTLP attributes. */
std::vector<std::pair<std::string, std::string>>
robotweax_srt_telemetry_internal::exporter_labels(const std::vector<std::pair<std::string, std::string>> &labels)
{
    std::set<std::string> used;
    for (const auto &label : labels)
    {
        used.insert(label.first);
    }
    std::set<std::string> taken;
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(labels.size());
    for (const auto &[key, value] : labels)
    {
        std::string name = key;
        if (reserved_label(name))
        {
            do
            {
                name = "host_" + name;
            } while (reserved_label(name) || used.count(name) != 0 || taken.count(name) != 0);
        }
        taken.insert(name);
        out.emplace_back(std::move(name), value);
    }
    return out;
}

std::string robotweax_srt_telemetry_internal::prometheus_labels(
    const std::uint64_t id, const robotweax_srt_telemetry_endpoint_role role,
    const std::vector<std::pair<std::string, std::string>> &labels)
{
    std::string text = "source_id=\"";
    integer(text, id);
    text += "\",role=\"";
    text += role_name(role);
    text += '"';
    for (const auto &[name, value] : exporter_labels(labels))
    {
        text += ',';
        text += name;
        text += "=\"";
        prometheus_escape(text, value);
        text += '"';
    }
    return text;
}

namespace
{

void family(std::string &out, const char *name, const char *type, const char *help)
{
    out += "# HELP ";
    out += name;
    out += ' ';
    for (const char *c = help; *c != '\0'; ++c)
    {
        out += *c == '\\' ? std::string("\\\\") : *c == '\n' ? std::string("\\n") : std::string(1, *c);
    }
    out += "\n# TYPE ";
    out += name;
    out += ' ';
    out += type;
    out += '\n';
}

void sample(std::string &out, const char *name, const std::string &labels, const std::string &extra, const double value)
{
    std::string number_text;
    if (!number(number_text, value))
    {
        return;
    }
    out += name;
    out += '{';
    out += labels;
    if (!extra.empty())
    {
        out += ',';
        out += extra;
    }
    out += "} ";
    out += number_text;
    out += '\n';
}

bool window_usable(const ExportSource &s)
{
    return s.has_window && !s.window_stale;
}

} // namespace

std::string robotweax_srt_telemetry_internal::render_prometheus(const ExportView &view)
{
    std::string out;
    std::vector<std::string> labels;
    labels.reserve(view.sources.size());
    for (const auto &s : view.sources)
    {
        labels.push_back(s.prometheus_labels.empty() ? prometheus_labels(s.id, s.role, s.labels)
                                                     : s.prometheus_labels);
    }
    const auto each = [&](auto &&body)
    {
        for (std::size_t i = 0; i < view.sources.size(); ++i)
        {
            body(view.sources[i], labels[i]);
        }
    };

    family(out, "srt_telemetry_schema_info", "gauge",
           "Metric naming scheme version of this exporter (value 1, version in the label).");
    out += "srt_telemetry_schema_info{version=\"";
    integer(out, ROBOTWEAX_SRT_TELEMETRY_METRIC_SCHEMA_VERSION);
    out += "\"} 1\n";

    family(out, "srt_source_up", "gauge", "1 while the source's statistics read succeeds, 0 while it fails.");
    each([&](const ExportSource &s, const std::string &l) { sample(out, "srt_source_up", l, "", s.failed ? 0.0 : 1.0); });

    family(out, "srt_window_seconds", "gauge", "Length of the exported statistics window.");
    each(
        [&](const ExportSource &s, const std::string &l)
        {
            if (window_usable(s))
            {
                sample(out, "srt_window_seconds", l, "", static_cast<double>(s.window.window_nano) / 1e9);
            }
        });
    family(out, "srt_window_samples", "gauge", "Samples in the exported window.");
    each(
        [&](const ExportSource &s, const std::string &l)
        {
            if (window_usable(s))
            {
                sample(out, "srt_window_samples", l, "", s.window.sample_count);
            }
        });
    family(out, "srt_window_quality", "gauge", "1 when the exported window carries the data-quality flag.");
    each(
        [&](const ExportSource &s, const std::string &l)
        {
            if (!window_usable(s))
            {
                return;
            }
            for (const auto &f : window_flags)
            {
                sample(out, "srt_window_quality", l, std::string("flag=\"") + f.name + '"',
                       (s.window.quality_flags & f.bit) != 0 ? 1.0 : 0.0);
            }
        });

    for (std::size_t m = 0; m < names.size(); ++m)
    {
        const auto metric = static_cast<robotweax_srt_telemetry_metric>(m);
        const auto *info = robotweax_srt_telemetry_metric_info(metric);
        const bool rate = info->kind == ROBOTWEAX_SRT_TELEMETRY_METRIC_KIND_RATE;
        std::string help = names[m].help;
        help += rate ? " Window statistic in label stat: rate (exact over the window), max and p95 of 100 ms "
                       "samples."
                     : " Window statistic in label stat: min, max, mean, last, p95.";
        bool any = false;
        each(
            [&](const ExportSource &s, const std::string &)
            {
                any = any || (window_usable(s) && in_scope(s, names[m].scope) &&
                              (s.window.available_metrics & ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(metric)) != 0);
            });
        if (!any)
        {
            continue;
        }
        family(out, names[m].prometheus, "gauge", help.c_str());
        each(
            [&](const ExportSource &s, const std::string &l)
            {
                if (!window_usable(s) || !in_scope(s, names[m].scope) ||
                    (s.window.available_metrics & ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(metric)) == 0)
                {
                    return;
                }
                const auto &x = s.window.metrics[m];
                const bool p95 = (x.flags & ROBOTWEAX_SRT_TELEMETRY_METRIC_SUMMARY_PERCENTILE_AVAILABLE) != 0;
                if (rate)
                {
                    sample(out, names[m].prometheus, l, "stat=\"rate\"", x.rate);
                    sample(out, names[m].prometheus, l, "stat=\"max\"", x.max);
                }
                else
                {
                    sample(out, names[m].prometheus, l, "stat=\"min\"", x.min);
                    sample(out, names[m].prometheus, l, "stat=\"max\"", x.max);
                    sample(out, names[m].prometheus, l, "stat=\"mean\"", x.mean);
                    sample(out, names[m].prometheus, l, "stat=\"last\"", x.last);
                }
                if (p95)
                {
                    sample(out, names[m].prometheus, l, "stat=\"p95\"", x.p95);
                }
            });
    }

    if ((view.flags & ROBOTWEAX_SRT_TELEMETRY_RENDER_NO_COUNTERS) == 0)
    {
        for (const auto &c : counters)
        {
            bool any = false;
            each([&](const ExportSource &s, const std::string &)
                 { any = any || (in_scope(s, c.scope) && counter_available(s, c)); });
            if (!any)
            {
                continue;
            }
            family(out, c.prometheus, "counter", c.help);
            each(
                [&](const ExportSource &s, const std::string &l)
                {
                    if (in_scope(s, c.scope) && counter_available(s, c))
                    {
                        /* Integers exactly: counters must not lose steps. */
                        out += c.prometheus;
                        out += '{';
                        out += l;
                        out += "} ";
                        integer(out, c.value(s));
                        out += '\n';
                    }
                });
        }
    }

    bool any_health = false;
    each([&](const ExportSource &s, const std::string &) { any_health = any_health || s.has_health; });
    if (any_health)
    {
        family(out, "srt_health_verdict", "gauge",
               "Health verdict: 0 unknown, 1 ok, 2 watch, 3 at risk, 4 failing.");
        each(
            [&](const ExportSource &s, const std::string &l)
            {
                if (s.has_health)
                {
                    sample(out, "srt_health_verdict", l, "", static_cast<double>(s.health.verdict));
                }
            });
        family(out, "srt_health_info", "gauge", "Current verdict, reason and suggested knob (value 1).");
        each(
            [&](const ExportSource &s, const std::string &l)
            {
                if (!s.has_health)
                {
                    return;
                }
                std::string extra = "verdict=\"";
                extra += robotweax_srt_telemetry_health_verdict_name(s.health.verdict);
                extra += "\",reason=\"";
                extra += robotweax_srt_telemetry_health_reason_name(s.health.reason);
                extra += "\",knob=\"";
                extra += robotweax_srt_telemetry_health_knob_name(s.health.knob);
                extra += "\",direction=\"";
                extra += direction_name(s.health.knob_direction);
                extra += '"';
                sample(out, "srt_health_info", l, extra, 1.0);
            });
        family(out, "srt_health_suggested_value", "gauge",
               "Suggested value for the knob in srt_health_info, in the knob's unit (s, bit/s, bytes/s, percent).");
        each(
            [&](const ExportSource &s, const std::string &l)
            {
                if (s.has_health && s.health.suggested_value != 0)
                {
                    sample(out, "srt_health_suggested_value", l,
                           std::string("knob=\"") + robotweax_srt_telemetry_health_knob_name(s.health.knob) + '"',
                           s.health.suggested_value);
                }
            });
        if ((view.flags & ROBOTWEAX_SRT_TELEMETRY_RENDER_NO_INDICATORS) == 0)
        {
            family(out, "srt_health_indicator_state", "gauge",
                   "Indicator state after hysteresis: 0 unknown, 1 ok, 2 watch, 3 at risk, 4 failing.");
            each(
                [&](const ExportSource &s, const std::string &l)
                {
                    if (!s.has_health)
                    {
                        return;
                    }
                    for (std::size_t i = 0; i < ROBOTWEAX_SRT_TELEMETRY_INDICATOR_COUNT; ++i)
                    {
                        const auto &ind = s.health.indicators[i];
                        if ((ind.flags & ROBOTWEAX_SRT_TELEMETRY_INDICATOR_APPLICABLE) == 0)
                        {
                            continue;
                        }
                        sample(out, "srt_health_indicator_state", l,
                               std::string("indicator=\"") +
                                   robotweax_srt_telemetry_health_indicator_name(
                                       static_cast<robotweax_srt_telemetry_health_indicator>(i)) +
                                   '"',
                               static_cast<double>(ind.state));
                    }
                });
            family(out, "srt_health_indicator_value", "gauge", "Indicator value (see docs/architecture/health-engine.md).");
            each(
                [&](const ExportSource &s, const std::string &l)
                {
                    if (!s.has_health)
                    {
                        return;
                    }
                    for (std::size_t i = 0; i < ROBOTWEAX_SRT_TELEMETRY_INDICATOR_COUNT; ++i)
                    {
                        const auto &ind = s.health.indicators[i];
                        if ((ind.flags & ROBOTWEAX_SRT_TELEMETRY_INDICATOR_AVAILABLE) == 0)
                        {
                            continue;
                        }
                        sample(out, "srt_health_indicator_value", l,
                               std::string("indicator=\"") +
                                   robotweax_srt_telemetry_health_indicator_name(
                                       static_cast<robotweax_srt_telemetry_health_indicator>(i)) +
                                   '"',
                               ind.value);
                    }
                });
        }
    }
    return out;
}

/* ------------------------------------------------------------------- JSON */

namespace
{

void json_string(std::string &out, const std::string_view value)
{
    out += '"';
    for (const char c : value)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (byte < 0x20 || byte == 0x7f)
        {
            char escape[8];
            std::snprintf(escape, sizeof(escape), "\\u%04x", byte);
            out += escape;
        }
        else
        {
            out += c;
        }
    }
    out += '"';
}

void json_number(std::string &out, const double value)
{
    if (!number(out, value))
    {
        out += "null";
    }
}

/* Appends ,"key":value for a finite value. */
void json_field(std::string &out, const char *key, const double value, bool &first)
{
    std::string text;
    if (!number(text, value))
    {
        return;
    }
    if (!first)
    {
        out += ',';
    }
    first = false;
    json_string(out, key);
    out += ':';
    out += text;
}

} // namespace

std::string robotweax_srt_telemetry_internal::render_json(const ExportView &view)
{
    std::string out = "{\"schema\":\"srt-telemetry.snapshot.v1\",\"metric_schema\":";
    integer(out, ROBOTWEAX_SRT_TELEMETRY_METRIC_SCHEMA_VERSION);
    out += ",\"generated_unix_nano\":";
    integer(out, view.generated_unix_nano);
    /* Milliseconds for JavaScript clients: nanoseconds exceed 2^53. */
    out += ",\"generated_unix_ms\":";
    integer(out, view.generated_unix_nano / 1'000'000);
    out += ",\"window_seconds\":";
    json_number(out, static_cast<double>(view.window_nano) / 1e9);
    out += ",\"sources\":[";
    bool first_source = true;
    for (const auto &s : view.sources)
    {
        out += first_source ? "" : ",";
        first_source = false;
        out += "{\"id\":";
        integer(out, s.id);
        out += ",\"role\":";
        json_string(out, role_name(s.role));
        out += ",\"labels\":{";
        bool first = true;
        for (const auto &[key, value] : s.labels)
        {
            out += first ? "" : ",";
            first = false;
            json_string(out, key);
            out += ':';
            json_string(out, value);
        }
        out += "},\"up\":";
        out += s.failed ? "false" : "true";
        out += ",\"connection\":";
        json_string(out, connection_name(s.connection));

        if (window_usable(s))
        {
            out += ",\"window\":{\"start_unix_nano\":";
            integer(out, s.window.start_unix_nano);
            out += ",\"end_unix_nano\":";
            integer(out, s.window.end_unix_nano);
            out += ",\"start_unix_ms\":";
            integer(out, s.window.start_unix_nano / 1'000'000);
            out += ",\"end_unix_ms\":";
            integer(out, s.window.end_unix_nano / 1'000'000);
            out += ",\"samples\":";
            integer(out, s.window.sample_count);
            out += ",\"quality\":[";
            first = true;
            for (const auto &f : window_flags)
            {
                if ((s.window.quality_flags & f.bit) != 0)
                {
                    out += first ? "" : ",";
                    first = false;
                    json_string(out, f.name);
                }
            }
            out += "],\"metrics\":{";
            first = true;
            for (std::size_t m = 0; m < names.size(); ++m)
            {
                const auto metric = static_cast<robotweax_srt_telemetry_metric>(m);
                if (!in_scope(s, names[m].scope) ||
                    (s.window.available_metrics & ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(metric)) == 0)
                {
                    continue;
                }
                const auto *info = robotweax_srt_telemetry_metric_info(metric);
                const auto &x = s.window.metrics[m];
                out += first ? "" : ",";
                first = false;
                json_string(out, names[m].otel);
                out += ":{\"unit\":";
                json_string(out, info->unit);
                bool inner = false;
                if (info->kind == ROBOTWEAX_SRT_TELEMETRY_METRIC_KIND_RATE)
                {
                    json_field(out, "rate", x.rate, inner);
                    json_field(out, "total", x.total, inner);
                }
                json_field(out, "min", x.min, inner);
                json_field(out, "max", x.max, inner);
                json_field(out, "mean", x.mean, inner);
                json_field(out, "last", x.last, inner);
                if ((x.flags & ROBOTWEAX_SRT_TELEMETRY_METRIC_SUMMARY_PERCENTILE_AVAILABLE) != 0)
                {
                    json_field(out, "p95", x.p95, inner);
                }
                out += '}';
            }
            out += "}}";
        }

        if (s.has_metrics)
        {
            out += ",\"latest\":{\"observed_unix_nano\":";
            integer(out, s.metrics.observed_at_unix_nano);
            out += ",\"observed_unix_ms\":";
            integer(out, s.metrics.observed_at_unix_nano / 1'000'000);
            out += ",\"metrics\":{";
            first = true;
            for (std::size_t m = 0; m < names.size(); ++m)
            {
                const auto metric = static_cast<robotweax_srt_telemetry_metric>(m);
                if (!in_scope(s, names[m].scope) ||
                    (s.metrics.available_metrics & ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(metric)) == 0)
                {
                    continue;
                }
                std::string text;
                if (!number(text, s.metrics.values[m]))
                {
                    continue;
                }
                out += first ? "" : ",";
                first = false;
                json_string(out, names[m].otel);
                out += ':';
                out += text;
            }
            out += "}}";
        }

        if ((s.has_raw || s.has_ext) && (view.flags & ROBOTWEAX_SRT_TELEMETRY_RENDER_NO_COUNTERS) == 0)
        {
            out += ",\"counters\":{";
            first = true;
            for (const auto &c : counters)
            {
                if (!in_scope(s, c.scope) || !counter_available(s, c))
                {
                    continue;
                }
                out += first ? "" : ",";
                first = false;
                json_string(out, c.otel);
                out += ':';
                integer(out, c.value(s));
            }
            out += '}';
        }

        if (s.has_health)
        {
            const auto &h = s.health;
            out += ",\"health\":{\"verdict\":";
            json_string(out, robotweax_srt_telemetry_health_verdict_name(h.verdict));
            out += ",\"reason\":";
            json_string(out, robotweax_srt_telemetry_health_reason_name(h.reason));
            /* A group member's verdict is capped at Watch; its own grade. */
            out += ",\"capped\":";
            out += (h.flags & ROBOTWEAX_SRT_TELEMETRY_HEALTH_STATUS_CAPPED) != 0 ? "true" : "false";
            out += ",\"uncapped_verdict\":";
            json_string(out, robotweax_srt_telemetry_health_verdict_name(h.uncapped_verdict));
            out += ",\"knob\":";
            json_string(out, robotweax_srt_telemetry_health_knob_name(h.knob));
            out += ",\"direction\":";
            json_string(out, direction_name(h.knob_direction));
            out += ",\"suggested_value\":";
            if (h.knob != ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE && h.suggested_value != 0)
            {
                json_number(out, h.suggested_value);
            }
            else
            {
                out += "null";
            }
            /* How long the verdict had held at the latest sample. */
            out += ",\"verdict_age_seconds\":";
            json_number(out, h.sample_monotonic_nano >= h.verdict_since_monotonic_nano
                                 ? static_cast<double>(h.sample_monotonic_nano - h.verdict_since_monotonic_nano) / 1e9
                                 : 0.0);
            out += ",\"primary_indicator\":";
            if (h.primary_indicator >= 0 && h.primary_indicator < ROBOTWEAX_SRT_TELEMETRY_INDICATOR_COUNT)
            {
                json_string(out, robotweax_srt_telemetry_health_indicator_name(
                                     static_cast<robotweax_srt_telemetry_health_indicator>(h.primary_indicator)));
            }
            else
            {
                out += "null";
            }
            out += ",\"missing_metrics\":[";
            first = true;
            for (std::size_t m = 0; m < names.size(); ++m)
            {
                if ((h.missing_metrics & ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(static_cast<robotweax_srt_telemetry_metric>(m))) !=
                    0)
                {
                    out += first ? "" : ",";
                    first = false;
                    json_string(out, names[m].otel);
                }
            }
            out += ']';
            if ((view.flags & ROBOTWEAX_SRT_TELEMETRY_RENDER_NO_INDICATORS) == 0)
            {
                out += ",\"indicators\":[";
                first = true;
                for (std::size_t i = 0; i < ROBOTWEAX_SRT_TELEMETRY_INDICATOR_COUNT; ++i)
                {
                    const auto &ind = h.indicators[i];
                    if ((ind.flags & ROBOTWEAX_SRT_TELEMETRY_INDICATOR_APPLICABLE) == 0)
                    {
                        continue;
                    }
                    out += first ? "" : ",";
                    first = false;
                    out += "{\"name\":";
                    json_string(out, robotweax_srt_telemetry_health_indicator_name(
                                         static_cast<robotweax_srt_telemetry_health_indicator>(i)));
                    out += ",\"state\":";
                    json_string(out, robotweax_srt_telemetry_health_verdict_name(ind.state));
                    out += ",\"reason\":";
                    json_string(out, robotweax_srt_telemetry_health_reason_name(ind.reason));
                    out += ",\"required\":";
                    out += (ind.flags & ROBOTWEAX_SRT_TELEMETRY_INDICATOR_REQUIRED) != 0 ? "true" : "false";
                    out += ",\"value\":";
                    if ((ind.flags & ROBOTWEAX_SRT_TELEMETRY_INDICATOR_AVAILABLE) != 0)
                    {
                        json_number(out, ind.value);
                    }
                    else
                    {
                        out += "null";
                    }
                    out += '}';
                }
                out += ']';
            }
            out += '}';
        }
        out += '}';
    }
    out += "]}\n";
    return out;
}

/* --------------------------------------------------------------- C ABI */

extern "C" const char *robotweax_srt_telemetry_metric_otel_name(const robotweax_srt_telemetry_metric metric)
{
    const auto index = static_cast<std::size_t>(metric);
    return index < names.size() ? names[index].otel : nullptr;
}

extern "C" const char *robotweax_srt_telemetry_metric_prometheus_name(const robotweax_srt_telemetry_metric metric)
{
    const auto index = static_cast<std::size_t>(metric);
    return index < names.size() ? names[index].prometheus : nullptr;
}

extern "C" const char *robotweax_srt_telemetry_metric_scope(const robotweax_srt_telemetry_metric metric)
{
    const auto index = static_cast<std::size_t>(metric);
    return index < names.size() ? names[index].scope : nullptr;
}

extern "C" void robotweax_srt_telemetry_render_options_default(const robotweax_srt_telemetry_render_format format,
                                                               robotweax_srt_telemetry_render_options_v1 *options)
{
    if (options == nullptr)
    {
        return;
    }
    std::memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
    options->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    options->format = format;
}

/* ------------------------------------------------------------------- OTLP */

namespace
{

/* OTLP/HTTP JSON (ExportMetricsServiceRequest, protobuf JSON mapping with
 * the OTLP rules: lowerCamelCase keys, 64-bit integers as strings, enums as
 * integers). */
constexpr int temporality_cumulative = 2;

void otlp_string_attribute(std::string &out, const std::string_view key, const std::string_view value, bool &first)
{
    out += first ? "" : ",";
    first = false;
    out += "{\"key\":";
    json_string(out, key);
    out += ",\"value\":{\"stringValue\":";
    json_string(out, value);
    out += "}}";
}

void otlp_int_attribute(std::string &out, const std::string_view key, const std::uint64_t value, bool &first)
{
    out += first ? "" : ",";
    first = false;
    out += "{\"key\":";
    json_string(out, key);
    out += ",\"value\":{\"intValue\":\"";
    integer(out, value);
    out += "\"}}";
}

struct OtlpWriter
{
    std::string &out;
    bool first_metric = true;
    bool first_point = true;
    bool open = false;

    /* Starts a metric; points follow; close() ends it. kind: "gauge" or
     * "sum" (monotonic, cumulative). */
    void begin(const char *name, const char *unit, const std::string &description, const bool sum)
    {
        out += first_metric ? "" : ",";
        first_metric = false;
        out += "{\"name\":";
        json_string(out, name);
        out += ",\"description\":";
        json_string(out, description);
        out += ",\"unit\":";
        json_string(out, unit);
        out += sum ? ",\"sum\":{\"aggregationTemporality\":" + std::to_string(temporality_cumulative) +
                         ",\"isMonotonic\":true,\"dataPoints\":["
                   : std::string(",\"gauge\":{\"dataPoints\":[");
        first_point = true;
        open = true;
    }

    void close()
    {
        out += "]}}";
        open = false;
    }

    /* A point: base attributes (already JSON), extra string attributes, then
     * the value. Non-finite doubles are left out. */
    void point(const std::string &attributes, const std::vector<std::pair<const char *, std::string>> &extra,
               const std::uint64_t start, const std::uint64_t time, const double value)
    {
        std::string number_text;
        if (!number(number_text, value))
        {
            return;
        }
        head(attributes, extra, start, time);
        out += ",\"asDouble\":";
        out += number_text;
        out += '}';
    }

    void point_int(const std::string &attributes, const std::vector<std::pair<const char *, std::string>> &extra,
                   const std::uint64_t start, const std::uint64_t time, const std::uint64_t value)
    {
        head(attributes, extra, start, time);
        out += ",\"asInt\":\"";
        integer(out, value);
        out += "\"}";
    }

  private:
    void head(const std::string &attributes, const std::vector<std::pair<const char *, std::string>> &extra,
              const std::uint64_t start, const std::uint64_t time)
    {
        out += first_point ? "" : ",";
        first_point = false;
        out += "{\"attributes\":[";
        out += attributes;
        bool first = attributes.empty();
        for (const auto &[key, value] : extra)
        {
            otlp_string_attribute(out, key, value, first);
        }
        out += ']';
        if (start != 0)
        {
            out += ",\"startTimeUnixNano\":\"";
            integer(out, start);
            out += '"';
        }
        out += ",\"timeUnixNano\":\"";
        integer(out, time);
        out += '"';
    }
};

const char *counter_unit(const char *otel_name)
{
    const std::string_view name = otel_name;
    return name.ends_with(".bytes") ? "By" : "{packet}";
}

} // namespace

std::string robotweax_srt_telemetry_internal::render_otlp_json(const ExportView &view)
{
    std::string out = "{\"resourceMetrics\":[{\"resource\":{\"attributes\":[";
    bool first = true;
    for (const auto &[key, value] : view.resource)
    {
        otlp_string_attribute(out, key, value, first);
    }
    /* Defaults, each unless the host set it (keys are unique). */
    static const std::pair<const char *, const char *> defaults[] = {
        {"service.name", "srt-telemetry"},
        {"telemetry.sdk.name", "robotweax-srt-telemetry"},
        {"telemetry.sdk.language", "cpp"},
        {"telemetry.sdk.version", ROBOTWEAX_SRT_TELEMETRY_VERSION}};
    for (const auto &[key, value] : defaults)
    {
        bool set = false;
        for (const auto &attribute : view.resource)
        {
            set = set || attribute.first == key;
        }
        if (!set)
        {
            otlp_string_attribute(out, key, value, first);
        }
    }
    out += "]},\"scopeMetrics\":[{\"scope\":{\"name\":\"robotweax.srt_telemetry\",\"version\":";
    json_string(out, ROBOTWEAX_SRT_TELEMETRY_VERSION);
    out += ",\"attributes\":[";
    first = true;
    otlp_int_attribute(out, "srt.metric_schema", ROBOTWEAX_SRT_TELEMETRY_METRIC_SCHEMA_VERSION, first);
    out += "]},\"metrics\":[";

    /* Attributes per source: source_id, role and the host labels, renamed
     * like the Prometheus labels so both outputs carry the same keys. */
    std::vector<std::string> attributes;
    attributes.reserve(view.sources.size());
    for (const auto &s : view.sources)
    {
        std::string a;
        bool f = true;
        otlp_int_attribute(a, "source_id", s.id, f);
        otlp_string_attribute(a, "role", role_name(s.role), f);
        for (const auto &[key, value] : exporter_labels(s.labels))
        {
            otlp_string_attribute(a, key, value, f);
        }
        attributes.push_back(std::move(a));
    }
    const auto now = view.generated_unix_nano;
    OtlpWriter w{out};
    const auto metric = [&](const char *name, const char *unit, const std::string &description, const bool sum,
                            auto &&points)
    {
        /* A metric with no points is left out. */
        const auto mark = out.size();
        const bool was_first = w.first_metric;
        w.begin(name, unit, description, sum);
        points();
        if (w.first_point)
        {
            out.resize(mark);
            w.first_metric = was_first;
            w.open = false;
            return;
        }
        w.close();
    };

    metric("srt.source.up", "{up}", "1 while the source's statistics read succeeds, 0 while it fails.", false,
           [&]
           {
               for (std::size_t i = 0; i < view.sources.size(); ++i)
               {
                   w.point_int(attributes[i], {}, 0, now, view.sources[i].failed ? 0 : 1);
               }
           });
    metric("srt.window.length", "s", "Length of the exported statistics window.", false,
           [&]
           {
               for (std::size_t i = 0; i < view.sources.size(); ++i)
               {
                   const auto &s = view.sources[i];
                   if (window_usable(s))
                   {
                       w.point(attributes[i], {}, s.window.start_unix_nano, s.window.end_unix_nano,
                               static_cast<double>(s.window.window_nano) / 1e9);
                   }
               }
           });
    metric("srt.window.samples", "{sample}", "Samples in the exported window.", false,
           [&]
           {
               for (std::size_t i = 0; i < view.sources.size(); ++i)
               {
                   const auto &s = view.sources[i];
                   if (window_usable(s))
                   {
                       w.point_int(attributes[i], {}, s.window.start_unix_nano, s.window.end_unix_nano,
                                   s.window.sample_count);
                   }
               }
           });
    metric("srt.window.quality", "{flag}", "1 when the exported window carries the data-quality flag.", false,
           [&]
           {
               for (std::size_t i = 0; i < view.sources.size(); ++i)
               {
                   const auto &s = view.sources[i];
                   if (!window_usable(s))
                   {
                       continue;
                   }
                   for (const auto &f : window_flags)
                   {
                       w.point_int(attributes[i], {{"flag", f.name}}, s.window.start_unix_nano,
                                   s.window.end_unix_nano, (s.window.quality_flags & f.bit) != 0 ? 1 : 0);
                   }
               }
           });

    for (std::size_t m = 0; m < names.size(); ++m)
    {
        const auto id = static_cast<robotweax_srt_telemetry_metric>(m);
        const auto *info = robotweax_srt_telemetry_metric_info(id);
        const bool rate = info->kind == ROBOTWEAX_SRT_TELEMETRY_METRIC_KIND_RATE;
        std::string help = names[m].help;
        help += rate ? " Window statistic in attribute stat: rate (exact over the window), max and p95 of 100 ms "
                       "samples."
                     : " Window statistic in attribute stat: min, max, mean, last, p95.";
        metric(names[m].otel, info->unit, help, false,
               [&]
               {
                   for (std::size_t i = 0; i < view.sources.size(); ++i)
                   {
                       const auto &s = view.sources[i];
                       if (!window_usable(s) || !in_scope(s, names[m].scope) ||
                           (s.window.available_metrics & ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(id)) == 0)
                       {
                           continue;
                       }
                       const auto &x = s.window.metrics[m];
                       const auto start = s.window.start_unix_nano;
                       const auto end = s.window.end_unix_nano;
                       if (rate)
                       {
                           w.point(attributes[i], {{"stat", "rate"}}, start, end, x.rate);
                           w.point(attributes[i], {{"stat", "max"}}, start, end, x.max);
                       }
                       else
                       {
                           w.point(attributes[i], {{"stat", "min"}}, start, end, x.min);
                           w.point(attributes[i], {{"stat", "max"}}, start, end, x.max);
                           w.point(attributes[i], {{"stat", "mean"}}, start, end, x.mean);
                           w.point(attributes[i], {{"stat", "last"}}, start, end, x.last);
                       }
                       if ((x.flags & ROBOTWEAX_SRT_TELEMETRY_METRIC_SUMMARY_PERCENTILE_AVAILABLE) != 0)
                       {
                           w.point(attributes[i], {{"stat", "p95"}}, start, end, x.p95);
                       }
                   }
               });
    }

    if ((view.flags & ROBOTWEAX_SRT_TELEMETRY_RENDER_NO_COUNTERS) == 0)
    {
        for (const auto &c : counters)
        {
            metric(c.otel, counter_unit(c.otel), c.help, true,
                   [&]
                   {
                       for (std::size_t i = 0; i < view.sources.size(); ++i)
                       {
                           const auto &s = view.sources[i];
                           if (in_scope(s, c.scope) && counter_available(s, c))
                           {
                               /* Cumulative since the counters started: the
                                * first sample, or the last counter reset. */
                               const auto start =
                                   s.counters_start_unix_nano != 0 ? s.counters_start_unix_nano : now;
                               w.point_int(attributes[i], {}, std::min(start, now), now, c.value(s));
                           }
                       }
                   });
        }
    }

    metric("srt.health.verdict", "{verdict}", "Health verdict: 0 unknown, 1 ok, 2 watch, 3 at risk, 4 failing.", false,
           [&]
           {
               for (std::size_t i = 0; i < view.sources.size(); ++i)
               {
                   const auto &s = view.sources[i];
                   if (s.has_health)
                   {
                       w.point_int(attributes[i], {}, 0, now, static_cast<std::uint64_t>(s.health.verdict));
                   }
               }
           });
    metric("srt.health.info", "{info}", "Current verdict, reason and suggested knob (value 1).", false,
           [&]
           {
               for (std::size_t i = 0; i < view.sources.size(); ++i)
               {
                   const auto &s = view.sources[i];
                   if (s.has_health)
                   {
                       w.point_int(attributes[i],
                                   {{"verdict", robotweax_srt_telemetry_health_verdict_name(s.health.verdict)},
                                    {"reason", robotweax_srt_telemetry_health_reason_name(s.health.reason)},
                                    {"knob", robotweax_srt_telemetry_health_knob_name(s.health.knob)},
                                    {"direction", direction_name(s.health.knob_direction)}},
                                   0, now, 1);
                   }
               }
           });
    metric("srt.health.suggested_value", "{value}",
           "Suggested value for the knob in srt.health.info, in the knob's unit (s, bit/s, bytes/s, percent).", false,
           [&]
           {
               for (std::size_t i = 0; i < view.sources.size(); ++i)
               {
                   const auto &s = view.sources[i];
                   if (s.has_health && s.health.suggested_value != 0)
                   {
                       w.point(attributes[i], {{"knob", robotweax_srt_telemetry_health_knob_name(s.health.knob)}}, 0,
                               now, s.health.suggested_value);
                   }
               }
           });
    if ((view.flags & ROBOTWEAX_SRT_TELEMETRY_RENDER_NO_INDICATORS) == 0)
    {
        for (const bool value : {false, true})
        {
            /* Units in braces are annotations: a state is not a ratio. */
            metric(value ? "srt.health.indicator.value" : "srt.health.indicator.state", value ? "1" : "{state}",
                   value ? "Indicator value (see docs/architecture/health-engine.md)."
                         : "Indicator state after hysteresis: 0 unknown, 1 ok, 2 watch, 3 at risk, 4 failing.",
                   false,
                   [&]
                   {
                       for (std::size_t i = 0; i < view.sources.size(); ++i)
                       {
                           const auto &s = view.sources[i];
                           if (!s.has_health)
                           {
                               continue;
                           }
                           for (std::size_t k = 0; k < ROBOTWEAX_SRT_TELEMETRY_INDICATOR_COUNT; ++k)
                           {
                               const auto &ind = s.health.indicators[k];
                               const auto needed = value ? ROBOTWEAX_SRT_TELEMETRY_INDICATOR_AVAILABLE
                                                         : ROBOTWEAX_SRT_TELEMETRY_INDICATOR_APPLICABLE;
                               if ((ind.flags & needed) == 0)
                               {
                                   continue;
                               }
                               const std::vector<std::pair<const char *, std::string>> extra = {
                                   {"indicator", robotweax_srt_telemetry_health_indicator_name(
                                                     static_cast<robotweax_srt_telemetry_health_indicator>(k))}};
                               if (value)
                               {
                                   w.point(attributes[i], extra, 0, now, ind.value);
                               }
                               else
                               {
                                   w.point_int(attributes[i], extra, 0, now, static_cast<std::uint64_t>(ind.state));
                               }
                           }
                       }
                   });
        }
    }
    out += "]}]}]}";
    return out;
}
