# Fault lab

The fault lab checks the [health engine](../architecture/health-engine.md) on
real SRT streams, with each provider, against the Phase 2 gate:

> the right verdict in at least 9 of 10 lab fault classes per role, and never
> a wrong OK.

Executable: `robotweax_srt_telemetry_public_srt_fault_lab` (ctest label `lab`,
built with the public SRT adapter). It runs in about four and a half minutes.

```sh
fault_lab                    # all scenarios, gate verdict, exit 0 on pass
fault_lab --list             # scenario names and descriptions
fault_lab --scenario rtt_step --timeline   # one scenario, verdict timeline
fault_lab --scenario bandwidth_cap --trace # plus per-second metrics
```

## Setup

Each scenario connects a fresh SRT caller (the sender) to a listener (the
receiver) through an impairing UDP relay inside the same process. The caller
sends 500 packets/s of 1316 bytes, about 5.3 Mbit/s. The relay adds 10 ms in
each direction (20 ms RTT) and applies the fault: loss, loss bursts, jitter, a
rate cap with a tail-drop queue, or a blackhole. Faults on the data path apply
to the forward direction; delay and blackholes apply to both.

Both sockets get a sampler source (100 ms period) with health enabled, and a
connection monitor that feeds the health context. The lab reads both verdicts
every 100 ms.

Timeline per scenario: 6 s clean warm-up, the fault (12 s unless noted), then a
settle period (4 s unless noted).

No kernel netem is needed, so the lab runs unprivileged in CI and on any
developer machine.

## Scenarios and expected verdicts

Expectations were written before the first run, from what each end can
observe. "Fine" means OK or Watch; "bad" means At risk or Failing.

| Scenario | Fault | Sender expects | Receiver expects | Right knob (sender / receiver) |
| --- | --- | --- | --- | --- |
| `clean` (control) | none | OK throughout | OK throughout | — |
| `random_loss_2pct` | 2 % random loss, 200 ms latency | fine | fine | — |
| `burst_loss` | 30-packet burst every 2 s | fine | fine | — |
| `jitter_reorder` | 0–20 ms jitter (reordering) | fine | fine | — |
| `heavy_loss_tight_latency` | 15 % loss, 60 ms latency on 20 ms RTT | Watch or worse | bad | latency / latency |
| `rtt_step` | RTT 20 → 140 ms, 200 ms latency | bad | bad | latency / latency |
| `bandwidth_cap` | 3 Mbit/s cap, 100 kB queue | bad | bad | bitrate / bitrate |
| `outage_3s` | blackhole for 3 s (settle 6 s) | bad | bad | none / none or latency |
| `receiver_stall` | receiving application stops reading (3 MB receive buffer) | bad | bad | receiving application / receiving application |
| `maxbw_too_low` | sender SRTO_MAXBW 2.8 Mbit/s | bad | bad | MAXBW / not judged |
| `connection_lost` | blackhole for good | Failing | Failing | — |

## Scoring

For each role:

- **Verdict:** the worst verdict from fault onset to the end of the settle
  period must be in the expected set. For the control, the verdict must be OK
  at every read after warm-up, with no Unknown.
- **Wrong OK:** from 4 s after onset (1 s gauge window + 2 s hysteresis + 1 s
  margin) until the fault ends, a role whose expected set excludes OK must
  never report OK.
- **Advice** (reported, not gated): the knob named most often at the worst
  verdict, before any lost connection, must be one of the right knobs.

**Gate:** per role, at least 90 % of the fault classes right (9 of 10), no
wrong OK, and the control OK.

## Results (1 October 2026, loopback, this cloud workspace)

Three consecutive full runs per provider with the final code, with both
providers running at the same time. All passed, with identical scores in
every run:

| Provider | Sender right | Receiver right | Wrong OK | Controls | Advice right, sender / receiver |
| --- | --- | --- | --- | --- | --- |
| Haivision SRT v1.5.5 | 10/10 | 10/10 | 0 | OK | 6/6 / 5/5 |
| Robotweax SRT | 10/10 | 10/10 | 0 | OK | 6/6 / 5/5 |

Typical verdicts, both providers alike:

