# Product and architecture plan

This is the product plan the project has followed since 2026-10-01, copied
from the maintainer's planning document so that the repository holds
everything needed to continue. Where the plan and the code differ, the code
and the architecture docs win; the **Status** notes (2026-10-04) say what is
built. Phases and gates are in the [roadmap](../roadmap.md).

## Goal

SRT Telemetry becomes the reference way to observe an SRT stream from either
end: one embeddable C library that turns raw `srt_bistats` counters into
role-aware, honest health signals, plus drop-in sender and receiver
dashboards. Any product that links SRT should get Media-Gateway-grade
observability in an afternoon, without adopting a gateway.

"State of the art" is defined by targets that can be tested. The 1.0 targets
were proposed on 2026-10-01; they are agreed directions, not measurements.

| Dimension | Target for 1.0 | Status (2026-10-04) |
| --- | --- | --- |
| Field coverage | All 54 distinct observations of `SRT_TRACEBSTATS` (52 mapped, 2 interval rates derived instead), group statistics, plus socket state, negotiated options, KM state, reject reason | 52 of 54 mapped (PR #11), connection monitor done; group statistics not yet |
| Operator question | Every dashboard answers "is it fine, and if not, which knob" at a glance (verdict + reason + suggested change) | Done in the built-in UI, Grafana dashboards and alert rules (PRs #20, #21) |
| Time resolution | 100 ms sampling, burst-preserving min/max/p95 per 1 s, 10 s, 60 s window | Done |
| Honesty | No fake zeros: every value carries availability; unavailable shows as unavailable | Done in core, exports and UI |
| Integration cost | ≤ 30 lines of host code; zero-dependency C core; same binary with Haivision libsrt and Robotweax SRT | Embedding library; srt-live-transmit in 28 host lines (PR #22); GStreamer and gateway open |
| Overhead | < 1 % of one core at 1,000 sockets × 10 Hz | Not measured (Phase 4) |
| Outputs | In-process callback, JSONL, Prometheus, OTLP, live WebSocket feed, embeddable web UI, Grafana dashboards | Done, plus a JSON API and Prometheus alert rules |

## Benchmark

Haivision sets the bar for operator wording, but only inside its own
appliances; the open Sony exporter is embeddable but ships raw counters with
no meaning attached. Nobody offered both.

| Product | What the operator gets | Where it falls short |
| --- | --- | --- |
| Haivision Media / SRT Gateway | Per-route and per-listener-client statistics: state, buffer (time left to recover the first missing packet), negotiated latency, RTT, lost / skipped / undecrypted, retransmit rate, sender drops, path max bandwidth, encryption state, FEC counters; inline tuning tips; graphs; 24 h JSON download | Locked to the appliance; one route at a time; no fleet view or alerting; tips are static text, not computed from the stream |
| Haivision Kraken | Two graphs: delays (buffer, RTT, latency) and bandwidth (sent, received, resent, lost); CSV download | Very small field set |
| Makito X / X4 | Statistics page per device, read side by side for encoder and decoder | Manual comparison of two web pages |
| Sony / Haivision srt-prometheus-exporter (MPL-2.0) | Library on the SRT statistics API with an embedded HTTP server; per-socket metrics for Prometheus and Grafana | Raw counters only; rates only at scrape resolution, so sub-second bursts vanish; no role semantics, no verdict, no dashboards, no events |

Where SRT Telemetry wins:

- **Embeddable everywhere:** a library, not an appliance; one integration
  gives a product a built-in UI and a Prometheus/OTLP feed.
- **Computed advice, not static tips:** Haivision's guidance ("if Buffer
  often hits 0, lower the bitrate; if occasionally, raise latency") becomes a
  live rule with the stream's real numbers.
- **Burst-faithful:** 100 ms sampling in the process, exported as windowed
  min/max, so a 300 ms recovery-margin collapse is visible even at a 15 s
  scrape.
- **Honest semantics:** unique vs retransmitted vs FEC traffic; network loss
  vs sender drop vs receiver drop; "unavailable" instead of zero.
- **Events next to graphs:** connect, reject reason, key refresh, group
  member switch, latency renegotiation on the same timeline.

Sources the benchmark was based on (2026-09/10): the SRT statistics API
documentation, Haivision Media Gateway stream and route statistics pages,
Kraken statistics page, Haivision's "Optimizing SRT performance" guide, and
the srt-prometheus-exporter repository.

## Architecture

The library does all analysis once, in the process; every output is a thin
formatter over the same records, so a Grafana panel, the built-in UI and a
JSONL file cannot disagree. One versioned naming scheme (OpenTelemetry style,
for example `srt.receiver.recovery_headroom`) is shared by every output and
the UI ([metric names](../architecture/metric-names.md)).

Integration surfaces, in priority order:

1. **C ABI:** attach a socket, get records. Built as the
   [embedding library](../architecture/embedding.md)
   (`robotweax_srt_telemetry_embed_attach` and friends); the lower layers
   (sampler, adapter, HTTP server) stay available.
2. **Built-in UI** served by the embedded HTTP server
   ([web UI](../architecture/web-ui.md)). The plan's idea of reusable web
   components (`<srtt-sender>`, `<srtt-receiver>`, `<srtt-fleet>`) for the
   host's own web UI is **not built**: today the UI is a single page that can
   be framed (`frame_ancestors`).
3. **Grafana:** provisioned dashboards and alert rules on Prometheus data
   ([alerting](../architecture/alerting.md)); OTLP feeds other backends.
4. **Reference integrations:** srt-live-transmit (**done**), GStreamer
   `srtsrc`/`srtsink`, the Robotweax gateway; FFmpeg and OBS later.
5. **Bindings:** Go, Python and Rust over the C ABI (**not started**).

## Metric model

Every field the SRT statistics API offers is mapped, grouped by the question
it answers, and split by role ([field map](../architecture/srt-stats-field-map.md)).
Buffer fields must be read with `instantaneous=1`; otherwise `pktSndBuf`,
`msSndBuf`, `pktRcvBuf` and `msRcvBuf` are moving averages that smear bursts.

| Question | Sender fields | Receiver fields |
| --- | --- | --- |
| How much is flowing? | `byteSentUnique`, `byteRetrans`, `pktSndFilterExtra`, `byteSent`, `mbpsSendRate` | `byteRecvUnique`, `pktRcvRetrans`, `pktRcvFilterExtra`, `byteRecv`, `mbpsRecvRate` |
| What does the path allow? | `mbpsBandwidth`, `mbpsMaxBW`, `usPktSndPeriod`, `byteMSS` | `mbpsBandwidth`, `byteMSS` |
| What is being lost? | `pktSndLoss` (NAK-reported), `pktRecvNAK` | `pktRcvLoss`, `byteRcvLoss`, `pktSentNAK`, `pktReorderDistance`, `pktReorderTolerance` |
| What is recovered? | `pktRetrans`, `byteRetrans` | `pktRcvRetrans`, `pktRcvFilterSupply`, `pktRcvFilterLoss` |
| What never arrives? | `pktSndDrop`, `byteSndDrop` (too-late drop, not network loss) | `pktRcvDrop`, `byteRcvDrop`, `pktRcvBelated`, `pktRcvAvgBelatedTime`, `pktRcvUndecrypt`, `byteRcvUndecrypt` |
| How much margin is left? | `msSndBuf`, `pktSndBuf`, `byteSndBuf`, `byteAvailSndBuf`, `msSndTsbPdDelay`, `usSndDuration` | `msRcvBuf`, `pktRcvBuf`, `byteRcvBuf`, `byteAvailRcvBuf`, `msRcvTsbPdDelay` |
| Is the peer keeping up? | `pktFlightSize`, `pktFlowWindow`, `pktCongestionWindow`, `pktRecvACK` | `pktSentACK`, `byteAvailRcvBuf` |
| How is the path behaving? | `msRTT` | `msRTT` |
| Bonding | Group: `pktSentUnique`, `byteSentUnique` | Group: `pktRecvUnique`, `pktRcvDrop` |

Per connection, on change rather than per sample
([connection monitor](../architecture/connection-monitor.md)): socket state,
stream ID (opt-in), peer version, reject reason; negotiated latency,
OHEADBW, MAXBW, INPUTBW, LOSSMAXTTL, SNDDROPDELAY, TLPKTDROP, packet filter;
KM states and key length. **Not built yet:** peer address, connection mode,
reconnect count as a field, group member lists, host-supplied encoder target
bitrate and application read rate.

## Health model

Eight indicators, each from one endpoint's own data, drive a per-role
verdict (OK, Watch, At risk, Failing, Unknown) with a reason and a suggested
knob. One endpoint, descriptive, no causal claims about the far side. The
built engine is documented in [health engine](../architecture/health-engine.md);
the planned indicators were:

| Indicator | Computed as | Role | What it tells the operator |
| --- | --- | --- | --- |
| Recovery headroom | `msRcvBuf` ÷ `msRcvTsbPdDelay` (min over window) | Receiver | Time left to repair the first gap before it becomes a skip |
| Latency sufficiency | negotiated latency ÷ `msRTT` | Both | Whether latency allows enough retransmission rounds |
| Unrecovered ratio | `pktRcvDrop` ÷ (`pktRecvUnique` + `pktRcvDrop`) | Receiver | What the viewer actually loses |
| Recovery efficiency | 1 − drops ÷ loss detected (FEC supply credited) | Receiver | Whether ARQ/FEC is winning |
| Overhead budget used | retransmitted rate ÷ (unique rate × OHEADBW %) | Sender | How close retransmissions are to the overhead ceiling |
| Path headroom | (unique + retransmitted + FEC rate) ÷ `mbpsBandwidth` | Sender | Whether the stream fits the estimated capacity |
| Sender backlog | `msSndBuf` ÷ peer latency, plus `pktSndDrop` rate | Sender | Packets ageing toward a too-late drop |
| Peer read stall | `pktFlightSize` ÷ `pktFlowWindow`; falling `byteAvailRcvBuf` | Both | The receiving application, not the network, is the bottleneck |

Engine rules: time-based windows (never sample indices); relative,
configurable thresholds with shipped defaults marked as uncalibrated;
hysteresis (worse after 2 s, better after 10 s); Unknown is a verdict with the
missing field named; advice names a knob and a direction (headroom repeatedly
at zero with path headroom > 1 → lower bitrate; occasionally at zero with
spare path → raise latency to a computed value). All built.

## Dashboards as planned

The sender's question: "is my stream fitting the path, and is anything ageing
out before it leaves?" The receiver's: "is the viewer getting everything, and
how close am I to the first visible hit?" The planned rows, with what exists:

| Planned panel | Sender | Receiver | Built |
| --- | --- | --- | --- |
| Status strip: verdict, reason, knob; state, uptime, reconnects; peer, stream ID, peer version; latency; encryption/KM; FEC; bonding | ✓ | ✓ | Verdict, reason, knob, connection state; peer/uptime/FEC/bonding details not in the strip |
| Throughput vs capacity (unique, retransmitted, FEC; link bandwidth, MAXBW, encoder target) | ✓ | | Throughput chart; capacity lines not drawn |
| Overhead budget (retransmit % vs OHEADBW) | ✓ | | As an indicator, not a chart |
| Send backlog vs drop deadline; sender drops | ✓ | | Timing and margins charts |
| Loss reported by peer | ✓ | | Loss and recovery chart |
| Recovery headroom | | ✓ | Chart and indicator |
| Loss funnel (detected → retransmission → FEC → dropped) | | ✓ | Not built as a funnel |
| Lateness (belated packets, reorder distance) | | ✓ | Values in "Now", no chart |
| Integrity (undecryptable, KM changes) | | ✓ | Values and KM events |
| Path (RTT with min/p95 band; latency ÷ RTT) | ✓ | ✓ | Timing chart, indicator |
| Flow control / application drain | ✓ | ✓ | Margins chart, peer read stall indicator |
| Bonding (per-member state, share, loss) | ✓ | ✓ | **Not built** (no group support yet) |
| Event timeline | ✓ | ✓ | Events list and verdict timeline |

**Fleet view:** one row per socket, worst first, with verdict, reason,
advice, sparkline, rates; built in the UI and in Grafana. A paired view
(both ends of one stream side by side, display only) is **not built**.

**Alert rules as planned:** At risk for 10 s, Failing for 2 s, any
unrecovered packet, KM not secured on an encrypted stream, connection lost,
reconnect storm (3 in 5 min). **Built:** Failing (10 s), At risk (30 s),
Ungraded, statistics unreadable, target down; connection lost is covered by
Failing. Not built: any-unrecovered, KM not secured, reconnect storm.

**Retention as planned:** last 10 min at 100 ms plus configurable 1 s history,
downloadable as JSONL; a compact 1 s record (target ≤ 128 B). **Built:** the
compact record is 112 B per window; history is per window via
`/api/v1/sources/{id}/history`; there is no 10 min ring at 100 ms.

## Risks

| Risk | Effect | Mitigation (plan) | State |
| --- | --- | --- | --- |
| Defaults calibrated only in the lab | Verdicts wrong on real networks; operators lose trust | Ship as "Watch" only until 10+ field streams are reviewed; publish thresholds | Open: verdicts ship ungated; owner of thresholds undecided |
| `srt_bistats` lock contention at 10 Hz × many sockets | Jitter in the host's send path | Benchmark in Phase 4; fall back to 1 Hz with burst-triggered 100 ms mode | Open (Phase 4) |
| Field semantics differ across SRT versions | Inconsistent numbers between providers | Capability bits per exact provider revision | In the contract |
| Bonding and FEC labs are thin | Bonding/FEC panels untested | Add group and FEC scenarios to the fault lab before 1.0 | Open (Phase 4) |
| Scope creep back into diagnosis | Repeats the earlier detour | Any rule needing peer data goes to SRT Doctor's backlog | Rule in force |

Effort estimate from 2026-10-01: about 11–12 engineer-weeks for 1.0 in five
phases (roughly 7 calendar weeks with two engineers). Phases 0–2 and most of
Phase 3 were done by 2026-10-03.
