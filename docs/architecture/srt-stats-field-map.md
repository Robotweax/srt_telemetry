# SRT statistics field map

This map accounts for every field of the public `SRT_TRACEBSTATS`
(`CBytePerfMon`) surface in Haivision SRT v1.5.5 and Robotweax SRT. Both
providers expose the same 82-field layout. Those 82 fields carry
**54 distinct observations**: 28 lifetime counters (each also exposed as an
interval twin), 20 instantaneous gauges, one clock, and 5 values that exist only
as interval fields.

| Status | Observations |
| --- | --- |
| Mapped in `raw_sample_v1` | 27 |
| Mapped in `ext_sample_v1` | 25 |
| Deliberately excluded (`mbpsSendRate`, `mbpsRecvRate`) | 2 |
| **Total** | **54** |

The 28 interval twins (`pktSent`, `byteRecv`, ...) are not mapped. With
`clear = 0` they duplicate their lifetime counter since the last clear, and the
delta engine derives exact interval values from the lifetime counters instead.

`mbpsBandwidth` has a different meaning in each provider, so it maps to one of
two contract fields (see below); the 25 extended fields count it once.

The live gate test `robotweax_srt_telemetry_public_srt_field_coverage_test`
qualifies all 52 mapped observations on a bidirectional loopback and fails if
any is **unavailable** on either endpoint. It also checks a few values that
must be non-zero on a healthy loopback (connection age, MSS, ACK and
sender-active deltas). It passes against Haivision SRT v1.5.5 and Robotweax SRT
`main`. It proves availability, not value correctness: without FEC,
encryption or loss, the filter, undecryptable and belated fields are observed
zeros. Those values are qualified by the fault lab (see "Still open").

## Collection rules

- Collection always uses `clear = 0` and `instantaneous = 1`. The adapter never
  resets the host's interval counters, and buffer gauges are current values
  rather than one-second moving averages.
- A clear availability bit means unavailable, never an observed zero. A
  negative or non-finite provider value is unavailable.
- Units follow OpenTelemetry: seconds, bytes, packets, bit/s. Durations that
  must stay integral for deltas use nanoseconds.
- Role says which dashboard the observation is meant for. Every SRT endpoint
  can both send and receive, so both sides report every field.

## Lifetime counters

| Contract field | Source (lifetime) | Role | Notes |
| --- | --- | --- | --- |
| `sent_packet_total` | `pktSentTotal` | sender | includes retransmissions and FEC parity |
| `received_packet_total` | `pktRecvTotal` | receiver | may include duplicates and FEC control |
| `unique_sent_packet_total` | `pktSentUniqueTotal` | sender | first transmissions only |
| `unique_received_packet_total` | `pktRecvUniqueTotal` | receiver | newly accepted DATA, including FEC-rebuilt |
| `sender_loss_packet_total` | `pktSndLossTotal` | sender | loss learned when a NAK newly queues a retransmission |
| `receiver_loss_packet_total` | `pktRcvLossTotal` | receiver | newly observed sequence gaps |
| `retransmitted_packet_total` | `pktRetransTotal` | sender | retransmissions actually sent |
| `sender_drop_packet_total` | `pktSndDropTotal` | sender | local TTL/TLPKTDROP removals; see below |
| `receiver_drop_packet_total` | `pktRcvDropTotal` | receiver | not delivered to the application; includes undecryptable |
| `receiver_undecryptable_packet_total` | `pktRcvUndecryptTotal` | receiver | rejected at the decrypt/auth boundary |
| `sent_wire_byte_total` | `byteSentTotal` | sender | DATA plus IP/UDP/SRT headers |
| `received_wire_byte_total` | `byteRecvTotal` | receiver | DATA plus IP/UDP/SRT headers |
| `unique_sent_wire_byte_total` | `byteSentUniqueTotal` | sender | |
| `unique_received_wire_byte_total` | `byteRecvUniqueTotal` | receiver | |
| `retransmitted_wire_byte_total` | `byteRetransTotal` | sender | |
| ext `sent_ack_packet_total` | `pktSentACKTotal` | receiver | ACKs sent |
| ext `received_ack_packet_total` | `pktRecvACKTotal` | sender | ACKs received |
| ext `sent_nak_packet_total` | `pktSentNAKTotal` | receiver | one compressed multi-range NAK counts once |
| ext `received_nak_packet_total` | `pktRecvNAKTotal` | sender | |
| ext `sender_active_nano_total` | `usSndDurationTotal` | sender | time with queued or unacknowledged DATA; µs to ns |
| ext `receiver_loss_wire_byte_total` | `byteRcvLossTotal` | receiver | estimated from average packet size |
| ext `sender_drop_wire_byte_total` | `byteSndDropTotal` | sender | |
| ext `receiver_drop_wire_byte_total` | `byteRcvDropTotal` | receiver | estimated from average packet size |
| ext `receiver_undecryptable_payload_byte_total` | `byteRcvUndecryptTotal` | receiver | payload only, no headers, in both providers |
| ext `sent_filter_extra_packet_total` | `pktSndFilterExtraTotal` | sender | FEC parity sent; needs `CAP_FEC_STATS_V1` |
| ext `received_filter_extra_packet_total` | `pktRcvFilterExtraTotal` | receiver | FEC control consumed; needs `CAP_FEC_STATS_V1` |
| ext `filter_recovered_packet_total` | `pktRcvFilterSupplyTotal` | receiver | rebuilt by FEC; needs `CAP_FEC_STATS_V1` |
| ext `filter_unrecovered_packet_total` | `pktRcvFilterLossTotal` | receiver | declared irrecoverable by FEC; needs `CAP_FEC_STATS_V1` |

