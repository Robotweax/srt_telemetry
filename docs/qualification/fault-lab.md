# Fault lab

The fault lab checks the [health engine](../architecture/health-engine.md) on
real SRT streams, with each provider, against the Phase 2 gate:

> the right verdict in at least 90 % of the lab fault classes per role, and never
> a wrong OK.

Executable: `robotweax_srt_telemetry_public_srt_fault_lab` (ctest label `lab`,
built with the public SRT adapter). A full run takes about seven and a half
minutes. CI runs it for each provider after the other tests and keeps the
report as an artifact.

```sh
fault_lab                    # all scenarios, gate verdict, exit 0 on pass
fault_lab --list             # scenario names and descriptions
fault_lab --scenario rtt_step --timeline   # one scenario, verdict timeline
fault_lab --scenario bandwidth_cap --trace # plus per-second metrics
```

## Setup

Each scenario connects a fresh SRT caller (the sender) to a listener (the
receiver) through an impairing UDP relay inside the same process. The caller
sends 500 packets/s of 1316 bytes (about 5.3 Mbit/s) unless noted. The relay
adds 10 ms in each direction (20 ms RTT) and applies the fault:
- loss, or loss bursts;
- jitter;
- a rate cap with a tail-drop queue;
- a blackhole.

Loss, jitter and the rate cap apply to the forward direction; delay and
blackholes apply to both. The random generator has a fixed seed.

Both sockets get a sampler source (100 ms period) with health enabled, and a
connection monitor that feeds the health context. The lab reads both verdicts
every 100 ms.

Timeline per scenario: 6 s clean warm-up, the fault (12 s unless noted), then a
settle period (4 s unless noted).

No kernel netem is needed, so the lab runs unprivileged in CI and on any
developer machine.

## Scenarios

**Controls** must be OK at every read after warm-up, with no Unknown:

| Control | Condition |
| --- | --- |
| `clean` | no impairment, 60 s |
| `rtt_rise_healthy` | RTT rises from 20 to 60 ms with 400 ms latency, no loss |
| `high_bitrate` | 2500 packets/s (26 Mbit/s), no impairment |

**Fault classes.** "Fine" means OK or Watch; "bad" means At risk or Failing.

| Fault class | Fault | Sender expects | Receiver expects | Right knob, sender / receiver |
| --- | --- | --- | --- | --- |
| `random_loss_2pct` | 2 % random loss, 200 ms latency | fine | fine | — |
| `burst_loss` | 30-packet burst every 2 s | fine | fine | — |
| `jitter_reorder` | 0–20 ms jitter (heavy reordering) | fine | fine | — |
| `heavy_loss_tight_latency` | 15 % loss, 60 ms latency on 20 ms RTT | Watch or worse | bad | latency / latency |
| `rtt_step` | RTT 20 → 140 ms, 200 ms latency | bad | bad | latency / latency |
| `bandwidth_cap` | 3 Mbit/s cap, 100 kB queue, 40 s | bad | bad | bitrate / bitrate |
| `outage_6s` | blackhole for 6 s (peer idle timeout 10 s, settle 20 s) | bad | bad | none / none |
| `receiver_stall` | receiving application stops reading (3 MB receive buffer) | bad | bad | receiving application / receiving application |
| `maxbw_too_low` | sender SRTO_MAXBW 2.8 Mbit/s (reset at fault end) | bad | bad | MAXBW (after the reset: MAXBW or none) / not judged |
| `outage_long_latency` | blackhole for 6 s with 2.5 s latency (settle 20 s): drops come later after the path returns | bad | bad | none / none |
| `outage_then_cap` | blackhole for 4 s, then a 3 Mbit/s cap for 30 s | bad | bad | none, then (from 14 s) bitrate / same |
| `connection_lost` | blackhole for good | Failing | Failing | — |

Three of the twelve fault classes (loss, bursts, jitter) are false-alarm checks:
a role passes them by staying OK or Watch. Seven classes require detection.

Where the expectations came from:
- **Verdict sets:** written from what each end can observe before the first
  run. The sender's "Watch or worse" for heavy loss reflects what a sender can
  know: it sees loss reports and a latency of 3 × RTT, but not whether the
  viewer lost data.
- **Knob expectations:** added after the third calibration run, once advice
  was judged.
- **Outage:** the original 3 s outage was replaced after review, because its
  fault ended before the detection grace, so a wrong OK could not be scored.
  The receiver's knob was tightened from "none or latency" to "none".

## Scoring

For each role:

- **Verdict:** the worst verdict from fault onset to the end of the settle
  period must be in the expected set.
- **Wrong OK:** a role whose expected set excludes OK must not report OK on
  three consecutive reads in the scored window. The window runs from 4 s after
  onset (1 s gauge window + 2 s hysteresis + 1 s margin) until 2 s after the
  fault ends. Those last 2 s cover the aftermath, such as drops that arrive
  with the first packets after an outage.
- **Advice:** every At risk or Failing read must name one of the right knobs.
  This applies from 4 s after the first such read (the same grace as for
  detection) until the end of the settle period, so the aftermath is scored
  too. The 4 s let advice sharpen while evidence builds: a bandwidth cap first
  looks like a latency problem, then like congestion (3.4 s on Haivision
  SRT). Reads after a lost connection have no knob and are not
  judged.