| Scenario | Sender | Receiver | Detected after onset |
| --- | --- | --- | --- |
| loss, bursts, jitter | OK | OK | — |
| `heavy_loss_tight_latency` | Watch, latency low for RTT → latency 90–100 ms | At risk, late repairs → latency 130–150 ms | at once |
| `rtt_step` | Failing, latency low for RTT → latency 570 ms | same | 2.3–2.5 s |
| `bandwidth_cap` | Failing: latency low for RTT, then path congested → bitrate 2.5 Mbit/s | Failing: late repairs, then path congested → bitrate 2.7 Mbit/s | 2.2–2.4 s |
| `outage_3s` | Failing, acknowledgements stalled (no knob) | Failing after the gap, unrecovered loss | 2.5 s / 5.2 s |
| `receiver_stall` | Failing, peer read stall → receiving application | Failing, application read stall | 3.2–3.4 s / 2.2–2.4 s |
| `maxbw_too_low` | Failing, sender drops → MAXBW 680–690 kB/s | Failing (headroom or recovery), advice latency (it cannot see the sender's setting) | 2.8–3.0 s / 2.2–2.3 s |
| `connection_lost` | Failing, acknowledgements stalled → connection lost | Unknown (no data) → Failing, connection lost | 2.5 s / 5.2 s |

## What the lab changed

The first run passed the receiver gate on both providers but failed the sender
gate on Haivision (7/10, clean control Failing) and produced one sender wrong
OK on Robotweax. Each fix below came from a measured trace:

- **Path headroom.** The capacity estimate follows the arrival rate. On a clean
  path it reads 4.1–5.7 Mbit/s for a 5.4 Mbit/s stream; with jitter it falls to
  3 Mbit/s. A ratio near 1 is therefore normal, and on its own the ratio is
  not evidence. Thresholds moved from 0.75 / 0.9 / 1.05 to 1.25 / 1.5 / 2.0,
  and the indicator is graded only with congestion evidence (a filling send
  buffer or sender drops).
- **Sender read stall.** When the receiving application stops, the advertised
  flow window drains (1900 → 0 packets in 4 s), while flight ÷ flow window
  stays near 0.01 until the buffer is full. The sender now grades the share of
  the receive buffer held beyond what the latency explains. This turned the
  Robotweax wrong OK into detection after 3.3 s.
- **Sender-drop advice.** "Lower the bitrate" was the default; it is now
  evidence-based. An outage with no loss reports gives `acknowledgements_stalled`
  with no knob, and MAXBW at the cap gives a MAXBW increase.
- **Congestion.** Under the bandwidth cap both ends first advised more latency
  (1.15–1.72 s). RTT well above its baseline with sustained loss now means
  `path_congested` with a bitrate decrease.
- **Bursts versus sustained loss.** After an outage the receiver advised a
  lower bitrate. That advice now needs loss in most samples of the window.

## Provider differences seen

- **Capacity estimate:** Haivision SRT reports one (`mbpsBandwidth`) that
  follows the arrival rate. Robotweax SRT's field is a measured arrival rate,
  so path headroom does not apply there.
- **Reordering:** with 0–20 ms jitter, Haivision SRT retransmits heavily
  (2.4–3.4 Mbit/s on a 5.4 Mbit/s stream, with no receiver loss). Robotweax SRT
  does not. Neither side's viewer loses data, so both are OK; the Haivision
  overhead shows in the retransmission panels. SRTO_LOSSMAXTTL is the knob.
  The engine does not advise it yet, because the sender has no reorder signal.
- **Stalled receiver:** Haivision SRT breaks the connection once the receive
  buffer overflows ("SEQUENCE DISCREPANCY … BREAKING CONNECTION"). Robotweax
  SRT keeps the connection, and the sender drops instead.

## Limits

- Loopback only: real-network jitter, cross-traffic and estimator behaviour
  will differ. Thresholds stay defaults until field streams calibrate them
  (Phase 4).
- Encryption, FEC and bonding scenarios join the lab in Phase 4.
- Timing-based: on a heavily loaded machine, detection times grow. The 4 s
  detection grace has about 0.6 s of margin in the slowest case
  (`receiver_stall` on the sender, 3.4 s).
