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
| OK | Every applicable indicator is within its thresholds, and the required one is graded. Also a connected backup path waiting as standby (reason `standby`; see [Context](#context)). |
| Watch | Something moved: a single dropped packet, a margin narrowing. Nothing to change yet. Also the most a connection-group path shows (see [Context](#context)). |
| At risk | The stream works, but a margin is close to gone; the advice says what to change. |
| Failing | The viewer is losing data, or the connection is lost. |
| Unknown | No grade is possible: no packets, a missing metric, a failed statistics read, connecting, or an unknown role. The reason names which, and `missing_metrics` names the metrics. |

**Never a wrong OK.** OK needs the role's required indicator to be graded on
current data: the unrecovered ratio for a receiver, the sender backlog for a
sender. When nothing has arrived (or been sent) within the gauge window for
2 s more, the verdict becomes Unknown / `no_data` instead of keeping an old OK.
Shorter gaps, such as a trickle stream between packets, keep the verdict.
After such a gap, and at stream start, OK needs 2 s of fresh evidence. Any
escalation that begins in those 2 s finishes first, so the drops that arrive
with the first packets after an outage never follow an OK.

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
| Path headroom | sender | (unique + retransmitted) rate ÷ estimated link capacity over 1 s; graded only with congestion evidence (send buffer above its Watch level, sender drops, or the congestion signature) | higher | 1.25 / 1.5 / 2.0 |
| Sender backlog | sender | send buffer span ÷ too-late drop threshold, maximum over 1 s; any sender drop in 10 s is at least At risk, above 0.1 % Failing | higher | 0.35 / 0.6 / 0.9 |
| Peer read stall | both | sender: the larger of flight ÷ flow window and the share of the receive buffer held beyond 1.5 × (latency + RTT) of the stream (flow window against its largest value); receiver: receive buffer span ÷ latency (data waiting for the application); maximum over 1 s | higher | sender 0.25 / 0.5 / 0.9; receiver 1.5 / 2.5 / 4 |

A sender blames the receiving application only when the advertised flow window
shrank to under half its largest value within the last 10 s. Libsrt grows the
flow window by the packets a sender drops, so samples with sender drops do
not raise that reference. Without the latency in the statistics, occupancy is
not graded. Haivision SRT
breaks the connection once a stalled receiver's buffer overflows, and its flow
window recovers before that, so the evidence is kept for the count window.

Path headroom needs care: both providers' capacity estimate follows the
arrival rate. On a clean loopback path it swings between about 4.1 and
5.7 Mbit/s for a 5.4 Mbit/s stream, and 0–20 ms of jitter pulls it down to
3 Mbit/s on a path with room (fault lab). The ratio is therefore not evidence
on its own; it is graded only when this end also sees congestion.

The defaults were checked against the [fault lab](../qualification/fault-lab.md)
on loopback with both providers. They remain defaults until field streams
calibrate them (Phase 4).
Products override any of them with `health_engine_set_thresholds`, or
`sampler_set_health_thresholds` per role.

Notes:

- Packet counts use a 10 s sliding window, so a single dropped packet stays
  visible long enough to pass the hysteresis. Gauges use a 1 s window.
- Recovery headroom is not graded while the buffer first fills: for 1.5
  latency periods, at least one gauge window, after packets start (or restart
  after a gap of 2 s or three sampling intervals).
- Rates over the counts use the time the window actually covers, which is
  shorter than 10 s at the start of a stream or after an outage.
- Path headroom needs a provider that estimates link capacity (Haivision SRT);
  with Robotweax SRT it is unavailable and does not block an OK.
- The overhead budget needs the bandwidth settings (OHEADBW, MAXBW) in the
  context; with MAXBW unlimited it does not apply.

## Hysteresis

A worse state is entered after it has held for 2 s; the level entered is the
best one that held for the whole 2 s. A better state is entered after 10 s
without the worse one. Short dips never flip the verdict, and a verdict does
not flicker between two states. An indicator without data keeps its state for
10 s, then becomes Unknown; a pending escalation survives gaps shorter than
2 s, while a pending improvement needs unbroken evidence. An indicator that
stops applying (MAXBW set to unlimited) forgets its state. A lost connection
is Failing at once. Failed statistics reads keep the verdict (reason
`source_error`) for up to 2 s after the last good sample, then give Unknown.

## Reasons and advice

The verdict carries the reason of its worst indicator (ties go to the one that
explains most: unrecovered ratio, sender backlog, recovery headroom, path
headroom, overhead budget, latency sufficiency, recovery efficiency, read
stall) and a suggested knob with a direction and a value.

| Reason | Knob | Suggested value |
| --- | --- | --- |
| `recovery_headroom_low`, `late_repairs`, `latency_low_for_rtt`, most `unrecovered_loss` | latency, increase | max(1.5 × current, 4 × RTT, or 6 × RTT above 1 % loss), in 10 ms steps |
| any of the above while the path is congested, or in the aftermath of congestion: `path_congested` | bitrate, decrease | 80 % of what the path delivers (receiver: unique received rate, while a queue fills the rate that arrived meanwhile, at most the capacity estimate; sender: unique rate × (1 − reported loss share), while a queue fills the capacity estimate, or no value without one) |
| `unrecovered_loss` / `recovery_inefficient` with latency ≥ 8 × RTT and loss above 5 % in most samples | bitrate, decrease | 80 % of the received rate |
| `undecryptable` | passphrase, check | none |
| `overhead_budget_high` (relative MAXBW) | OHEADBW, increase | 1.5 × the retransmission share, in 5 % steps, at least 5 % above the current value; above 100 % (SRT's maximum) lower the bitrate instead |
| `overhead_budget_high` (absolute MAXBW) | MAXBW, increase | 1.25 × the sending rate, in bytes/s as SRTO_MAXBW takes it |
| `overhead_budget_high` within the count window (or the aftermath, if longer) of an acknowledgement stall: `acknowledgements_stalled` | none: the burst repairs an outage's backlog; the budget did not run short | none |
| `path_capacity_exceeded` | bitrate, decrease | 70 % of the estimated capacity |
| `sender_backlog`, `sender_drops` | by evidence, first match: flow window shrank → `peer_read_stall`, receiving application; sending at an absolute MAXBW → MAXBW, increase; above the estimated capacity with loss reports in the gauge window, a rising RTT or congestion, and not in an acknowledgement stall's aftermath → bitrate, decrease; latency short for the RTT → latency, increase; retransmissions above 10 % with loss reports in the gauge window, not in an acknowledgement stall's aftermath → bitrate, decrease; otherwise `acknowledgements_stalled` (retransmissions without loss reports are the sender's timers re-sending what nobody acknowledges; the burst of reports and repairs when the path returns is the outage's aftermath, and it inflates the rate against a stale estimate) | 1.25 × offered rate (sent + dropped) in bytes/s; 70 % of capacity; latency as above; 80 % of the unique rate |
| `peer_read_stall`, `application_read_stall` | receiving application, check | none |
| `acknowledgements_stalled` | none: data waits without loss reports, so the path or the peer is not acknowledging (an outage); no setting fixes it | none |
| receiver drops in the aftermath of a gap in arrivals longer than the latency (an outage), with no other drops in the count window | none: no retransmission recovers that gap | none |

**Drops are diagnosed when they happen.** Drops stay in the 10 s count
window after their cause has gone. Judged again with the evidence of a
healthy moment, they would get a wrong reason and knob (a Haivision receiver
advised "raise the latency" 10 s after a 6 s outage). So:

- **Outage aftermath.** The *aftermath* of an outage lasts 2 s + latency +
  the sender's drop threshold (max(latency, 1 s)) + two sampling intervals:
  packets due during the gap arrive late, and the sender's stale backlog
  drains.
  - *Receiver.* Each gap in arrivals (100 ms or longer, after the first
    packet) is remembered beyond the count window. Drops in the aftermath of
    a gap longer than the latency are the outage's: no knob. Drops outside
    it are judged as usual, even while an outage is in the window.
  - *Sender.* An acknowledgement stall is a filling send buffer with no other
    cause. It starts after a gauge window without drops, so a buffer still
    draining after drops is not a stall. It continues while that diagnosis
    holds, and while no loss reports arrive (reports mean the path
    acknowledges again: the burst on its return does not renew the stall).
    Drops in its aftermath are the outage's, including their burst of loss
    reports and retransmissions. A shrunk flow window or a MAXBW cap still
    gets its own advice.
- **Kept diagnosis.** The diagnosis of new drops is kept while they are
  counted.
  - *Receiver.* The drop indicators keep it until new drops come: after a
    lifted bandwidth cap the advice stays "lower the bitrate".
  - *Sender.* A backlog diagnosis rests on live gauges (flow window, rate,
    RTT), so a concrete one is always current. Only a weak one gives way to
    the kept diagnosis. Weak means heavy retransmission alone, or no cause
    at all.
  - *New drops with no cause.* They keep the last concrete diagnosis made
    within the count window, e.g. the backlog dropped after MAXBW was
    raised.
- **Resolved settings.** The kept diagnosis loses its knob once its setting
  has been changed as advised: an absolute MAXBW raised enough or replaced,
  or the latency raised enough after a reconnect. A sender then shows
  "sender drops" with nothing more to change.
- **Capacity needs evidence of a busy path.** The sender's "above the
  estimated capacity" advice needs loss reports, the congestion signature,
  or an RTT above 1.5 × its baseline + 5 ms (a queue). During an outage the
  estimate is stale and none of these show, so data waiting is
  acknowledgements stalled.

**Congestion.** The engine keeps a baseline RTT: the lowest RTT of loss-free
samples over the last 5 minutes, in 10 s buckets. Lossy samples never raise
it, so a congested path keeps its diagnosis for as long as it lasts. A route
change without loss is adopted within 5 minutes. The path counts as congested
when RTT exceeds 2 × baseline + 10 ms and loss above 5 % shows in most samples
of the count window (a short queue that drops), or when a deep queue fills:
over the last 3 s RTT rises by more than the baseline and at least 50 ms to
over 2 × baseline + 10 ms, the rise is spread over the span (at least a
quarter of it in each half), and RTT falls in at most 30 % of the sample
pairs, with or without loss. A step in RTT (a route change, also when
libsrt's smoothing spreads it over two or three samples) puts its whole rise
into one half; jitter falls as often as it rises; a slow drift does not add
50 ms in 3 s; none of them counts. The signature needs three RTT samples in
the span, so it works from 1 s sampling down; equal consecutive values (a
sampler reading faster than libsrt updates RTT) do not count against it. A
path that queues is not helped by more latency, so latency advice turns into
a bitrate decrease. That diagnosis also holds for the congestion's aftermath
(2 s + latency + max(latency, 1 s) + two sampling intervals, at most the
count window + max(latency, 1 s)): when the queue empties, the late packets,
drops and the low headroom they leave are the queue's, not a latency
problem. In the fault lab, a 3 Mbit/s cap on a 5.4 Mbit/s stream raises RTT
from 21 to 286 ms with 44 % loss. Both ends switch from latency advice to a
lower bitrate within about 1.5 s of the verdict: 2.5 Mbit/s at the sender,
2.7 Mbit/s at the receiver.

In the SRT Network Lab (srt_telemetry#27) a kernel-netem cap of 3 Mbit/s with
a 4000-packet queue fills without loss: RTT climbs about 430 ms per second
past the 2 s latency. Before the deep-queue signature both ends advised more
latency (up to 15 s); the fault lab's `bandwidth_cap_deep_queue` reproduces it
and now gets a lower bitrate at both ends about 3.3 s after onset. The same
lab's 2 s outage made the sender advise a lower bitrate once the path
returned (the burst of loss reports and repairs); `outage_2s_long_latency`
reproduces it, and the sender now reports the stall without a knob.

**What one end cannot know.** When a sender's MAXBW is set too low, the receiver
sees packets arrive late and headroom fall, and it advises more latency. Only
the sender can see its own setting. Explaining the difference between the two
ends is SRT Doctor's job.

## Context

The statistics cannot tell whether a socket is connected, or which bandwidth
settings apply. `health_context_v1` carries both. The public SRT adapter fills
it from a connection-monitor poll with
`robotweax_srt_telemetry_public_srt_health_context`. Pass the previous context
back in: Haivision SRT reports `OPENED` after a broken connection, and the
helper uses the previous condition to call that lost.

Two flags describe a member of an SRT connection group:

- `HEALTH_CONTEXT_GROUP_MEMBER` caps the verdict at Watch, because one path of
  several may fail without harm. The reason and advice stay. The status gets
  `HEALTH_STATUS_CAPPED`, and `uncapped_verdict` holds the path's own grade.
  A single socket is never capped.
- `HEALTH_CONTEXT_GROUP_STANDBY` (only with GROUP_MEMBER) marks a connected
  Main/Backup member that is not the active one. It carries only keepalives,
  so it is OK with reason `standby` rather than graded on an idle link: no
  data, or an RTT estimate without traffic (an idle SRT link measures no RTT
  and reports its initial estimate, about 100 ms). Its latency-based
  indicators (latency sufficiency, recovery headroom) keep no state while it
  waits, so no grade from the idle period comes along when it takes over;
  they start fresh and settle within about 2 s (the enter hold). Its loss and
  drop indicators keep grading, so a path that flaps between active and
  standby is still graded. A lost standby still reads lost.

  A connected standby reads OK even on an impaired link: SRT carries only
  keepalives on it, and notices a dead link only through their timeout.
  Whether a backup is ready shows only once it carries the stream (see the
  protocol findings on RTT measurement for standby links).

The embed sets both from the group session; see
[connection groups](embedding.md#connection-groups).

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

- `on_health` fires when a source's verdict, reason, knob or direction
  changes, or the suggested value moves by more than 10 %.
- Each change of verdict or reason is journaled as `verdict_changed`: previous
  and current verdict, reason as code, and a detail such as
  `at_risk recovery_headroom_low: latency increase 0.18`.
- Each compact history record holds the worst verdict of its second and that
  verdict's reason (`health_verdict`, `health_reason`). Unknown ranks between
  OK and Watch here, so a second is recorded as OK only if every graded sample
  in it was OK.
- The engine runs on the sampler's tick clock for good and failed samples.

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

- `robotweax_srt_telemetry_public_srt_fault_lab` (label `lab`, each provider):
  the [fault-lab gate](../qualification/fault-lab.md) on real SRT streams.
