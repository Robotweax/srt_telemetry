# Metric names

Every output (the streaming callbacks, Prometheus, the JSON snapshot, OTLP,
the WebSocket feed and the built-in UI) uses one naming scheme. Its version is
`ROBOTWEAX_SRT_TELEMETRY_METRIC_SCHEMA_VERSION` (currently 1), exported as
`srt_telemetry_schema_info{version="1"}` and as `metric_schema` in the JSON
snapshot. A released name never changes meaning. A change gets a new name and
a new schema version.

## Conventions

- **OpenTelemetry style** (`srt.<scope>.<name>`): used by the JSON snapshot,
  OTLP and the UI. The unit is a separate attribute, in UCUM.
- **Prometheus** (`srt_<scope>_<name>_<unit>`): base units (seconds, bytes,
  bits per second, packets per second, ratios), and `_total` for counters.
- **Scope:** `sender`, `receiver` or `connection` (both roles). A source
  exports the metrics of its own role and of the connection; a source of
  unknown role exports all of them.
- **Missing values:** a metric that is unavailable for a source is left out.
  It is never exported as zero.
- **C names:** `robotweax_srt_telemetry_metric_otel_name()`,
  `_metric_prometheus_name()` and `_metric_scope()` return the names for each
  `robotweax_srt_telemetry_metric`. Streaming callbacks use them to label
  their own outputs.

## Window metrics

Exported from the latest closed window: 10 s by default, any configured
length on request. Rates carry the exact window rate (window total ÷ covered
time) and the peak 100 ms rate. Gauges and ratios carry min, max, mean and
last. p95 is present where it is computed (by default for bitrates, RTT,
buffers and recovery headroom). In Prometheus the statistic is the `stat`
label. In JSON each metric is an object with one key per statistic, and for
rates also `total`.

