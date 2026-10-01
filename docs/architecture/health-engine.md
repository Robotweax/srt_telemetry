# Health engine

The health engine turns one endpoint's metric samples into the answer an
operator wants from a dashboard: **is it fine, and if not, which knob?**
Header: `robotweax/srt_telemetry_health.h`. The sampler runs one engine per
source when health is enabled.

The verdict is descriptive and one-sided. It says what this endpoint sees and
which setting would help. It makes no causal claim about the far side;
explaining differences between the two ends is SRT Doctor's job.

## Verdicts

| Verdict | Meaning |
| --- | --- |
| OK | Every applicable indicator is within its thresholds, and the required one is graded. |
| Watch | Something moved: a single dropped packet, a margin narrowing. Nothing to change yet. |
| At risk | The stream works, but a margin is close to gone; the advice says what to change. |
| Failing | The viewer is losing data, or the connection is lost. |
| Unknown | No grade is possible: no packets, a missing metric, a failed statistics read, connecting, or an unknown role. The reason names which, and `missing_metrics` names the metrics. |

**Never a wrong OK.** OK needs the role's required indicator to be graded on
current data: the unrecovered ratio for a receiver, the sender backlog for a
sender. When nothing has arrived (or been sent) within the last gauge window,
the verdict becomes Unknown / `no_data` instead of keeping an old OK.

## Indicators

Each indicator is computed from this endpoint's own data and graded against
three thresholds (Watch, At risk, Failing).

| Indicator | Role | Computed as | Worse when | Default Watch / At risk / Failing |
| --- | --- | --- | --- | --- |
| Recovery headroom | receiver | receive buffer span ÷ latency, minimum over 1 s | lower | 0.6 / 0.35 / 0.15 |
| Latency sufficiency | both | latency ÷ RTT (RTT maximum over 1 s) | lower | 4 / 2.5 / 1.5 |
| Unrecovered ratio | receiver | dropped ÷ (unique received + dropped) over 10 s | higher | > 0 / 0.1 % / 1 % |
| Recovery efficiency | receiver | 1 − (dropped − undecryptable) ÷ loss detected over 10 s, graded from 10 lost packets | lower | 0.99 / 0.9 / 0.5 |
| Overhead budget | sender | retransmitted ÷ allowed overhead over 10 s; allowed = input × OHEADBW, or MAXBW − input | higher | 0.5 / 0.8 / 1.0 |
| Path headroom | sender | (unique + retransmitted) rate ÷ estimated link capacity over 1 s | higher | 0.75 / 0.9 / 1.05 |
| Sender backlog | sender | send buffer span ÷ too-late drop threshold, maximum over 1 s; any sender drop in 10 s is at least At risk, above 0.1 % Failing | higher | 0.35 / 0.6 / 0.9 |
| Peer read stall | both | sender: flight ÷ flow window; receiver: receive buffer span ÷ latency (data waiting for the application); maximum over 1 s | higher | sender 0.6 / 0.85 / 0.98; receiver 1.5 / 2.5 / 4 |

The defaults are defaults until field streams calibrate them (Phase 4).
Products override any of them with `health_engine_set_thresholds`, or
`sampler_set_health_thresholds` per role.

Notes:

- Packet counts use a 10 s sliding window, so a single dropped packet stays
  visible long enough to pass the hysteresis. Gauges use a 1 s window.
- Recovery headroom is not graded during the first 1.5 latency periods of a
  stream, when the buffer is still filling.
- Path headroom needs a provider that estimates link capacity (Haivision SRT);
  with Robotweax SRT it is unavailable and does not block an OK.
- The overhead budget needs the bandwidth settings (OHEADBW, MAXBW) in the
  context; with MAXBW unlimited it does not apply.

## Hysteresis

A worse state is entered after it has held for 2 s; the level entered is the
best one that held for the whole 2 s. A better state is entered after 10 s
without the worse one. Short dips never flip the verdict, and a verdict does
not flicker between two states. An indicator without data keeps its state for
10 s, then becomes Unknown. Two exceptions act at once: a lost connection
(Failing) and a failed statistics read (Unknown).

