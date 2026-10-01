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
| Ratio | recovery headroom, flight / flow window, send buffer / sender drop threshold |

Rules:

- Rates exist only when the delta has a valid interval and the source counter
  is available in both samples. A baseline sample has no rates.
- `rate_numerators` keeps the exact counter delta behind each rate, so windows
  can report exact totals rather than averages of averages. A sample's whole
  interval counts toward the window that contains its timestamp; totals are
  therefore exact at sample boundaries, not at window edges. After a gap,
  flagged `SAMPLING_GAP`, a window's total can include time before its start.
- **Recovery headroom** = receive buffer span / receiver latency. It is about 1
  with no missing packet and falls toward 0 while a gap waits for repair; this
  is the normalised form of the "Buffer" figure in Haivision's gateways. It is
  computed only while at least 8 unique packets arrive per latency period.
  Below that, a single buffered packet would read as 0, so the metric is
  unavailable instead: the receive side of a pure sender, a trickle stream,
  or an outage. An outage is visible as a zero unique receive rate. During
  the first latency period after a stream starts, headroom ramps up from 0;
  the health engine ignores that phase.
- **Send buffer / sender drop threshold** compares the age of unacknowledged
  data with the point where the sender drops it. In both providers that is
  max(peer latency, 1 s) + 20 ms when `SRTO_SNDDROPDELAY` is 0, so 120 ms of
  latency still allows about 1 s of backlog. Values near 1 mean sender drops
  are imminent. It is computed only while data is sent or queued.
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

Window quality flags:

| Flag | Meaning |
| --- | --- |
| `PARTIAL` | emitted by flush, or the rest of a window that flush already reported |
| `SAMPLING_GAP` | an interval longer than twice the expected period |
| `CLOCK_STEP` | wall time went backwards, jumped by more than one period beyond the monotonic interval, or the monotonic clock regressed |
| `COUNTER_RESET` | a counter decreased in this window |
| `BASELINE` | first sample or provider change |

Empty windows are not emitted, so a stalled sampler shows as a gap rather than
as zeros.

A push accumulates the sample only after every window it closes has been
emitted. If the sink fails, the failed window stays open and the same sample
can be pushed again without being counted twice. Non-finite values are
ignored. The sink must not throw; an exception is treated as a sink failure.

## Cost

All buffers are allocated at creation; push and flush never allocate. p95
values are stored as 32-bit floats, about 4 bytes × p95 metrics × (window ÷
period) per window. With the defaults (seven p95 metrics, 100 ms period,
1 s / 10 s / 60 s windows) that is about 25 KB per socket. Hosts with many
sockets can narrow `percentile_metrics`, or set it to 0. If samples arrive
faster than the configured period, p95 uses the first samples that fit and is
flagged `PERCENTILE_TRUNCATED`. It is then computed from a reservoir sample,
so a burst late in the window can still reach it. min, max and mean always use
every sample.

`window_summary_v1` embeds 48 fixed-size `metric_summary_v1` entries, 32 of
them defined today. New metrics can use the free slots, but new per-metric
statistics such as p99 will need a `window_summary_v2`.
