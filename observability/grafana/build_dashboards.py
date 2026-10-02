#!/usr/bin/env python3
"""Generates the SRT Telemetry Grafana dashboards (metric schema 1).

  python3 observability/grafana/build_dashboards.py

writes observability/grafana/dashboards/srt-fleet.json and srt-endpoint.json.
The dashboards query the Prometheus names of docs/architecture/metric-names.md;
every window metric selects one statistic (stat label). Edit this file, not
the JSON; tools/check_dashboards.py checks the JSON against the names.
"""

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "dashboards")

DS = {"type": "prometheus", "uid": "${datasource}"}
SEL = 'job=~"$job",instance=~"$instance"'
SRC = SEL + ',source_id=~"$source"'

# Status palette (docs/architecture/web-ui.md): the same colours as the UI.
COLORS = {"unknown": "#8a8984", "ok": "#0ca30c", "watch": "#fab219", "at_risk": "#ec835a", "failing": "#d03b3b"}
VERDICTS = [(0, "Unknown", "unknown"), (1, "OK", "ok"), (2, "Watch", "watch"), (3, "At risk", "at_risk"),
            (4, "Failing", "failing")]


# Reason codes in operator words (the built-in UI uses the same texts).
REASONS = {
    "none": "all clear", "no_data": "no data", "missing_metric": "metric missing",
    "missing_setting": "setting missing", "source_error": "statistics read failed", "connecting": "connecting",
    "connection_lost": "connection lost", "role_unknown": "role unknown",
    "recovery_headroom_low": "little recovery headroom", "latency_low_for_rtt": "latency short for the RTT",
    "unrecovered_loss": "unrecovered loss", "undecryptable": "undecryptable packets",
    "late_repairs": "repairs arrive late", "recovery_inefficient": "repairs failing",
    "overhead_budget_high": "recovery bandwidth used up", "path_capacity_exceeded": "above path capacity",
    "sender_backlog": "send buffer backlog", "sender_drops": "sender drops",
    "peer_read_stall": "receiver not reading", "application_read_stall": "application not reading",
    "acknowledgements_stalled": "no acknowledgements", "path_congested": "path congested",
}


def reason_mappings():
    return [{"type": "value", "options": {code: {"text": text, "index": i}
                                          for i, (code, text) in enumerate(REASONS.items())}}]


def verdict_mappings():
    return [{"type": "value", "options": {str(v): {"text": t, "color": COLORS[c], "index": v}
                                          for v, t, c in VERDICTS}}]


def verdict_thresholds():
    return {"mode": "absolute", "steps": [{"color": COLORS["unknown"], "value": None},
                                          {"color": COLORS["ok"], "value": 1},
                                          {"color": COLORS["watch"], "value": 2},
                                          {"color": COLORS["at_risk"], "value": 3},
                                          {"color": COLORS["failing"], "value": 4}]}


class Layout:
    """Places panels left to right on Grafana's 24-column grid."""

    def __init__(self):
        self.x = 0
        self.y = 0
        self.row_h = 0
        self.next_id = 1
        self.panels = []

    def add(self, panel, w, h):
        if self.x + w > 24:
            self.x = 0
            self.y += self.row_h
            self.row_h = 0
        panel["id"] = self.next_id
        self.next_id += 1
        panel["gridPos"] = {"x": self.x, "y": self.y, "w": w, "h": h}
        panel.setdefault("datasource", DS)
        self.panels.append(panel)
        self.x += w
        self.row_h = max(self.row_h, h)

    def row(self, title):
        self.x = 0
        self.y += self.row_h
        self.row_h = 0
        self.add({"type": "row", "title": title, "collapsed": False, "panels": []}, 24, 1)
        self.x = 0
        self.y += 1
        self.row_h = 0


def target(expr, legend="", ref="A", instant=False, fmt="time_series"):
    t = {"datasource": DS, "expr": expr, "legendFormat": legend, "refId": ref, "range": not instant,
         "instant": instant}
    if fmt != "time_series":
        t["format"] = fmt
    return t


