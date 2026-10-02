# OTLP export

SRT Telemetry speaks OTLP in two ways:

- **Renderer** (core library): `robotweax_srt_telemetry_sampler_render_otlp_to()`
  writes the sampler's state as one OTLP/JSON `ExportMetricsServiceRequest`.
  Use it to send the metrics yourself, through any transport you already
  have.
- **Push exporter** (HTTP library, `srt_telemetry_otlp.h`): a thread that
  renders every interval and POSTs to an OpenTelemetry Collector's
  `/v1/metrics`.

The metric names are the OpenTelemetry names in
[Metric names](metric-names.md). Prometheus scraping of the embedded server
stays the reference path for the bundled dashboards and alert rules; OTLP is
for OpenTelemetry backends.

## What a request contains

One `resourceMetrics` entry:

- **Resource attributes:** the ones you pass (at most 32, unique keys, UTF-8
  without control characters, up to 255 bytes), plus defaults for each key
  you do not set: `service.name=srt-telemetry`, `telemetry.sdk.name`,
  `telemetry.sdk.language=cpp` and `telemetry.sdk.version`.
- **Scope** `robotweax.srt_telemetry` with the library version and the
  attribute `srt.metric_schema` (the metric schema version, currently 1).

Each data point carries `source_id`, `role` and the source's host labels as
attributes, with the same `host_` renaming of colliding keys as the
Prometheus labels.

| Kind | OTLP type | Data points |
| --- | --- | --- |
| Window rates, gauges and ratios | Gauge | one per statistic, attribute `stat` (`rate`, `min`, `max`, `mean`, `last`, `p95`), stamped at the window end |
| Lifetime counters | Sum, cumulative, monotonic | `startTimeUnixNano` is when the counters started: the connection time (sample time minus the provider's connection age) at the first sample or the latest counter reset, or that sample's time when the provider reports no age |
| Health, source and window state | Gauge | as in the Prometheus families, with the same attributes |

The encoding follows the OTLP/JSON rules: camelCase field names, 64-bit
integers and nanosecond timestamps as decimal strings, enums as integers.
Units are UCUM; counts use annotations (`{packet}`, `{verdict}`), so a
Collector that re-exports to Prometheus does not add a `_ratio` suffix.

The render flags `RENDER_NO_COUNTERS` and `RENDER_NO_INDICATORS` trim the
output as for Prometheus. The dashboard demo's ten sources render to about 60
metrics and 800 data points.

## Push exporter

```c
robotweax_srt_telemetry_label_v1 resource[] = {
    {"service.name", "encoder"}, {"service.instance.id", "encoder-7"}};
robotweax_srt_telemetry_otlp_exporter_config_v1 config;
robotweax_srt_telemetry_otlp_exporter_config_default(sampler, &config);
snprintf(config.endpoint, sizeof config.endpoint, "http://collector.local:4318");
config.resource_attributes = resource;
config.resource_attribute_count = 2;
robotweax_srt_telemetry_otlp_exporter *exporter;
if (robotweax_srt_telemetry_otlp_exporter_start(&config, &exporter) == ROBOTWEAX_SRT_TELEMETRY_OK) {
    /* ... */
    robotweax_srt_telemetry_otlp_exporter_stop(exporter); /* before the sampler */
}
```

| Field | Default | Range and notes |
| --- | --- | --- |
| `endpoint` | `http://127.0.0.1:4318/v1/metrics` | `http://host[:port][/path]`, IPv6 in brackets (a zone as `%25eth0`); no path means `/v1/metrics` |
| `headers` | empty | `key=value,key=value` as in `OTEL_EXPORTER_OTLP_HEADERS`, values percent-decoded (`authorization=Bearer%20abc`); `host`, `content-length`, `content-type`, `connection` and `transfer-encoding` are refused |
| `interval_ms` | 10000 | 100 ms to 1 h; the first export follows one interval after start |
| `timeout_ms` | 5000 | 100 ms to 60 s, per attempt (name lookup, connect, send, response) |
| `window_nano` | 0 (the default window) | must be a window the sampler keeps; checked at start |
| `resource_attributes` | none | at most 32, copied at start |

**Retries.** A connection error, a timeout, or the status 429, 502, 503 or 504
is retried up to twice, after 1 s and 2 s (at most a quarter of the interval
each). Retries only use what is left of the interval and are cut short by
it, so exports keep their cadence; the first attempt always gets its whole
timeout. Other statuses fail the export at once, including one the
collector sends before it has read the whole request. `Retry-After` is not
honoured. A failed export is dropped: the next
one carries current values, and the counters are cumulative, so nothing is
lost but resolution.

**Stats.** `robotweax_srt_telemetry_otlp_exporter_stats()` returns exports
accepted and failed, retries, bytes sent, the latest HTTP status and error,
and the time of the latest accepted export. Alert on a growing
`exports_failed` or a stale `last_success_unix_nano` in the host.

**Transport.** Plain HTTP/1.1 with `Connection: close`, one connection per
attempt, on POSIX sockets. There is no TLS in this library on purpose: send
to a Collector on the same host or a trusted network, and let it forward over
TLS with authentication (see `observability/otel-collector/config.yaml`).
The exporter refuses `https` endpoints instead of sending in clear text.

**Threading.** One thread per exporter. It takes the sampler lock only to
render (as a scrape does). A host name is resolved on a short-lived helper
thread, because a lookup cannot be interrupted; numeric addresses need none.
`stop()` abandons an attempt in progress, lookup included, within about
100 ms; an abandoned lookup finishes in the background and frees its result.
Call `stop()` before destroying the sampler, and not from a sampler callback.

## Tests

- `robotweax_srt_telemetry_export_tests`: the OTLP JSON of a fixed sampler
  state, checked field by field: resource and scope, gauges with `stat`,
  cumulative sums with their start time, health attributes, flags, and
  argument validation.
- `robotweax_srt_telemetry_otlp_proto_test` (when Python has
  `opentelemetry-proto`): `tools/otlp_validate.py` parses the output with the
  official protobuf definitions (unknown fields are errors) and checks types,
  units, temporality and timestamps.
- `robotweax_srt_telemetry_otlp_exporter_tests`, against a fake Collector:
  configuration validation (endpoints, IPv6 zones, percent-encoded headers),
  headers and body on the wire, resource attributes, retries on 503 and
  connection failure, giving up on 400, a host name through the resolver,
  and a prompt stop while the Collector does not answer. They pass under the
  thread and address sanitizers in local sanitizer builds (not in CI).
- CI end to end (`tools/observability_e2e.py`): the dashboard demo exports to
  a real OpenTelemetry Collector, and Prometheus finds the SRT metrics with
  the resource attributes on the Collector's endpoint.