## Reasons and advice

The verdict carries the reason of its worst indicator (ties go to the one that
explains most: unrecovered ratio, sender backlog, recovery headroom, path
headroom, overhead budget, latency sufficiency, recovery efficiency, read
stall) and a suggested knob with a direction and a value.

| Reason | Knob | Suggested value |
| --- | --- | --- |
| `recovery_headroom_low`, `late_repairs`, `latency_low_for_rtt`, most `unrecovered_loss` | latency, increase | max(1.5 × current, 4 × RTT, or 6 × RTT above 1 % loss), in 10 ms steps |
| `unrecovered_loss` / `recovery_inefficient` with latency ≥ 8 × RTT and loss above 5 % | bitrate, decrease | 80 % of the received rate |
| `undecryptable` | passphrase, check | none |
| `overhead_budget_high` (relative MAXBW) | OHEADBW, increase | 1.5 × the retransmission share, in 5 % steps; above 100 % lower the bitrate instead |
| `overhead_budget_high` (absolute MAXBW) | MAXBW, increase | 1.25 × the sending rate |
| `path_capacity_exceeded` | bitrate, decrease | 70 % of the estimated capacity |
| `sender_backlog`, `sender_drops` | bitrate, decrease (path near capacity, or by default); latency, increase (latency short for the RTT) | 70 % of capacity, or 80 % of the sending rate |
| `peer_read_stall`, `application_read_stall` | receiving application, check | none |

## Context

The statistics cannot tell whether a socket is connected, or which bandwidth
settings apply. `health_context_v1` carries both. The public SRT adapter fills
it from a connection-monitor poll with
`robotweax_srt_telemetry_public_srt_health_context`. Pass the previous context
back in: Haivision SRT reports `OPENED` after a broken connection, and the
helper uses the previous condition to call that lost.

## Sampler integration

```c
robotweax_srt_telemetry_sampler_enable_health(sampler, 1);
robotweax_srt_telemetry_sampler_set_health_callback(sampler, on_health, user);
/* per connection-monitor poll */
robotweax_srt_telemetry_public_srt_health_context(&poll_result, &context);
robotweax_srt_telemetry_sampler_set_health_context(sampler, source_id, &context);
/* any time */
robotweax_srt_telemetry_sampler_health_read(sampler, source_id, &status);
```

- `on_health` fires when a source's verdict, reason or advice changes.
- Each change of verdict or reason is journaled as `verdict_changed`: previous
  and current verdict, reason as code, and a detail such as
  `at_risk recovery_headroom_low: latency increase 0.18`.
- Each compact history record holds the worst verdict of its second and that
  verdict's reason (`health_verdict`, `health_reason`).

## Tests

- `robotweax_srt_telemetry_health_tests`: API validation; clean streams OK at
  once and for 60 s; hysteresis timing (enter after 2 s, leave after 10 s,
  flicker ignored); synthetic receiver classes (repaired loss, single drop,
  unrepairable loss with late repairs, RTT step, undecryptable, application
  stall, arrivals stopping); synthetic sender classes (bandwidth cap with
  drops, path near capacity, overhead budget, peer read stall, RTT step, idle
  sender, missing settings); Unknown cases (missing drop counter, connecting,
  source error, unknown role, warm-up); the sampler path end to end (callback,
  journal, history verdicts, context, threshold overrides).
- `robotweax_srt_telemetry_public_srt_connection_tests`: health context from
  polls (connecting, connected with settings, OPENED after connected, broken).
- `robotweax_srt_telemetry_public_srt_sampler_test` (live, each provider): a
  clean loopback stream is OK on both ends; closing the caller turns the
  receiver Failing / `connection_lost` and journals it.

The fault-lab gate (right verdict in at least 9 of 10 fault classes per role,
never a wrong OK) runs on real SRT streams in the next part.
