# SRT statistics field map

This map defines candidate mappings from the public SRT v1.5.5
`SRT_TRACEBSTATS` surface into the common telemetry contract.

Robotweax SRT mappings are backed by its released public statistics contract.
The adapter and its exact `srt_bistats` callback signature compile against both
the Robotweax public header and pinned Haivision SRT v1.5.5. Haivision mappings
for physical and unique packet/byte totals plus RTT have now executed on live
cross-provider sockets against that exact pinned version in both caller/listener
role combinations. The remaining mappings stay qualification candidates.

Collection uses `clear = 0`. The adapter derives rates from lifetime counters
and monotonic sample time rather than taking ownership of the host's interval
reset boundary.

## Common counter candidates

| Contract observation | Public field | Semantics retained |
| --- | --- | --- |
| sent physical packets | `pktSentTotal` | includes successful retransmission datagrams |
| received physical packets | `pktRecvTotal` | may include physical duplicates |
| sent unique packets | `pktSentUniqueTotal` | first successful transmission |
| received unique packets | `pktRecvUniqueTotal` | newly accepted source DATA |
| sender loss packets | `pktSndLossTotal` | loss learned when a NAK newly queues retransmission |
| receiver loss packets | `pktRcvLossTotal` | newly observed receiver gaps |
| retransmitted packets | `pktRetransTotal` | retransmission datagrams actually sent |
| sender drop packets | `pktSndDropTotal` | local send/retransmission-buffer removals from message TTL or negotiated TLPKTDROP; not proof of network or application payload loss |
| receiver drop packets | `pktRcvDropTotal` | local receiver drop action |
| receiver undecryptable packets | `pktRcvUndecryptTotal` | protected DATA rejected at decrypt/auth boundary |
| sent physical wire bytes | `byteSentTotal` | SRT DATA plus documented IP/UDP overhead |
| received physical wire bytes | `byteRecvTotal` | SRT DATA plus documented IP/UDP overhead |
| sent unique wire bytes | `byteSentUniqueTotal` | unique DATA wire accounting |
| received unique wire bytes | `byteRecvUniqueTotal` | unique DATA wire accounting |
| retransmitted wire bytes | `byteRetransTotal` | retransmission wire accounting |

`pktRcvBelated` is not currently promoted to the lifetime-counter baseline: the
public layout exposes it as an interval value without a matching lifetime field.
It requires an explicit interval-ownership policy or an enhanced Robotweax
counter before reliable shared collection.

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

## Gauge candidates

| Contract observation | Public field | Conversion / note |
| --- | --- | --- |
| RTT | `msRTT` | milliseconds to seconds |
| flight size | `pktFlightSize` | packets |
| flow window | `pktFlowWindow` | packets; not "available bytes" |
| congestion window | `pktCongestionWindow` | packets |
| available send buffer | `byteAvailSndBuf` | bytes |
| available receive buffer | `byteAvailRcvBuf` | bytes |
| send buffer occupancy | `byteSndBuf` | sender DATA wire bytes |
| send buffer span | `msSndBuf` | milliseconds to seconds |
| receive buffer occupancy | `byteRcvBuf` | retained application payload bytes |
| receive buffer span | `msRcvBuf` | milliseconds to seconds; not latency margin |
| sender TSBPD delay | `msSndTsbPdDelay` | milliseconds to seconds |
| receiver TSBPD delay | `msRcvTsbPdDelay` | milliseconds to seconds |

## Deliberately excluded shortcuts

- `mbpsSendRate` and `mbpsRecvRate` are not the canonical rate source. Their
  interval depends on the statistics reset boundary and physical accounting.
- `packets_lost` is not a canonical metric because it erases observation side.
- `packets_dropped_late` is not a canonical metric because it conflates drop
  causes and belated arrival.
- `window_available` is not a canonical metric because the source surfaces use
  different packet and byte quantities.
- `receive_buffer_ms` is not labeled latency margin. Margin is a derived,
  configuration-aware quantity to be validated separately.

## Qualification record required

Each adapter/provider version must record:

- exact provider version and build flags;
- role and data direction;
- Live/Message or File/Stream profile;
- address family, encryption, FEC, and group profile;
- field availability and observed reset behavior;
- counter relationships under loss, duplicate, reorder, and retransmission;
- sample-call overhead and close/sample race behavior; and
- any divergence from this candidate map.
