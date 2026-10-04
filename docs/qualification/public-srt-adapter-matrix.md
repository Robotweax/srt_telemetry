# Public SRT adapter qualification matrix

Updated: 2026-10-04

This matrix separates build-time compatibility from live behavioral
qualification. A pass in the first three columns does not authorize production
capabilities by itself.

| Provider candidate | Exact source | Public header/signature | Deterministic adapter behavior | Bidirectional self-loopback | Cross-implementation interop | Status |
| --- | --- | --- | --- | --- | --- | --- |
| Robotweax SRT `v0.2.6` | `50cba37bc89f423ee7c0a292c3c3eae71b5fc7f9`; release tag | Pass | Pass | Pass on macOS and Linux ARM64 | Pass against Haivision 1.5.5 in both caller/listener roles | Release-specific candidate; pressure limits retained |
| Robotweax SRT `v0.2.0-11-ge3d61f7` | `e3d61f77e852001224b6586c63bcff46a3200223`; clean local clone | Pass | Pass | Pass | Pass, nine-field Live/Message IPv4 subset | Fabric-pinned interop candidate |
| Robotweax SRT `v0.2.0-4-g271f832` | `271f832ef4bb96fab13777a313a0af9d7e20af7e`; clean local clone | Pass | Pass | Pass | Pass, nine-field Live/Message IPv4 subset | Interop candidate |
| Haivision SRT v1.5.7 | `899348d8318eb9a3c5a5b6ec43c4a1114288773a`; tag `v1.5.7`; generated installed headers | Pass | Pass | Pass (Linux x86-64, 2026-10-04) | Pass against Robotweax SRT main in CI | Pinned CI provider since 2026-10-04 |
| Haivision SRT v1.5.5 | `b6b4ae990daa8193625a4ddeaeaed03023b23125`; tag `v1.5.5`; generated installed headers | Pass | Pass | Pass | Pass, nine-field Live/Message IPv4 subset | Interop candidate |

The [0.2.6 record](../../qualification/results/2026-09-27-srt-026/README.md)
adds exact-release macOS interop and lifecycle results plus 16 Linux ARM64
namespace/netem runs. All application transfers completed, but receiver-pause
causal evidence and the pure queue-growth bandwidth gate did not pass in every
run. Its Docker UDP buffer maxima differ from the earlier dedicated Lab VM.
These findings do not promote a broad provider capability or change the
existing diagnostic thresholds.

Both builds use the same adapter source and a compile-time assertion that the
provider's `srt_bistats` address has exactly the injected callback type. The
tests also prove `clear=0`, `instantaneous=1`, candidate field mapping, unit
conversion, unavailable negative values, lifetime-counter deltas, and explicit
native failures.

The provider-linked loopback test opens real caller/listener sockets, exchanges
1,316-byte messages in both directions, samples caller and accepted endpoints,
and requires availability plus positive deltas for physical and unique packet
and wire-byte counters. RTT must also be available on both endpoints.

The two-process qualification executes each implementation once as caller and
once as listener. Both directions carry a deterministic 1,316-byte payload only
after out-of-band baseline barriers. The retained result records source
revisions, build profiles, participant observations, and SHA-256 digests of the
provider libraries, peer executables, and payloads:

- [2026-08-28 macOS arm64 public SRT interop](../../qualification/results/2026-08-28-macos-arm64-public-srt-interop.json)
- [2026-08-30 Fabric-pinned Robotweax revision interop](../../qualification/results/2026-08-30-macos-arm64-public-srt-interop-e3d61f7.json)

## Sampling and lifecycle candidate evidence

The retained macOS arm64 run executes the providers sequentially and records
the exact binaries. Its isolated synthetic callback measured median incremental
SDK work of 40.5 ns in the Robotweax-linked build and 54.5 ns in the
Haivision-linked build. The provider-call median was 26.6 us for Robotweax SRT
and 0.211 us for Haivision SRT. The paired Robotweax provider-call difference
was negative because provider variance exceeded the much smaller SDK cost; the
synthetic measurement is the meaningful adapter-cost isolation.

Both providers passed the supported order in which the telemetry session is
destroyed before its socket is closed. The stale-handle probe returned the
explicit provider-sample failure with native `SRT_ERROR` and no available
fields. A separate time-bounded close/sample race also terminated without a
crash or hang and reported the same failure, but it remains deliberately
unsupported host usage.

Scheduler isolation and power management control were unavailable on this
host, so these timings are candidate observations rather than release budgets.
The roughly two-orders-of-magnitude provider-call difference is an
investigation signal for Robotweax SRT, not yet a regression conclusion.