def timeseries(title, targets, unit, description="", min0=True, max_value=None, stack=False):
    defaults = {"unit": unit, "custom": {"lineWidth": 2, "fillOpacity": 0, "spanNulls": False,
                                         "showPoints": "never", "axisSoftMin": 0 if min0 else None,
                                         "insertNulls": 3000, "stacking": {"mode": "normal" if stack else "none"}},
                "color": {"mode": "palette-classic"}}
    if max_value is not None:
        defaults["custom"]["axisSoftMax"] = max_value
    return {"type": "timeseries", "title": title, "description": description, "targets": targets,
            "fieldConfig": {"defaults": defaults, "overrides": []},
            "options": {"legend": {"displayMode": "list", "placement": "bottom", "showLegend": True},
                        "tooltip": {"mode": "multi", "sort": "desc"}}}


def name_label(expr):
    """Adds an "endpoint" label from the host label chosen in $name_label."""
    return f'label_replace({expr}, "endpoint", "$1", "$name_label", "(.+)")'


COMMON_VARS = [
    {"type": "datasource", "name": "datasource", "label": "Prometheus", "query": "prometheus",
     "current": {}, "hide": 0, "refresh": 1, "options": []},
    {"type": "query", "name": "job", "label": "Job", "datasource": DS,
     "query": {"query": "label_values(srt_telemetry_schema_info, job)", "refId": "job"},
     "definition": "label_values(srt_telemetry_schema_info, job)", "refresh": 2, "includeAll": True,
     "multi": True, "current": {"text": "All", "value": "$__all"}, "sort": 1},
    {"type": "query", "name": "instance", "label": "Instance", "datasource": DS,
     "query": {"query": 'label_values(srt_telemetry_schema_info{job=~"$job"}, instance)', "refId": "instance"},
     "definition": 'label_values(srt_telemetry_schema_info{job=~"$job"}, instance)', "refresh": 2,
     "includeAll": True, "multi": True, "current": {"text": "All", "value": "$__all"}, "sort": 1},
    {"type": "custom", "name": "name_label", "label": "Name label",
     "description": "The host label that names an endpoint (e.g. stream, channel).",
     "query": "stream,channel,name,stream_id", "current": {"text": "stream", "value": "stream"},
     "options": [], "hide": 0},
]


def dashboard(uid, title, description, layout, variables, links):
    return {
        "uid": uid, "title": title, "description": description, "tags": ["srt", "srt-telemetry"],
        "timezone": "browser", "editable": True, "graphTooltip": 1, "refresh": "10s", "schemaVersion": 41,
        "version": 1, "time": {"from": "now-30m", "to": "now"},
        "timepicker": {"refresh_intervals": ["5s", "10s", "30s", "1m", "5m"]},
        "templating": {"list": variables}, "annotations": {"list": []}, "links": links,
        "panels": layout.panels,
    }


# ---------------------------------------------------------------- fleet


