# SRT statistics field map

This map defines candidate mappings from the public SRT v1.5.5
`SRT_TRACEBSTATS` surface into the common telemetry contract.

Robotweax SRT mappings are backed by its released public statistics contract.
Haivision mappings remain qualification candidates until executed against an
exact pinned Haivision version in both relevant directions.

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
| sender drop packets | `pktSndDropTotal` | combined TTL and negotiated TLPKTDROP causes in v1.5.5 |
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