- [2026-08-28 macOS arm64 sampling and lifecycle](../../qualification/results/2026-08-28-macos-arm64-public-srt-runtime.json)
- [2026-08-30 Fabric-pinned Robotweax revision sampling and lifecycle](../../qualification/results/2026-08-30-macos-arm64-public-srt-runtime-e3d61f7.json)

The second run qualifies the exact Robotweax SRT revision pinned by Intelligent Media Fabric. All
42 telemetry tests passed against that build, including the public adapter, live self-loopback,
supported lifecycle, and provider-linked JSONL recorder path. The cross-provider runner also passed
with each implementation as caller and listener. Robotweax provider-call p50 fell from the earlier
26.6 us observation to 100.4 ns after the empty receive-statistics fast path; because neither host
run controlled scheduling or power management, this remains a strong investigation result rather
than a release performance claim.

The newer profile does not inherit the loss, burst, pressure, recovery-margin, or operator-study
claims retained for revision `271f832ef4bb96fab13777a313a0af9d7e20af7e`. Linux, IPv6,
encryption, FEC, groups, `fips0`, reset behavior, and representative deployment performance remain
separate qualification work.

## Constant-loss field candidate evidence

An exact Robotweax sender and Haivision receiver were connected through a
user-space UDP proxy. For each of four active intervals, the proxy discarded 10
of 200 first-observed DATA sequences and forwarded 10 later observations of the
same sequences. All application messages arrived.

Robotweax reported 9 sender-loss packets, 10 retransmitted packets, 210 sent
physical packets, and 200 sent unique packets per active interval. Haivision
reported 9 receiver-loss packets while receiving 200 physical and 200 unique
packets. Loss and retransmission returned to zero after fault removal.

The reverse capture uses an exact Haivision sender and Robotweax receiver. It
sends 201 original messages so one non-dropped DATA sequence follows the tenth
fault. Each active interval records 10 proxy drops, 10 repeated sequences, 10
Haivision sender-loss and retransmission packets, 211 sent physical packets,
201 sent unique packets, and 10 Robotweax receiver-loss packets. All 201
application messages arrive and all three deltas return to zero after recovery.

Together these are direction- and profile-specific candidate evidence for the
public loss, retransmission, and physical-versus-unique mappings. The injected
drop count is not treated as identical to either public loss counter. The
reverse schedule qualifies exposed gap loss, not a lost finite-flight tail
without later DATA. No release capability is declared until representative
profiles and that remaining boundary pass.

- [2026-08-28 captured Robotweax-to-Haivision constant loss](../../testdata/golden-incidents/incident-002-captured-constant-loss-robotweax-haivision/manifest.json)
- [2026-08-28 captured Haivision-to-Robotweax constant loss](../../testdata/golden-incidents/incident-003-captured-constant-loss-haivision-robotweax/manifest.json)

The direct diagnostic captures repeat this constant-loss profile with a
nine-record SDK ring inside each exact provider-linked peer. Both endpoint
snapshots are published through the versioned JSONL exporter and consumed by
the local analyzer. In both provider directions it identifies samples 3–6 as
the deviation window, sample 7 as recovery, and selects recoverable
transmission-path loss. No recorder overwrite occurs.

- [2026-08-29 Robotweax-to-Haivision direct JSONL analysis](../../testdata/golden-incidents/incident-006-jsonl-constant-loss-robotweax-haivision/manifest.json)
- [2026-08-29 Haivision-to-Robotweax direct JSONL analysis](../../testdata/golden-incidents/incident-007-jsonl-constant-loss-haivision-robotweax/manifest.json)

## Burst-loss field candidate evidence

The same exact peers pass a contiguous burst profile in both sender directions.
During each active interval the proxy discards original DATA positions 81
through 100 and forwards the 20 retransmissions. Each sender reports 20 loss
packets, 20 retransmitted packets, 220 sent physical packets, and 200 sent
unique packets. Each receiver reports 20 loss packets while receiving all 200
unique application packets. Loss and retransmission return to zero after fault
removal.

This qualifies recovery and public counter magnitude for an exposed 20-packet
gap on the declared profiles. One hundred later originals expose every missing
sequence, so finite flight-tail behavior remains outside the boundary. The
one-second public counters also do not establish loss adjacency by themselves;
the retained proxy pattern is the burst-shape ground truth. No release
capability is declared from these candidates.

- [2026-08-29 captured Robotweax-to-Haivision burst loss](../../testdata/golden-incidents/incident-004-captured-burst-loss-robotweax-haivision/manifest.json)
- [2026-08-29 captured Haivision-to-Robotweax burst loss](../../testdata/golden-incidents/incident-005-captured-burst-loss-haivision-robotweax/manifest.json)

## Incident-mode loss-shape candidate evidence