**Gate:** for each role:
- at least 90 % of the fault classes right (11 of 12);
- no wrong OK;
- every control OK;
- right advice in every fault class that has a knob expectation.

## Results (2 October 2026, loopback, this cloud workspace)

With the final code: two consecutive full runs per provider with advice
scored on every read and the two new outage scenarios, both providers running
at the same time on this 2-CPU workspace, then one more Haivision run on its
own. Every run passed with these scores:

| Provider | Sender right | Receiver right | Wrong OK | Controls | Advice right, sender / receiver |
| --- | --- | --- | --- | --- | --- |
| Haivision SRT v1.5.5 | 12/12 | 12/12 | 0 | 3/3 OK | 8/8 / 7/7 |
| Robotweax SRT | 12/12 | 12/12 | 0 | 3/3 OK | 8/8 / 7/7 |

On 4 October 2026 the Haivision pin moved to v1.5.7 (`899348d8`, the Network
Lab's interop reference). Two standalone runs on Linux x86-64 in this cloud
workspace, one under ctest and one direct, both passed with the same scores:

| Provider | Sender right | Receiver right | Wrong OK | Controls | Advice right, sender / receiver |
| --- | --- | --- | --- | --- | --- |
| Haivision SRT v1.5.7 | 12/12 | 12/12 | 0 | 3/3 OK | 8/8 / 7/7 |

In one parallel run with an intermediate version of the code, the Haivision
receiver went Failing (recovery
headroom low) for 10 s under 2 % random loss, which is repaired loss that
should stay OK or Watch. It did not recur in three runs of that scenario on
its own or in the standalone full run. The two labs share two CPUs with
eight streaming threads, so starvation of the relay or reader is the likely
cause, not the engine. CI runs each provider on its own machine.

Typical verdicts (both providers alike unless noted):

| Fault class | Sender | Receiver | Detected after onset |
| --- | --- | --- | --- |
| loss, bursts, jitter | OK | OK | — |
| `heavy_loss_tight_latency` | Watch, latency low for RTT → latency 90–100 ms | At risk, late repairs → latency 130–150 ms | at once |
| `rtt_step` | Failing, latency low for RTT → latency 570 ms | same | 2.2–2.5 s |
| `bandwidth_cap` | Failing: latency low for RTT, then path congested → bitrate (Haivision later: path capacity exceeded) | Failing: path congested → bitrate, held for the whole 40 s | 2.2–2.3 s |
| `outage_6s` | Failing, acknowledgements stalled (no knob), held through the aftermath | OK for ~3 s, then Unknown (no data), then Failing, unrecovered loss (no knob) until the drops leave the count window | 2.4 s / 8.1–8.3 s |
| `receiver_stall` | Failing, peer read stall → receiving application | Failing, application read stall | 3.3–3.4 s / 2.1–2.2 s |
| `maxbw_too_low` | Failing, sender drops → MAXBW 1.27–1.29 MB/s (overshoots: both libraries count about 500 sender drops/s while 270 of the 500 packets/s still go out, so the offered rate reads high; a cap above the need is harmless, but the value is not yet calibrated) | Failing; advice latency or bitrate (it cannot see the sender's setting) | 2.8–3.0 s / 2.3 s |
| `connection_lost` | Failing, acknowledgements stalled → connection lost | Unknown (no data) → Failing, connection lost | 2.4 s / 5.2 s |

During a blackout, a receiver cannot tell a dead path from a paused source. It
keeps its verdict for 1 s (the gauge window) plus 2 s, then says Unknown. After
the path returns, it waits 2 s for evidence before OK, so the drops caused by
the gap become Failing without an OK in between.

## What the lab changed

**First run.** The receiver gate passed on both providers. The sender gate
failed: on Haivision 7/10 with a failing clean control, and one wrong OK on
Robotweax. Fixes from the measured traces:

- **Path headroom.**
  - What the trace showed: the capacity estimate follows the arrival rate. On
    a clean path it reads 4.1–5.7 Mbit/s for a 5.4 Mbit/s stream; with jitter
    it falls to 3 Mbit/s.
  - Fix: thresholds moved to 1.25 / 1.5 / 2.0. The ratio is graded only with
    congestion evidence: a filling send buffer, sender drops, or the
    congestion signature below.
- **Sender read stall.**
  - What the trace showed: when the receiving application stops, the flow
    window drains (1900 → 0 packets in 4 s), while flight ÷ flow window stays
    near 0.01.
  - Fix: the sender grades receive-buffer occupancy beyond what the latency
    explains.
- **Sender-drop advice.** It is now evidence-based. An outage gives
  `acknowledgements_stalled` with no knob, and MAXBW at the cap gives a MAXBW
  increase.
- **Congestion.**
  - What the trace showed: under the cap, RTT went from 21 to 286 ms with 44 %
    loss, and both ends advised more latency.
  - Fix: RTT above 2 × baseline + 10 ms with sustained loss now gives
    `path_congested` and a lower bitrate.

**Independent review.** It found two defects that the 22 s scenarios could not
show, plus weaknesses in the method. All are fixed, each with a unit test or a
scenario:

- **Congestion diagnosis faded after ~30 s.** The baseline RTT drifted toward
  the congested RTT. The baseline is now the lowest RTT of loss-free samples
  over 5 minutes; lossy samples never raise it. Tests: `bandwidth_cap` held
  for 40 s, and a 2-minute unit test.
- **The sender's flow-window reference ratcheted up after drops.** Libsrt
  grows the flow window by dropped packets, which left a permanent false
  read-stall bias. Samples with sender drops no longer raise the reference.
- **The outage could not be scored for a wrong OK.** It also showed a real
  gap: the receiver read OK for 2 s right after the path returned, before the
  drops counted. Fixes:
  - OK now needs 2 s of evidence after a gap or at stream start;
  - escalations that begin in that time finish first;
  - the outage lasts 6 s;
  - the wrong-OK window covers 2 s of aftermath.
- **CI flakiness.** A single OK read no longer fails the gate; it takes three
  consecutive reads.
- **Short controls.** Two controls were added (a healthy RTT rise and a
  26 Mbit/s stream), and the clean control runs for 60 s.
- **Congestion missed on a shallow queue.** RTT 21.8 → 59.5 ms with 60 % loss
  fell under the old 2 × base + 20 ms threshold. The absolute term is now
  10 ms, and path headroom counts congestion as evidence.

**Advice over time (2 October 2026).** The built-in UI showed a Haivision
receiver advising "raise the latency to 450 ms" about 10 s after a 6 s outage.
The lab had passed, because it judged advice only at the worst verdict, and
the outage's settle period ended before the advice changed. Scoring every
read, with the outage settle extended to 20 s, found three cases of the same
defect: drops stay in the 10 s count window after their cause is gone, and
judged again with the evidence of a healthy moment they got a wrong reason.
It showed in four places, all fixed:

| Case | Wrong advice, after the cause ended | Fix |
| --- | --- | --- |
| `outage_6s`, receiver (both providers) | latency, once the gap left the count window | arrival gaps are remembered beyond the window; drops in the 2 s after a gap longer than the latency are the outage's (no knob) |
| `bandwidth_cap`, receiver (both) | latency, after the cap was lifted | drops keep the diagnosis they had when they happened, while they are counted |
| `maxbw_too_low`, sender (both) | acknowledgements stalled, after MAXBW was reset and the backlog was dropped | a "no evidence" diagnosis of new drops keeps the last concrete one |
| `outage_6s`, sender (both) | lower the bitrate, for 1 s when the path returned | drops within 2 s of an acknowledgement stall are the outage's |

Each fix has a unit test that fails without it. An independent review of the
first fix then found that it hid real causes:
- a sender's outage aftermath renewed itself while drops continued, and
  overrode congestion, MAXBW and read-stall advice;
- old advice was reused for a new cause without drops;
- the fixed 2 s aftermath was too short at long latencies.

The rules in the [health engine](../architecture/health-engine.md#reasons-and-advice)
now:
- bound the aftermath by the latency and the sender's drop threshold;
- renew it only from a fresh stall diagnosis;
- let direct evidence win;
- give a sender's capacity advice only with loss reports.

Two scenarios guard them: `outage_long_latency` and `outage_then_cap`. A
second review round confirmed the fixes and found three smaller gaps, also
fixed:
- a buffer still draining after drops was taken for a stall;
- capacity advice was lost when a queue raises the RTT without loss reports;
- `frame_ancestors` accepted wildcards over a public suffix.

## Provider differences seen

- **Capacity estimate:**
  - Haivision SRT reports a capacity estimate (`mbpsBandwidth`) that follows
    the arrival rate.
  - Robotweax SRT's field is a measured arrival rate, so path headroom does
    not apply there.
- **Reordering:**
  - With 0–20 ms jitter, Haivision SRT retransmits heavily: 2.4–3.4 Mbit/s on
    a 5.4 Mbit/s stream, with no receiver loss. Robotweax SRT does not.
  - Neither side's viewer loses data, so both report OK; the overhead shows in
    the retransmission panels.
  - SRTO_LOSSMAXTTL is the knob. The engine does not advise it yet, because
    the sender has no reorder signal.
- **Stalled receiver:**
  - Haivision SRT breaks the connection once the receive buffer overflows
    ("SEQUENCE DISCREPANCY … BREAKING CONNECTION").
  - Robotweax SRT keeps the connection, and the sender drops instead.

## Limits

- **Loopback only.** Real-network jitter, cross traffic and estimator
  behaviour will differ. Wi-Fi or cellular RTT swings with random loss above
  5 % may read as congestion. Thresholds stay defaults until field streams
  calibrate them (Phase 4).
- **Not yet covered:** variable-bitrate and bursty senders. Encryption, FEC
  and bonding scenarios join the lab in Phase 4.
- **Timing-based.** The slowest detection (sender in `receiver_stall`,
  3.4 s) leaves about 0.6 s plus the three-read rule against the 4 s grace. A
  heavily loaded runner can still miss it; the CI report shows the timeline.