Filter counters are zero when no packet filter is negotiated. A zero there is an
observed zero, not "unavailable".

## Interval-only values

These have no lifetime field. The two counters accumulate since the
connection started, unless the host itself calls `srt_bstats`/`srt_bistats`
with `clear = 1`. The delta engine detects a host clear only when the counter is
lower than at the previous sample, and then reports a field-local reset
(`ext_delta.counter_reset_fields`) instead of a negative delta. If the counter
has already climbed back above the previous sample, the clear goes unnoticed
and that interval's delta is too small. Ext resets never set the shared v1
`QUALITY_COUNTER_RESET` flag, because the lifetime counters are unaffected.
Hosts that clear interval statistics should say so in their qualification
record.

| Contract field | Source | Role | Notes |
| --- | --- | --- | --- |
| ext `received_retransmitted_packet_since_clear` | `pktRcvRetrans` | receiver | counter; retransmissions that arrived |
| ext `belated_packet_since_clear` | `pktRcvBelated` | receiver | counter; arrived too late to be used (already acknowledged, dropped or outside the buffer), including late duplicates |
| ext `reorder_distance_packets` | `pktReorderDistance` | receiver | gauge; recent reorder distance, decays with the tolerance estimator and is **not** reset by a clear in either provider |
| excluded | `mbpsSendRate` | sender | rate over the host's clear interval; derived from lifetime bytes instead |
| excluded | `mbpsRecvRate` | receiver | same |

## Gauges

| Contract field | Source | Role | Notes |
| --- | --- | --- | --- |
| `rtt_seconds` | `msRTT` | both | provider-reported smoothed estimate; ms to s; validity, source and age unavailable through `msRTT` |
| `flight_size_packets` | `pktFlightSize` | sender | |
| `flow_window_packets` | `pktFlowWindow` | sender | receiver free space as reported in ACKs |
| `congestion_window_packets` | `pktCongestionWindow` | sender | not used in live mode |
| `available_send_buffer_bytes` | `byteAvailSndBuf` | sender | |
| `available_receive_buffer_bytes` | `byteAvailRcvBuf` | receiver | falls when the application stops reading |
| `send_buffer_bytes` | `byteSndBuf` | sender | DATA wire bytes |
| `send_buffer_seconds` | `msSndBuf` | sender | span of unacknowledged DATA |
| `receive_buffer_bytes` | `byteRcvBuf` | receiver | retained application payload |
| `receive_buffer_seconds` | `msRcvBuf` | receiver | span of acknowledged DATA; not latency margin by itself |
| `sender_tsbpd_delay_seconds` | `msSndTsbPdDelay` | sender | peer latency |
| `receiver_tsbpd_delay_seconds` | `msRcvTsbPdDelay` | receiver | own latency |
| ext `packet_send_period_seconds` | `usPktSndPeriod` | sender | minimum pacing interval; µs to s |
| ext `send_bandwidth_limit_bits_per_second` | `mbpsMaxBW` | sender | `SRTO_MAXBW`, or the congestion controller's value when unset (Haivision); 0 = none reported |
| ext `mss_bytes` | `byteMSS` | both | negotiated MSS |
| ext `send_buffer_packets` | `pktSndBuf` | sender | |
| ext `receive_buffer_packets` | `pktRcvBuf` | receiver | acknowledged packets only |
| ext `reorder_tolerance_packets` | `pktReorderTolerance` | receiver | current estimator, capped by `SRTO_LOSSMAXTTL` |
| ext `average_belated_seconds` | `pktRcvAvgBelatedTime` | receiver | smoothed lateness of belated packets; ms to s |
| ext `estimated_link_capacity_bits_per_second` | `mbpsBandwidth` | both | **Haivision only** (see below) |
| ext `data_arrival_rate_bits_per_second` | `mbpsBandwidth` | receiver | **Robotweax only** (see below) |