A continuous 240-message transfer was sampled at a wall-clock-paced 20 Hz in
both cross-provider sender directions. The distributed profile drops one packet
in each of eight 50 ms windows; the concentrated profile drops eight packets in
one window. Every scenario reports the same aggregate eight losses, eight
sender retransmissions, 248 physical sends, 240 unique sends, and 240 received
messages.

Both sender and receiver timelines expose eight one-packet windows versus one
eight-packet window. All four schedules miss zero complete sample windows; the
largest measured interval is 55.35 ms. This is candidate evidence for
incident-mode coarse loss-shape discrimination through the common public API,
so no Robotweax-only extension is selected for that operator question. Exact
adjacency inside a window and a controlled release cost gate remain open.

- [2026-08-29 macOS arm64 20 Hz loss-shape observability](../../qualification/results/2026-08-29-macos-arm64-loss-shape-observability.json)

## Receiver-pressure field candidate evidence

The exact peers were directly connected over IPv4 loopback while receiver
application reads were withheld for approximately 417–418 ms. Each direction
sent 7,000 messages during the pause plus 400 warm and recovery messages. Both
receivers reached 9,212,000 bytes of public receive-buffer occupancy during
samples 3–6. Sender flight or send-buffer growth overlapped that window, and
the receiver buffer recovered at sample 7.

All 7,400 messages arrived in both captures without public loss, endpoint drop,
undecryptable, or retransmission evidence. The paired public gauges therefore
support a receiver-endpoint pressure hypothesis in both provider directions.
They do not reveal exact application-consumption timing, and loopback does not
qualify production schedulers or physical network profiles. No Robotweax-only
signal is selected for this operator question.

- [2026-08-29 Robotweax sender to paused Haivision receiver](../../testdata/golden-incidents/incident-010-receiver-pause-robotweax-haivision/manifest.json)
- [2026-08-29 Haivision sender to paused Robotweax receiver](../../testdata/golden-incidents/incident-011-receiver-pause-haivision-robotweax/manifest.json)

## Sender-/egress-pressure field candidate evidence

A monotonic user-space UDP proxy restricted downstream DATA release to 1,400
packets per second while each sender offered 2,000 messages without application
pacing. The proxy queue exceeded 1,800 packets in both provider directions.
Public sender-buffer occupancy exceeded 2.4 MB and flight size exceeded 1,790
packets during samples 3–6, while each receiver buffer remained below the
conservative 1 MiB pressure threshold. Both gauges recovered at sample 7.

All 2,400 messages arrived in both captures without public loss, endpoint drop,
undecryptable, or retransmission evidence. Common public gauges therefore
support a sender-endpoint pressure hypothesis in both directions. Exact offered
application rate and egress capacity remain unavailable from public statistics;
the retained proxy stages are ground truth for the controlled restriction. No
Robotweax-only signal is selected for the bounded localization question.

- [2026-08-29 Robotweax sender through reduced egress capacity](../../testdata/golden-incidents/incident-012-bandwidth-reduction-robotweax-haivision/manifest.json)
- [2026-08-29 Haivision sender through reduced egress capacity](../../testdata/golden-incidents/incident-013-bandwidth-reduction-haivision-robotweax/manifest.json)

## Receiver recovery-margin field candidate evidence

A monotonic user-space UDP proxy retained the first 100 original DATA sequences
and every observed repeat until each receiver had completed 1,000 of 1,100
offered messages. Both directions expose paired sender/receiver loss, sender
retransmission, exactly 100 receiver drops, and a positive configured receiver
TSBPD delay. Across warm, active, and recovery transfers, 1,500 unique packets
were sent and 1,400 received; receiver drops exactly account for the gap.

Common public fields therefore support a bounded receiver recovery/playout
margin diagnosis. They do not expose belated-arrival counters or prove that a
larger latency setting would prevent the incident. No Robotweax-only signal is
selected for that bounded question.

- [2026-08-29 Robotweax sender to Haivision receiver recovery-margin exhaustion](../../testdata/golden-incidents/incident-014-recovery-margin-robotweax-haivision/manifest.json)
- [2026-08-29 Haivision sender to Robotweax receiver recovery-margin exhaustion](../../testdata/golden-incidents/incident-015-recovery-margin-haivision-robotweax/manifest.json)

## Remaining release qualification

- live caller-to-listener and listener-to-caller sampling for the remaining
  candidate fields and profiles;
- additional modes, address families, encryption, FEC, and group profiles;
- observed reset and reconnect semantics for every enabled field;
- release sampling budgets on representative controlled platforms and build
  profiles;
- longer-duration lifecycle and reconnect stress tests; and
- retained evidence manifests with exact binaries, build flags, payloads, and
  expected observations.

Until full field and deployment-profile checks pass, the candidate records
intentionally declare no release-qualified runtime capabilities.
