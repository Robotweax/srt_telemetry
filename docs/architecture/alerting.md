# Alert rules and Grafana dashboards

`observability/` holds what an operator needs to run SRT Telemetry with an
existing Prometheus and Grafana:

| Path | What it is |
| --- | --- |
| `prometheus/srt-telemetry-alerts.yml` | Alerting rules |
| `prometheus/srt-telemetry-alerts.test.yml` | Their unit tests (`promtool test rules`) |
| `prometheus/prometheus.yml` | Example scrape configuration |
| `grafana/dashboards/srt-fleet.json`, `srt-endpoint.json` | Dashboards (generated) |
| `grafana/build_dashboards.py` | Generates the dashboards |
| `grafana/provisioning/` | Example dashboard and data source provisioning |
| `otel-collector/config.yaml` | Example Collector configuration for the [OTLP exporter](otlp.md) |

`cmake --install` puts these files (without the generator) under
`share/robotweax_srt_telemetry/observability/`.

Everything targets the native Prometheus families of
[metric schema 1](metric-names.md), scraped from the
[embedded server](http-server.md)'s `/metrics`. Through an OpenTelemetry
Collector the names differ (see [Metric names](metric-names.md#otlp)).

## Setup

1. Scrape every process that runs the embedded server. A 10 s scrape interval
   matches the default window; a shorter one shows each window several times.
2. Add `srt-telemetry-alerts.yml` to `rule_files`, and route the alerts as
   usual in Alertmanager (`severity` is `critical` or `warning`).
3. In Grafana, import the two dashboards (or provision them from a
   directory) with a Prometheus data source. Grafana 12 (tested with 12.1).

## Alert rules

The rules build on the health engine's verdict, which already applies
hysteresis (worse after 2 s, better after 10 s), so the `for` durations only
ride out scrape jitter.

| Alert | Condition | For | Severity |
| --- | --- | --- | --- |
| `SRTStreamFailing` | verdict failing: the viewer is losing data, or the connection is lost | 10 s | critical |
| `SRTStreamAtRisk` | verdict at risk: a margin is close to gone | 30 s | warning |
| `SRTStreamUngraded` | verdict unknown while statistics read fine (no packets, a missing metric or setting, connecting) | 2 min | warning |
| `SRTSourceStatisticsFailing` | reading the socket's statistics fails, unless the connection is lost (a closed socket has no statistics; `SRTStreamFailing` covers it) | 1 min | warning |
| `SRTTelemetryTargetDown` | a target that exports SRT Telemetry stops answering scrapes | 1 min | critical |

Watch is not alerted on: it shows on the dashboards. Connection-group paths
are capped at Watch while another path is usable, so a cut path never alerts
on its own. The group's row does: a group on its last usable path is At risk
(`redundancy_lost`, `SRTStreamAtRisk`), and a group whose stream is hurt or
gone is Failing (`SRTStreamFailing`). A group that lost one path of three
is Watch (`path_lost`).

Each stream alert carries the source's labels (`source_id`, `role` and the
host labels such as `stream` or `site`). Its description says what the engine
advises, read from `srt_health_info` and `srt_health_suggested_value` when
the alert is evaluated, for the series with all of the alert's labels:

> The viewer is losing data, or the connection is lost. Reason:
> path_congested. Advice: decrease bitrate to 2.3Mbit/s.

The value is formatted in the knob's unit: latency as a duration, bitrate in
bit/s, MAXBW in B/s, OHEADBW in percent. `runbook_url` links to the reasons
and advice in [Health engine](health-engine.md#reasons-and-advice).

`SRTTelemetryTargetDown` is limited to targets that export SRT Telemetry, so
other jobs in the same Prometheus are left to their own rules. The recording
rule `srt_telemetry:target` marks them: a target keeps the mark while it is
down, however long, and loses it when it answers without SRT Telemetry or
leaves the scrape configuration.

**Through an OpenTelemetry Collector.** The health and source families keep
their names (see [Metric names](metric-names.md#otlp)), so the stream alerts
also work on metrics that arrive via OTLP. They match on all labels, so
several services behind one Collector, which share `job`, `instance` and
possibly `source_id`, stay apart by their resource labels
(`service_instance_id`). `SRTTelemetryTargetDown` cannot see a process that
stops pushing: watch the exporter's stats in the host, or the Collector's
own metrics.

## Dashboards

Both dashboards have the variables **Prometheus** (data source), **Job** and
**Instance** (from `srt_telemetry_schema_info`), and **Name label**: the host
label that names a stream (`stream` by default; `channel`, `name` or
`stream_id` are offered, and any label can be typed).

**SRT fleet** (`srt-fleet`) answers "is everything fine, and if not, what
do I change?":

- counts of endpoints per verdict;
- the **Endpoints** table, worst first: verdict, stream name, role, endpoint,
  reason in words, advice ("decrease bitrate"), suggested value, usable and
  expected paths (connection groups), bitrate, RTT, loss and drops. Each row
  links to the endpoint dashboard;
- history: verdicts over time, bitrate, RTT, unrecovered packets and loss
  before repair per endpoint;
- **Connection groups:** usable against expected paths per group over time,
  and each path's own grade over time.

A connection group counts as one endpoint: the counts, the table and the
history select `path=""`, so a group's paths (label `path`) appear only in
the Connection groups row and on the endpoint dashboard.

**SRT endpoint** (`srt-endpoint`, plus a **Source** variable) shows one sender
or receiver:

- verdict, reason, suggested change and value, and whether statistics read;
- its labels;
- for a connection group: usable and expected paths, each path's own grade
  over time, and each path's bitrate and RTT. The paths are found as the
  series with the group's labels plus `path` (`and ignoring (path,
  source_id)`); for other endpoints these panels stay empty;
- the health indicators with state and value, and the verdict and indicator
  states over time;
- throughput (original and retransmitted), loss and recovery, timing (RTT,
  buffers against latency), margins (drop limit and flow window used), and
  the packet counters over the selected range.

The dashboards are generated: edit `grafana/build_dashboards.py`, then run it.
They use the `stat` label on every window metric, and each table query is
wrapped in `max without (stat)` so its rows join.

## Tests

- `promtool test rules srt-telemetry-alerts.test.yml`: every alert fires and
  stays quiet when it should, with exact labels and descriptions
  (CI: the Haivision SRT adapter job).
- `tools/check_dashboards.py` (CI: the evidence job): every metric in the
  dashboards and rules is a known family, every window metric selects a
  `stat`, and the committed dashboards are what the generator produces. With
  `--prometheus URL`, every dashboard query must return data.
- `tools/observability_e2e.py` (CI: the Haivision SRT adapter job) starts the
  dashboard demo, an OpenTelemetry Collector, Prometheus with the rules and
  Grafana with the provisioned dashboards, and checks:
  - Prometheus scrapes the demo and the Collector, which receives the OTLP
    export with its resource attributes;
  - `SRTStreamFailing` fires with advice for the latency-starved
    (`remote-4g`) and capacity-capped (`uplink-3`) streams, both roles, and
    no Failing or At risk alert fires for the clean `studio-a`;
  - every dashboard query returns data;
  - Grafana loads both dashboards and answers a query through the
    provisioned data source.

  `--screenshots DIR` also captures both dashboards.