## Clock

| Contract field | Source | Notes |
| --- | --- | --- |
| ext `connection_age_seconds` | `msTimeStamp` | time since connect; transport-local, never wall clock |

## `mbpsBandwidth` differs by provider

- **Haivision SRT** reports a link-capacity estimate from probe packet pairs:
  on the receiving side its own estimate, on the sending side the value the
  peer carried back in ACKs. It maps to
  `estimated_link_capacity_bits_per_second`.
- **Robotweax SRT** reports measured local DATA arrival throughput, or zero
  without an arrival sample; it does not estimate link capacity
  ([Robotweax statistics](https://github.com/Robotweax/srt/blob/main/docs/statistics.md)).
  It maps to `data_arrival_rate_bits_per_second`.

The adapter only accepts the interpretation that belongs to the configured
provider, as encoded in
`ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_HAIVISION_V1_5_5_EXT_CANDIDATE_FIELDS`
and `ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_ROBOTWEAX_EXT_CANDIDATE_FIELDS`. A path-headroom indicator therefore needs link capacity from a Haivision
peer or another source; it cannot use Robotweax's arrival rate as capacity.

## Group statistics

`srt_bistats` on a group ID fills only these fields; the rest stay zero. Use a
separate session whose socket is the group ID and qualify
`ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_CANDIDATE_FIELDS` and
`ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_EXT_CANDIDATE_FIELDS`.

| Contract field | Group meaning |
| --- | --- |
| `unique_sent_packet_total` / `unique_sent_wire_byte_total` | each original packet once, regardless of how many links carried it |
| `unique_received_packet_total` / `unique_received_wire_byte_total` | first arrival over any link |
| `receiver_drop_packet_total` / ext `receiver_drop_wire_byte_total` | dropped on every link: what the application actually lost |
| ext `connection_age_seconds` | time since the first member connected |

Group mapping is unit-tested with an injected `srt_bistats`. The group
session ([connection groups](public-srt-group.md)) reads the group and all its
members in one call. Its live loopback test shows the group unique counters
equal to what the application sent and read on both providers. Impaired-path
qualification belongs to the Network Lab's multi-path work and is still open.

## Sender-drop interpretation

`sender_drop_packet_total` and its interval delta preserve the combined public
counter. They neither separate TTL from TLPKTDROP nor report a count of lost
application messages. An original datagram may already be queued in the kernel
when SRT abandons its retained copy and future retransmission responsibility;
that original can still arrive successfully.

Robotweax's [public definition at merged revision 09316eda](https://github.com/Robotweax/srt/blob/09316eda48e3360808fff2021eddc2f2bf821b06/docs/sender-drop-semantics.md)
documents the packet/ACK experiment. For that Live profile, the TLPKTDROP
deadline is `max(peer latency + sender drop delay, 1000 ms) + 20 ms`:
120 ms peer latency and zero sender drop delay give 1020 ms. This is a
provider/profile-specific rule, not a new common SDK metric or a universal
conversion from the TSBPD gauge. The public combined counter does not expose
which removal cause or deadline applied to a particular packet.

The instrumented Robotweax experiment establishes the mechanism for its pinned
build; it does not establish identical timing or counts for Haivision versions.

## Deliberately excluded shortcuts

- `mbpsSendRate` and `mbpsRecvRate` are not the canonical rate source. Their
  interval depends on the statistics reset boundary and physical accounting.
- `packets_lost` is not a canonical metric because it erases observation side.
- `packets_dropped_late` is not a canonical metric because it conflates drop
  causes and belated arrival.
- `window_available` is not a canonical metric because the source surfaces use
  different packet and byte quantities.
- `receive_buffer_ms` is not labeled latency margin. Margin is a derived,
  configuration-aware quantity computed by the health engine.

## Still open

- Live values (not just availability) for FEC counters with
  `SRTO_PACKETFILTER` negotiated, undecryptable counters with a wrong
  passphrase, belated and reorder fields under reordering, and group
  statistics over impaired paths (Network Lab multi-path setups).
- Recording extended fields in the bounded recorder and JSONL (Phase 2 compact
  record). Until then the extended sample is available through the adapter API.

## Qualification record required

Each adapter/provider version must record:

- exact provider version and build flags;
- role and data direction;
- Live/Message or File/Stream profile;
- address family, encryption, FEC, and group profile;
- field availability and observed reset behavior;
- counter relationships under loss, duplicate, reorder, and retransmission;
- sample-call overhead and close/sample race behavior; and
- any divergence from this map.

## Optional group path metadata

| Telemetry field | Public source | Side / lifecycle | Availability and meaning |
| --- | --- | --- | --- |
| group member `path`, `path_source = PATH_SOURCE_IDENTIFIER` | `robotweax_srt_group_path_data_v1`: `member_id`, `flags`, `identifier_length`, `identifier` | both sides; once negotiated, cached per member socket until removal/session destruction | Exact 1–32 bytes rendered as `pathid-` plus hex; `CAP_GROUP_PATH_IDENTIFIERS_V1` plus explicit getter injection required. Not a counter, timestamp or authenticated identity. |
| group member `MEMBER_PATH_PENDING` | unavailable, malformed or provisional getter record | connected unnamed member; retried each tick | Unknown metadata stays unknown; counters still sample. A confirmed zero-length/zero-flags record uses the existing fallback. |

Haivision v1.5.7 and published Robotweax v0.2.8 have no such getter; their host,
token and address naming remains available. Qualification is revision/build
specific. See [group sessions](public-srt-group.md#path-names) for snapshot races,
binary encoding, scope, resets and retained BROKEN-member behavior.

## Optional RTT observation history (V1)

Operator question: is the shown estimate initial, locally observed, or peer
advertised, and when was each observation accepted? The optional
`robotweax_srt_rtt_data_v1` extension answers this separately from `msRTT`.
Hosts explicitly inject a getter qualified against the selected build. The
adapter validates its 48-byte V1 contract; it does not resolve symbols, select a
provider by name, or guess an extended statistics layout. The helper is compiled
only where the public header declares V1. Old Robotweax and Haivision builds use
NULL and report unavailable. A failed/malformed read clears current metadata.

| Source | Unit and meaning | Reset / availability |
| --- | --- | --- |
| smoothed / variation | microseconds in getter, seconds in JSON; current instantaneous estimator values | readable connected or retained BROKEN member; separate from windowed RTT |
| observation flag | accepted estimator observation has existed; not accuracy or freshness | initially false; numeric 100 ms does not decide it |
| local ACKACK age | local monotonic elapsed age of a valid tracked ACKACK | null when absent; keepalive/clear do not refresh |
| peer estimate age | local acceptance age of a peer Full/Small ACK estimate | null when absent; peer measurement age is unknown |
| last source | latest accepted update kind; both histories can remain | none initially; new socket resets |
| capture age | sampler steady time since metadata was stored | lets UI distinguish capture staleness from observation age |

`embed_set_rtt_data_v1` opts in without extending existing configuration structs.
Each socket/member is read every tick. A retained BROKEN member remains readable;
removed/closed/detached members report absence/failure rather than cached freshness.
A continued named path reads the new socket's fresh history after reconnect.
Groups have no RTT history of their own. Runtime snapshots carry `rtt_metadata`
with availability available/read_failed/unavailable; this is not recorded in old
JSONL sample/history formats or exported as a new Prometheus/OTLP metric. The new
property's standalone schema is `schema/snapshot-rtt-metadata-v1.schema.json`.
No existing field, capability, event or health decision changes meaning.

## Optional local Backup sender authority

Operator question: did the local sender coordinator actually select another
member, or did a parallel probe/replay merely advance that path's DATA counters?
The separately qualified Robotweax `robotweax_srt_group_send_state_v1` V1 getter
is explicitly injected through `embed_set_group_send_state_v1`; null disables
and clears observations. Haivision/older/unqualified builds remain unavailable.
Getter failure is `read_failed`, with no stale selected member exported.

`send_authority` is instantaneous metadata with local-sender scope, never a
health input, RTT window, peer election or guarantee of current reachability.
Socket plus connection generation identifies each active/probe member. Path
aliases are best-effort host labels, not identity proof. Group generation and
revision reset with a new group; revision counts selection/replacement/clear
including initial selection, modulo 2^64. Probe start/cancel does not change it.
A hard fallback can clear then select (two changes). Snapshot polling can miss
intermediate states; revision gaps expose their count, not their identity or
exact time. `send_authority_changed` (event19) records observed differences,
including an initial comparison baseline, separately from existing inferred
`active_member_changed` DATA activity. A group that has never selected a member
(unselected, revision 0: a receiving group, or a sender before its first send)
takes its baseline silently; its first selection then reports `changes=1`.
The detail reads `observed; local_sender; revision=R; changes=N|baseline;
member_generation=G; path=P`, where G is the active member's connection
generation (0 when none), not the group generation. Existing event meanings stay unchanged.
The `authority=unavailable` prefix on the latter refers to that event's own
counter source; a separate authority snapshot may be available.

Ages use local monotonic elapsed seconds; absent active/probe is JSON null.
Capture age reports elapsed time since metadata ingestion; state ages remain
those of the captured instant. Getter reads/statistics clears do not reset SRT
revision history. Disable/re-enable resets SDK comparison baseline. UInt64
generations/revision are decimal strings for exact browser representation.
`sampled_monotonic_nano` is the local poll-start timestamp (nanoseconds, decimal
string), a lower bound on the getter read instant; capture age is measured
since ingestion after that read. Together they bound the observation interval
for independent trace comparison, without inventing an exact transition time.
The JSON contract is `schema/snapshot-send-authority-v1.schema.json`; existing
C sample/config layouts, counter windows and Prometheus metric semantics remain
unchanged. The SDK adds a separate metadata structure/setter and event value.


## Optional Standby RTT observation

Operator question: what was the last accepted local round trip on this Backup
member, how old is it, and what happened to the last monitoring attempt?
The source is the explicitly injected `robotweax_srt_standby_rtt_data_v1` V1
member getter (48 bytes) from a qualified selected Robotweax build. Provider
name alone never enables it. Haivision, older and unqualified builds remain
`unavailable`; a callable getter failure or malformed snapshot is `read_failed`
and discards the previous observation. A successful disabled connection read is
`available` with `negotiated=false`, without a fabricated zero measurement.
Group aggregate handles have no Standby RTT; read members individually.

The independent 64-byte telemetry metadata V1 preserves negotiated, admitted,
observed, fresh-at-capture and retained-terminal flags; the positive single RTT
in microseconds; age in microseconds (`UINT64_MAX` absent); accepted sample count
(saturating); and last attempt outcome. This is instantaneous metadata, not an
estimator, RTT window, counter delta, alert or health input. Admission/capacity
is local monitoring resource availability, not packet loss or DATA health.
`outcome` is historical. A DATA pause may leave `MEASURED` unchanged, as observed
in the qualified fleet v2 run; `PAUSED` does not prove current ineligibility.
Reads and statistics resets do not change measurement history. New connections
and removed/re-added sources start new histories. Getter replacement/null clears
all active and retained observations. Closed/invalid handle reads fail; a still
readable BROKEN handle may retain terminal measurements.

`standby_rtt` in runtime JSON is governed by
[`snapshot-standby-rtt-v1.schema.json`](../../schema/snapshot-standby-rtt-v1.schema.json).
RTT/age use seconds; no observation has null RTT and age. An observed sample can
have null age after a backward provider-clock read. Counts, exact microseconds
and monotonic timestamps are decimal strings to preserve uint64 precision.
`age_seconds` is the provider age at capture; `capture_age_seconds` adds time
since telemetry accepted it, using the local steady clock. `fresh_at_capture`
retains the provider flag, while `fresh` additionally requires total age <= 3 s;
stopped sampling cannot keep an old observation fresh. Fleet scheduling may
legitimately produce stale observations at the 32/s process budget.

Where monitoring was negotiated (or a read failed), the built-in UI shows a
separate Standby RTT column/tile, total age, freshness,
retained terminal status and **last attempt**. Normal DATA RTT, health grades,
active-member authority, counter activity, old JSONL/history, Prometheus and
OTLP metrics are unchanged. This metadata never selects a path or proves DATA
health, reachability, failback eligibility or remote clock accuracy.

Runtime evidence for the selected own source
[`c915a507`](https://github.com/Robotweax/srt/commit/c915a507fc5bdcb58965851a0dad4bb8930df8c2):
[single-group 5/5 Lab run](https://github.com/Robotweax/srt_network_lab/actions/runs/38152172314)
and [fleet v2 run 38170983400](https://github.com/Robotweax/srt_network_lab/actions/runs/38170983400)
(3/3 capacity, fairness and DATA-pause cases). The run qualifies plain own-to-own
loopback runtime behavior, not this telemetry adapter or WAN/mixed-provider/
encrypted-control behavior. The optional native embed test additionally checks
positive observations reach all four member rows on plain own-to-own loopback,
and clearing after getter disable. Other adapter/UI fixtures are synthetic
contract tests, not additional provider qualification.