def fleet():
    l = Layout()
    for index, (value, text, color) in enumerate(reversed(VERDICTS)):
        l.add({"type": "stat", "title": text,
               "targets": [target(f'count(srt_health_verdict{{{SEL}}} == {value}) or vector(0)', instant=True)],
               "fieldConfig": {"defaults": {"unit": "none", "decimals": 0, "color": {"mode": "thresholds"},
                                            "thresholds": {"mode": "absolute", "steps": [
                                                {"color": "#5c5b57", "value": None},
                                                {"color": COLORS[color], "value": 1}]},
                                            "mappings": []}, "overrides": []},
               "options": {"colorMode": "background" if value in (3, 4) else "value", "graphMode": "none",
                           "textMode": "value", "reduceOptions": {"calcs": ["lastNotNull"]},
                           "justifyMode": "center"}}, 5 if index < 4 else 4, 4)

    l.row("Endpoints, worst first")
    # One row per source: verdict, advice and key values, merged on labels.
    # Instant table queries merged into one row per source: each drops the
    # metric name and the stat label (max without), so only the source's
    # labels are common to all of them.
    def row(expr, ref):
        return target(f"max without (stat) ({expr})", ref=ref, instant=True, fmt="table")

    table_targets = [
        row(name_label(f'srt_health_verdict{{{SEL}}}'), "verdict"),
        row(f'label_join(srt_health_info{{{SEL}}}, "advice", " ", "direction", "knob")', "info"),
        row(f'srt_health_suggested_value{{{SEL}}}', "suggested"),
        row(f'srt_sender_unique_sent_bits_per_second{{{SEL},stat="rate"}} '
            f'or srt_receiver_unique_received_bits_per_second{{{SEL},stat="rate"}}', "bitrate"),
        row(f'srt_connection_rtt_seconds{{{SEL},stat="mean"}}', "rtt"),
        row(f'(srt_receiver_lost_packets_per_second{{{SEL},stat="rate"}} / '
            f'(srt_receiver_lost_packets_per_second{{{SEL},stat="rate"}} + '
            f'srt_receiver_unique_received_packets_per_second{{{SEL},stat="rate"}})) '
            f'or (srt_sender_loss_reported_packets_per_second{{{SEL},stat="rate"}} / '
            f'srt_sender_unique_sent_packets_per_second{{{SEL},stat="rate"}})', "loss"),
        row(f'srt_receiver_dropped_packets_per_second{{{SEL},stat="rate"}} '
            f'or srt_sender_dropped_packets_per_second{{{SEL},stat="rate"}}', "dropped"),
    ]
    l.add({"type": "table", "title": "Endpoints",
           "description": "Verdict and advice of the health engine, and the latest window's values. "
                          "Click a row to open the endpoint.",
           "targets": table_targets,
           "transformations": [
               {"id": "merge", "options": {}},
               {"id": "organize", "options": {
                   "includeByName": {"Value #verdict": True, "endpoint": True, "role": True, "source_id": True,
                                     "instance": True, "reason": True, "advice": True,
                                     "Value #suggested": True, "Value #bitrate": True, "Value #rtt": True,
                                     "Value #loss": True, "Value #dropped": True},
                   "indexByName": {"Value #verdict": 0, "endpoint": 1, "role": 2, "reason": 3, "advice": 4,
                                   "Value #suggested": 5, "Value #bitrate": 6, "Value #rtt": 7,
                                   "Value #loss": 8, "Value #dropped": 9, "instance": 10, "source_id": 11},
                   "renameByName": {"Value #verdict": "Verdict", "endpoint": "Endpoint", "role": "Role",
                                    "reason": "Reason", "advice": "Advice",
                                    "Value #suggested": "Suggested value", "Value #bitrate": "Bitrate",
                                    "Value #rtt": "RTT", "Value #loss": "Loss", "Value #dropped": "Dropped",
                                    "instance": "Instance", "source_id": "Source"}}},
               {"id": "sortBy", "options": {"sort": [{"field": "Verdict", "desc": True}]}},
           ],
           "fieldConfig": {"defaults": {"custom": {"align": "auto", "cellOptions": {"type": "auto"}},
                                        "mappings": [], "links": [{
                                            "title": "Open endpoint",
                                            "url": "/d/srt-endpoint/srt-endpoint?var-datasource=${datasource}"
                                                   "&var-job=${job}&var-instance=${__data.fields.Instance}"
                                                   "&var-source=${__data.fields.Source}&${__url_time_range}"}]},
                           "overrides": [
                               {"matcher": {"id": "byName", "options": "Verdict"},
                                "properties": [{"id": "mappings", "value": verdict_mappings()},
                                               {"id": "custom.cellOptions",
                                                "value": {"type": "color-background", "mode": "basic"}},
                                               {"id": "thresholds", "value": verdict_thresholds()}]},
                               {"matcher": {"id": "byName", "options": "Bitrate"},
                                "properties": [{"id": "unit", "value": "bps"}, {"id": "decimals", "value": 2}]},
                               {"matcher": {"id": "byName", "options": "RTT"},
                                "properties": [{"id": "unit", "value": "s"}, {"id": "decimals", "value": 3}]},
                               {"matcher": {"id": "byName", "options": "Loss"},
                                "properties": [{"id": "unit", "value": "percentunit"},
                                               {"id": "decimals", "value": 2}]},
                               {"matcher": {"id": "byName", "options": "Dropped"},
                                "properties": [{"id": "unit", "value": "pps"}, {"id": "decimals", "value": 1}]},
                               {"matcher": {"id": "byName", "options": "Suggested value"},
                                "properties": [{"id": "unit", "value": "short"}, {"id": "decimals", "value": 3},
                                               {"id": "description",
                                                "value": "In the knob's unit: s, bit/s, bytes/s or %."}]},
                               {"matcher": {"id": "byName", "options": "Reason"},
                                "properties": [{"id": "mappings", "value": reason_mappings()},
                                               {"id": "custom.minWidth", "value": 190}]},
                               {"matcher": {"id": "byName", "options": "Advice"},
                                "properties": [{"id": "mappings", "value": [{"type": "value", "options": {
                                    "none none": {"text": "—", "index": 0}}}]},
                                               {"id": "custom.minWidth", "value": 170}]},
                               {"matcher": {"id": "byName", "options": "Verdict"},
                                "properties": [{"id": "custom.minWidth", "value": 90}]},
                               {"matcher": {"id": "byName", "options": "Role"},
                                "properties": [{"id": "custom.width", "value": 90}]},
                               {"matcher": {"id": "byName", "options": "Source"},
                                "properties": [{"id": "custom.width", "value": 70}]},
                           ]},
           "options": {"showHeader": True, "cellHeight": "sm", "footer": {"show": False}}}, 24, 12)

    l.row("History")
    l.add({"type": "state-timeline", "title": "Verdict over time",
           "targets": [target(name_label(f'srt_health_verdict{{{SEL}}}'),
                              "{{endpoint}} {{role}} #{{source_id}}")],
           "fieldConfig": {"defaults": {"mappings": verdict_mappings(), "thresholds": verdict_thresholds(),
                                        "color": {"mode": "thresholds"}, "custom": {"fillOpacity": 85,
                                                                                   "lineWidth": 0}},
                           "overrides": []},
           "options": {"showValue": "never", "mergeValues": True, "rowHeight": 0.85,
                       "legend": {"showLegend": False}}}, 24, 10)
    l.add(timeseries("Bitrate", [
        target(name_label(f'srt_sender_unique_sent_bits_per_second{{{SEL},stat="rate"}}'),
               "{{endpoint}} sender #{{source_id}}", "A"),
        target(name_label(f'srt_receiver_unique_received_bits_per_second{{{SEL},stat="rate"}}'),
               "{{endpoint}} receiver #{{source_id}}", "B")], "bps",
        "Original packets sent, distinct packets received."), 12, 9)
    l.add(timeseries("Round-trip time", [
        target(name_label(f'srt_connection_rtt_seconds{{{SEL},stat="mean"}}'),
               "{{endpoint}} {{role}} #{{source_id}}")], "s"), 12, 9)
    l.add(timeseries("Unrecovered packets", [
        target(name_label(f'srt_receiver_dropped_packets_per_second{{{SEL},stat="rate"}}'),
               "{{endpoint}} receiver #{{source_id}}", "A"),
        target(name_label(f'srt_sender_dropped_packets_per_second{{{SEL},stat="rate"}}'),
               "{{endpoint}} sender #{{source_id}}", "B")], "pps",
        "Receiver: never delivered to the application. Sender: dropped as too late to send."), 12, 9)
    l.add(timeseries("Loss before repair", [
        target(name_label(f'srt_receiver_lost_packets_per_second{{{SEL},stat="rate"}} / '
                          f'(srt_receiver_lost_packets_per_second{{{SEL},stat="rate"}} + '
                          f'srt_receiver_unique_received_packets_per_second{{{SEL},stat="rate"}})'),
               "{{endpoint}} receiver #{{source_id}}")], "percentunit",
        "Share of packets detected missing at the receiver, before retransmission."), 12, 9)
    return dashboard("srt-fleet", "SRT fleet",
                     "Every SRT endpoint the selected exporters monitor: is it fine, or which knob?", l,
                     COMMON_VARS,
                     [{"title": "SRT endpoint", "type": "dashboards", "tags": ["srt-telemetry"],
                       "asDropdown": False, "includeVars": True, "keepTime": True}])