| OpenTelemetry name | Prometheus family | Unit | Kind | Statistics | Meaning |
| --- | --- | --- | --- | --- | --- |
| `srt.sender.sent.bitrate` | `srt_sender_sent_bits_per_second` | bit/s | rate | rate, max, p95 | Bits sent on the wire, original and retransmitted. |
| `srt.sender.unique_sent.bitrate` | `srt_sender_unique_sent_bits_per_second` | bit/s | rate | rate, max, p95 | Bits of original (first-time) packets sent. |
| `srt.sender.retransmitted.bitrate` | `srt_sender_retransmitted_bits_per_second` | bit/s | rate | rate, max, p95 | Bits of retransmitted packets sent. |
| `srt.receiver.received.bitrate` | `srt_receiver_received_bits_per_second` | bit/s | rate | rate, max, p95 | Bits received on the wire, including duplicates and retransmissions. |
| `srt.receiver.unique_received.bitrate` | `srt_receiver_unique_received_bits_per_second` | bit/s | rate | rate, max, p95 | Bits of distinct packets received. |
| `srt.sender.unique_sent.packet_rate` | `srt_sender_unique_sent_packets_per_second` | {packet}/s | rate | rate, max, p95 | Original packets sent. |
| `srt.receiver.unique_received.packet_rate` | `srt_receiver_unique_received_packets_per_second` | {packet}/s | rate | rate, max, p95 | Distinct packets received. |
| `srt.sender.loss_reported.packet_rate` | `srt_sender_loss_reported_packets_per_second` | {packet}/s | rate | rate, max, p95 | Packets the receiver reported lost (NAK). |
| `srt.receiver.lost.packet_rate` | `srt_receiver_lost_packets_per_second` | {packet}/s | rate | rate, max, p95 | Packets detected missing at the receiver (before repair). |
| `srt.sender.retransmitted.packet_rate` | `srt_sender_retransmitted_packets_per_second` | {packet}/s | rate | rate, max, p95 | Packets retransmitted. |
| `srt.receiver.retransmitted.packet_rate` | `srt_receiver_retransmitted_packets_per_second` | {packet}/s | rate | rate, max, p95 | Retransmitted packets received. |
| `srt.sender.dropped.packet_rate` | `srt_sender_dropped_packets_per_second` | {packet}/s | rate | rate, max, p95 | Packets the sender dropped as too late to send (not network loss). |
| `srt.receiver.dropped.packet_rate` | `srt_receiver_dropped_packets_per_second` | {packet}/s | rate | rate, max, p95 | Packets never delivered to the application (skipped), including undecryptable ones. |
| `srt.receiver.undecryptable.packet_rate` | `srt_receiver_undecryptable_packets_per_second` | {packet}/s | rate | rate, max, p95 | Packets that could not be decrypted. |
| `srt.receiver.belated.packet_rate` | `srt_receiver_belated_packets_per_second` | {packet}/s | rate | rate, max, p95 | Packets that arrived after their play time. |
| `srt.receiver.fec_recovered.packet_rate` | `srt_receiver_fec_recovered_packets_per_second` | {packet}/s | rate | rate, max, p95 | Packets rebuilt by the packet filter (FEC). |
| `srt.receiver.fec_unrecovered.packet_rate` | `srt_receiver_fec_unrecovered_packets_per_second` | {packet}/s | rate | rate, max, p95 | Packets the packet filter (FEC) could not rebuild. |
| `srt.receiver.nak_sent.packet_rate` | `srt_receiver_nak_sent_packets_per_second` | {packet}/s | rate | rate, max, p95 | Loss reports (NAK packets) sent. |
| `srt.sender.nak_received.packet_rate` | `srt_sender_nak_received_packets_per_second` | {packet}/s | rate | rate, max, p95 | Loss reports (NAK packets) received. |
| `srt.connection.rtt` | `srt_connection_rtt_seconds` | s | gauge | min, max, mean, last, p95 | Smoothed round-trip time. |
| `srt.sender.buffer.duration` | `srt_sender_buffer_seconds` | s | gauge | min, max, mean, last, p95 | Time span of data in the send buffer. |
| `srt.receiver.buffer.duration` | `srt_receiver_buffer_seconds` | s | gauge | min, max, mean, last, p95 | Time span of data in the receive buffer. |
| `srt.receiver.latency` | `srt_receiver_latency_seconds` | s | gauge | min, max, mean, last, p95 | Receiver latency (TSBPD delay). |
| `srt.sender.peer_latency` | `srt_sender_peer_latency_seconds` | s | gauge | min, max, mean, last, p95 | Latency the receiver applies, as negotiated. |
| `srt.sender.flight_size` | `srt_sender_flight_size_packets` | {packet} | gauge | min, max, mean, last, p95 | Packets sent and not yet acknowledged. |
| `srt.sender.flow_window` | `srt_sender_flow_window_packets` | {packet} | gauge | min, max, mean, last, p95 | Free receive buffer the peer advertises, in packets. |
| `srt.receiver.buffer.available` | `srt_receiver_buffer_available_bytes` | By | gauge | min, max, mean, last, p95 | Free space in the receive buffer. |
| `srt.sender.packet_send_period` | `srt_sender_packet_send_period_seconds` | s | gauge | min, max, mean, last, p95 | Interval between packets set by the sender's pacing. |
| `srt.connection.link_capacity` | `srt_connection_link_capacity_bits_per_second` | bit/s | gauge | min, max, mean, last, p95 | Link capacity estimate (Haivision SRT; follows the arrival rate). |
| `srt.receiver.recovery_headroom` | `srt_receiver_recovery_headroom_ratio` | 1 | ratio | min, max, mean, last, p95 | Receive buffer span / latency: time left to repair a gap, normalised; about 1 when nothing is missing. |
| `srt.sender.flight_to_flow_window` | `srt_sender_flight_to_flow_window_ratio` | 1 | ratio | min, max, mean, last, p95 | Flight size / flow window: near 1 when the peer stops taking data. |
| `srt.sender.buffer.drop_threshold_ratio` | `srt_sender_buffer_to_drop_threshold_ratio` | 1 | ratio | min, max, mean, last, p95 | Send buffer span / too-late drop threshold: near 1 just before sender drops. |

## Lifetime counters

Totals since the connection started, read from the latest sample. A new
connection starts at zero; Prometheus treats that as a counter reset.

