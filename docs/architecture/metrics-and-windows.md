# Metrics and windows

Dashboards plot metrics, not raw counters. This layer turns each sample
(`raw_sample_v1`, optional `ext_sample_v1`, and their deltas) into one value per
metric, and aggregates those values into tumbling time windows. Header:
`robotweax/srt_telemetry_metrics.h`.

## Metric catalog

`robotweax_srt_telemetry_metric_info()` returns each metric's stable name, UCUM
unit, kind and role. Exporters and the web UI use these names.

| Kind | Metrics |
| --- | --- |
| Rate (from counter deltas) | sent, unique sent, retransmitted, received and unique received bit/s; unique sent/received packets/s; sender loss, receiver loss, retransmitted, received-retransmitted, sender drop, receiver drop, undecryptable, belated, FEC recovered, FEC unrecovered packets/s; NAKs sent/received per second |
| Gauge | RTT, send and receive buffer span, receiver and sender latency, flight size, flow window, available receive buffer, packet send period, link capacity (Haivision estimate only) |
| Ratio | recovery headroom, flight / flow window, send buffer / peer latency |

Rules:

- Rates exist only when the delta has a valid interval and the source counter
  is available in both samples. A baseline sample has no rates.
- `rate_numerators` keeps the exact counter delta behind each rate, so windows
  can report exact totals rather than averages of averages.
- **Recovery headroom** = receive buffer span / receiver latency. It is about 1
  with no missing packet and falls toward 0 while a gap waits for repair; this
  is the normalised form of the "Buffer" figure in Haivision's gateways. It is
  computed only while data arrives or is buffered. An idle receive direction,
  such as the receive side of a pure sender, is unavailable rather than a false
  zero.
- **Send buffer / peer latency** follows the same rule for the send direction.
- Robotweax's `mbpsBandwidth` (measured arrival rate) is never reported as link
  capacity.

## Windows

`robotweax_srt_telemetry_window_aggregator` keeps up to four tumbling windows,
1 s, 10 s and 60 s by default. Each is aligned to wall-clock multiples, so
windows from different sockets and hosts line up. Each closed window reports
per metric:

| Value | Meaning |
| --- | --- |
| `min`, `max`, `mean`, `last` | over every sample in the window |
| `p95` | nearest-rank 95th percentile, for the metrics in `percentile_metrics` |
| `total`, `rate` | rates only: exact sum of counter deltas, and that sum over the covered time |

Because every 100 ms sample counts toward `max` and `p95` at every window
length, a 100 ms burst still shows in the 60 s window. A Prometheus scrape every
15 s sees it too. Example from the unit test: one 100 ms burst of 1 Mbit inside
a second of 100 kbit samples gives a 1 s window with `rate` 1.9 Mbit/s and `max`
10 Mbit/s.

Window quality flags: `PARTIAL` (emitted by flush), `SAMPLING_GAP` (an interval
over twice the expected period), `CLOCK_STEP` (wall time went backwards or the
monotonic clock regressed), `COUNTER_RESET`, `BASELINE` (first sample or
provider change). Empty windows are not emitted, so a stalled sampler shows as a
gap rather than as zeros.

## Cost

All buffers are allocated at creation; push and flush never allocate. p95
values are stored as 32-bit floats, about 4 bytes × p95 metrics × (window ÷
period) per window. With the defaults (seven p95 metrics, 100 ms period,
1 s / 10 s / 60 s windows) that is about 25 KB per socket. Hosts with many
sockets can narrow `percentile_metrics`, or set it to 0. If samples arrive
faster than the configured period, p95 uses the first samples that fit and is
flagged `PERCENTILE_TRUNCATED`; min, max and mean still use every sample.