# ------------------------------------------------------------- endpoint


def endpoint():
    l = Layout()
    l.add({"type": "stat", "title": "Verdict",
           "targets": [target(f'srt_health_verdict{{{SRC}}}', instant=True)],
           "fieldConfig": {"defaults": {"mappings": verdict_mappings(), "thresholds": verdict_thresholds(),
                                        "color": {"mode": "thresholds"}}, "overrides": []},
           "options": {"colorMode": "background", "graphMode": "none", "textMode": "value",
                       "reduceOptions": {"calcs": ["lastNotNull"]}}}, 4, 5)
    l.add({"type": "stat", "title": "Reason",
           "targets": [target(f'srt_health_info{{{SRC}}}', "{{reason}}", instant=True)],
           "description": "Reason codes: docs/architecture/health-engine.md#reasons-and-advice.",
           "fieldConfig": {"defaults": {"color": {"mode": "fixed", "fixedColor": "text"}}, "overrides": []},
           "options": {"textMode": "name", "colorMode": "none", "graphMode": "none",
                       "reduceOptions": {"calcs": ["lastNotNull"]}}}, 6, 5)
    l.add({"type": "stat", "title": "Suggested change",
           "targets": [target(f'srt_health_info{{{SRC},knob!="none"}}', "{{direction}} {{knob}}",
                                     instant=True)],
           "fieldConfig": {"defaults": {"color": {"mode": "fixed", "fixedColor": "text"}, "noValue": "—"},
                           "overrides": []},
           "options": {"textMode": "name", "colorMode": "none", "graphMode": "none",
                       "reduceOptions": {"calcs": ["lastNotNull"]}}}, 6, 5)
    l.add({"type": "stat", "title": "Suggested value",
           "description": "In the knob's unit: latency in s, bitrate in bit/s, MAXBW in bytes/s, OHEADBW in %.",
           "targets": [target(f'srt_health_suggested_value{{{SRC}}}', "{{knob}}", instant=True)],
           "fieldConfig": {"defaults": {"color": {"mode": "fixed", "fixedColor": "text"}, "decimals": 3,
                                        "noValue": "—"},
                           "overrides": [
                               {"matcher": {"id": "byName", "options": "latency"},
                                "properties": [{"id": "unit", "value": "s"}]},
                               {"matcher": {"id": "byName", "options": "bitrate"},
                                "properties": [{"id": "unit", "value": "bps"}]},
                               {"matcher": {"id": "byName", "options": "max_bandwidth"},
                                "properties": [{"id": "unit", "value": "Bps"}]},
                               {"matcher": {"id": "byName", "options": "overhead_bandwidth"},
                                "properties": [{"id": "unit", "value": "percent"}]}]},
           "options": {"textMode": "value", "colorMode": "none", "graphMode": "none",
                       "reduceOptions": {"calcs": ["lastNotNull"]}}}, 4, 5)
    l.add({"type": "stat", "title": "Statistics",
           "targets": [target(f'srt_source_up{{{SRC}}}', instant=True)],
           "fieldConfig": {"defaults": {"mappings": [{"type": "value", "options": {
               "1": {"text": "reading", "color": COLORS["ok"]}, "0": {"text": "read failing",
                                                                      "color": COLORS["failing"]}}}],
               "color": {"mode": "thresholds"},
               "thresholds": {"mode": "absolute", "steps": [{"color": COLORS["failing"], "value": None},
                                                            {"color": COLORS["ok"], "value": 1}]}},
               "overrides": []},
           "options": {"colorMode": "value", "graphMode": "none", "textMode": "value",
                       "reduceOptions": {"calcs": ["lastNotNull"]}}}, 4, 5)
    l.add({"type": "table", "title": "Labels",
           "targets": [target(f'srt_source_up{{{SRC}}}', instant=True, fmt="table")],
           "transformations": [{"id": "organize", "options": {"excludeByName": {"Time": True, "Value": True,
                                                                               "__name__": True}}}],
           "options": {"showHeader": True, "cellHeight": "sm"}}, 24, 4)

    l.row("Health indicators")
    l.add({"type": "table", "title": "Indicators",
           "description": "Each indicator's state after hysteresis and its value "
                          "(see docs/architecture/health-engine.md).",
           "targets": [target(f'srt_health_indicator_state{{{SRC}}}', ref="state", instant=True, fmt="table"),
                       target(f'srt_health_indicator_value{{{SRC}}}', ref="value", instant=True, fmt="table")],
           "transformations": [
               {"id": "joinByField", "options": {"byField": "indicator", "mode": "outer"}},
               {"id": "organize", "options": {
                   "includeByName": {"indicator": True, "Value #state": True, "Value #value": True},
                   "indexByName": {"indicator": 0, "Value #state": 1, "Value #value": 2},
                   "renameByName": {"indicator": "Indicator", "Value #state": "State",
                                    "Value #value": "Value"}}}],
           "fieldConfig": {"defaults": {"decimals": 3}, "overrides": [
               {"matcher": {"id": "byName", "options": "State"},
                "properties": [{"id": "mappings", "value": verdict_mappings()},
                               {"id": "custom.cellOptions", "value": {"type": "color-background",
                                                                      "mode": "basic"}},
                               {"id": "thresholds", "value": verdict_thresholds()}]}]},
           "options": {"showHeader": True, "cellHeight": "sm"}}, 10, 9)
    l.add({"type": "state-timeline", "title": "Verdict over time",
           "targets": [target(f'srt_health_verdict{{{SRC}}}', "verdict", "A"),
                       target(f'srt_health_indicator_state{{{SRC}}}', "{{indicator}}", "B")],
           "fieldConfig": {"defaults": {"mappings": verdict_mappings(), "thresholds": verdict_thresholds(),
                                        "color": {"mode": "thresholds"},
                                        "custom": {"fillOpacity": 85, "lineWidth": 0}}, "overrides": []},
           "options": {"showValue": "never", "mergeValues": True, "rowHeight": 0.8,
                       "legend": {"showLegend": False}}}, 14, 9)

    l.row("History")
    l.add(timeseries("Throughput", [
        target(f'srt_sender_unique_sent_bits_per_second{{{SRC},stat="rate"}}', "sent (original)", "A"),
        target(f'srt_sender_retransmitted_bits_per_second{{{SRC},stat="rate"}}', "retransmitted", "B"),
        target(f'srt_receiver_unique_received_bits_per_second{{{SRC},stat="rate"}}', "delivered (distinct)",
               "C"),
        target(f'srt_receiver_received_bits_per_second{{{SRC},stat="rate"}}', "on the wire", "D")], "bps"),
        12, 9)
    l.add(timeseries("Loss and recovery", [
        target(f'srt_sender_loss_reported_packets_per_second{{{SRC},stat="rate"}}', "reported lost", "A"),
        target(f'srt_sender_dropped_packets_per_second{{{SRC},stat="rate"}}', "dropped (too late)", "B"),
        target(f'srt_sender_retransmitted_packets_per_second{{{SRC},stat="rate"}}', "retransmitted", "C"),
        target(f'srt_receiver_lost_packets_per_second{{{SRC},stat="rate"}}', "lost (before repair)", "D"),
        target(f'srt_receiver_dropped_packets_per_second{{{SRC},stat="rate"}}', "unrecovered", "E"),
        target(f'srt_receiver_retransmitted_packets_per_second{{{SRC},stat="rate"}}', "repairs received", "F")],
        "pps"), 12, 9)
    l.add(timeseries("Timing", [
        target(f'srt_connection_rtt_seconds{{{SRC},stat="mean"}}', "RTT", "A"),
        target(f'srt_sender_buffer_seconds{{{SRC},stat="max"}}', "send buffer", "B"),
        target(f'srt_receiver_buffer_seconds{{{SRC},stat="min"}}', "receive buffer", "C"),
        target(f'srt_receiver_latency_seconds{{{SRC},stat="last"}} or '
               f'srt_sender_peer_latency_seconds{{{SRC},stat="last"}}', "latency", "D")], "s",
        "Buffers against the latency: a receive buffer far below the latency leaves little time to repair."),
        12, 9)
    l.add(timeseries("Margins", [
        target(f'srt_receiver_recovery_headroom_ratio{{{SRC},stat="min"}}', "recovery headroom", "A"),
        target(f'srt_sender_buffer_drop_threshold_utilization_ratio{{{SRC},stat="max"}}', "drop limit used", "B"),
        target(f'srt_sender_flow_window_utilization_ratio{{{SRC},stat="max"}}', "flow window used", "C")],
        "percentunit", "Receiver: buffer ÷ latency (about 100 % when nothing is missing). "
                       "Sender: how close the send buffer is to dropping, and how full the peer is.",
        max_value=1), 12, 9)
    l.add(timeseries("Packets over the range", [
        target(f'increase(srt_receiver_lost_packets_total{{{SRC}}}[$__range])', "lost", "A", instant=True),
        target(f'increase(srt_receiver_dropped_packets_total{{{SRC}}}[$__range])', "unrecovered", "B",
               instant=True),
        target(f'increase(srt_sender_retransmitted_packets_total{{{SRC}}}[$__range])', "retransmitted", "C",
               instant=True),
        target(f'increase(srt_sender_dropped_packets_total{{{SRC}}}[$__range])', "dropped (too late)", "D",
               instant=True)], "short", "From the lifetime counters, over the dashboard's time range."), 12, 9)
    l.panels[-1]["type"] = "bargauge"
    l.panels[-1]["options"] = {"orientation": "horizontal", "displayMode": "basic", "showUnfilled": True,
                               "reduceOptions": {"calcs": ["lastNotNull"]}}

    single = []
    for v in COMMON_VARS:
        v = dict(v)
        if v["name"] in ("job", "instance"):
            v.update({"includeAll": False, "multi": False, "current": {}})
        single.append(v)
    variables = single + [
        {"type": "query", "name": "source", "label": "Source", "datasource": DS,
         "query": {"query": 'label_values(srt_source_up{job=~"$job",instance=~"$instance"}, source_id)',
                   "refId": "source"},
         "definition": 'label_values(srt_source_up{job=~"$job",instance=~"$instance"}, source_id)',
         "refresh": 2, "includeAll": False, "multi": False, "sort": 3, "current": {}}]
    return dashboard("srt-endpoint", "SRT endpoint",
                     "One SRT sender or receiver: verdict and advice, indicators, and its history.", l,
                     variables,
                     [{"title": "SRT fleet", "type": "link", "url": "/d/srt-fleet/srt-fleet?${__url_time_range}",
                       "icon": "dashboard"}])


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, build in (("srt-fleet.json", fleet), ("srt-endpoint.json", endpoint)):
        with open(os.path.join(OUT, name), "w", encoding="utf-8") as f:
            json.dump(build(), f, indent=2, ensure_ascii=False)
            f.write("\n")
        print("wrote", name)


if __name__ == "__main__":
    main()