| OpenTelemetry name | Prometheus family | Meaning |
| --- | --- | --- |
| `srt.sender.unique_sent.packets` | `srt_sender_unique_sent_packets_total` | Original packets sent since the connection started. |
| `srt.sender.unique_sent.bytes` | `srt_sender_unique_sent_bytes_total` | Bytes of original packets sent. |
| `srt.sender.retransmitted.packets` | `srt_sender_retransmitted_packets_total` | Packets retransmitted. |
| `srt.sender.loss_reported.packets` | `srt_sender_loss_reported_packets_total` | Packets reported lost by the receiver. |
| `srt.sender.dropped.packets` | `srt_sender_dropped_packets_total` | Packets dropped by the sender as too late. |
| `srt.receiver.unique_received.packets` | `srt_receiver_unique_received_packets_total` | Distinct packets received. |
| `srt.receiver.unique_received.bytes` | `srt_receiver_unique_received_bytes_total` | Bytes of distinct packets received. |
| `srt.receiver.lost.packets` | `srt_receiver_lost_packets_total` | Packets detected missing (before repair). |
| `srt.receiver.dropped.packets` | `srt_receiver_dropped_packets_total` | Packets never delivered to the application. |
| `srt.receiver.undecryptable.packets` | `srt_receiver_undecryptable_packets_total` | Packets that could not be decrypted. |

## Health

| Prometheus family | JSON (`health`) | Meaning |
| --- | --- | --- |
| `srt_health_verdict` | `verdict` | 0 unknown, 1 ok, 2 watch, 3 at risk, 4 failing |
| `srt_health_info{verdict,reason,knob,direction}` | `verdict`, `reason`, `knob`, `direction` | Current verdict with its reason and suggested knob (value 1) |
| `srt_health_suggested_value{knob}` | `suggested_value` | Value for the knob in its unit: s, bit/s, bytes/s or percent |
| `srt_health_indicator_state{indicator}` | `indicators[].state` | Each indicator after hysteresis |
| `srt_health_indicator_value{indicator}` | `indicators[].value` | Each indicator's value (null when unavailable) |
| — | `primary_indicator`, `missing_metrics`, `since_monotonic_nano` | Indicator behind the verdict; metrics a verdict needed but did not get |

Reason, knob and indicator names are those of
`robotweax_srt_telemetry_health_*_name()`
(see [Health engine](health-engine.md)).

## Source and window

| Prometheus family | JSON | Meaning |
| --- | --- | --- |
| `srt_source_up` | `up` | 1 while statistics reads succeed |
| — | `connection` | `connected`, `connecting`, `lost` or `unreported` (from the health context) |
| `srt_window_seconds` | `window_seconds` (top level) | Length of the exported window |
| `srt_window_samples` | `window.samples` | Samples in it |
| `srt_window_quality{flag}` | `window.quality` | `partial`, `sampling_gap`, `clock_step`, `counter_reset`, `baseline`, `source_error` |

## Labels

Every Prometheus series carries `source_id`, `role` and the source's host
labels. A host label whose key collides with an exporter label (`source_id`,
`role`, `stat`, `flag`, `indicator`, `verdict`, `reason`, `knob`, `direction`,
`job`, `instance`, or any key starting with `__`) is renamed `host_<key>`. In
JSON, host labels appear unchanged under `labels`.

## JSON snapshot

`schema` is `srt-telemetry.snapshot.v1`. Top level: `metric_schema`,
`generated_unix_nano` (time of the latest tick), `window_seconds`, `sources`.
Each source has:

- `id`, `role`, `labels`, `up`, `connection`;
- `window` (when a window has closed): `start_unix_nano`, `end_unix_nano`,
  `samples`, `quality`, `metrics`;
- `latest` (the latest sample): `observed_unix_nano`, `metrics`;
- `counters`;
- `health` (when health is enabled).

A window that ended more than two window lengths before the latest tick is
left out as stale.

## Size

Two sources (one sender, one receiver) render to about 25 KB of Prometheus
text, roughly 75 series per source. For large fleets, the flags
`RENDER_NO_COUNTERS` and `RENDER_NO_INDICATORS` trim the output. Phase 4
measures scrape cost at 1,000 sockets.

## Tests

- `robotweax_srt_telemetry_export_tests`:
  - names: unique, scoped and unit-suffixed, and all listed in this document;
  - Prometheus output parsed line by line: syntax, one HELP and TYPE per
    family before its samples, no duplicate series;
  - label escaping and renamed reserved keys;
  - role scoping, exact window rates, lifetime counters, health series and
    flags;
  - the JSON snapshot checked by a strict parser;
  - the buffer variant (size query, too small, exact) and a writer that calls
    the sampler.
- `robotweax_srt_telemetry_public_srt_sampler_test` (live, each provider):
  both formats render the sender and receiver of a real SRT stream.
