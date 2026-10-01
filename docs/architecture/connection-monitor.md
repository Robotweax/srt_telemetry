# Connection monitor

The connection monitor reports what the statistics cannot: whether a socket is
connected, why a connection was rejected, which settings the two ends
negotiated, and whether encryption works. Dashboards show it in their status
strip and on the event timeline.

```c
#include <robotweax/srt_telemetry_public_srt_host.h>  /* includes the host's srt.h */

robotweax_srt_telemetry_public_srt_connection_config_v1 config = {0};
config.struct_size = sizeof(config);
config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
config.socket = socket;
config.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
robotweax_srt_telemetry_host_connection_functions(&config);

robotweax_srt_telemetry_public_srt_connection_monitor *monitor = NULL;
robotweax_srt_telemetry_public_srt_connection_monitor_create(&config, &monitor);
/* about once per second: */
robotweax_srt_telemetry_public_srt_connection_monitor_poll(monitor, unix_ns, mono_ns, &result);
```

As with statistics, the SDK never links an SRT library. The host injects
`srt_getsockstate`, `srt_getrejectreason` and `srt_getsockflag`. The optional
header `srt_telemetry_public_srt_host.h` provides inline wrappers for that, so
a host that already includes `srt.h` needs one call.

## Snapshot

| Field | Source | Notes |
| --- | --- | --- |
| `state` | `srt_getsockstate` | `SRT_SOCKSTATUS` values (5 = connected) |
| `reject_reason` | `srt_getrejectreason` | `SRT_REJ_*`; 0 = none |
| `receiver_latency_seconds`, `peer_latency_seconds` | `SRTO_RCVLATENCY`, `SRTO_PEERLATENCY` | negotiated |
| `overhead_percent` | `SRTO_OHEADBW` | retransmission allowance over input rate |
| `max_bandwidth_bytes_per_second` | `SRTO_MAXBW` | -1 unlimited, 0 relative to input |
| `input_bandwidth_bytes_per_second`, `min_input_bandwidth_bytes_per_second` | `SRTO_INPUTBW`, `SRTO_MININPUTBW` | 0 = estimated |
| `loss_max_ttl_packets` | `SRTO_LOSSMAXTTL` | reorder tolerance cap |
| `sender_drop_delay_seconds` | `SRTO_SNDDROPDELAY` | negative = sender drop off |
| `too_late_packet_drop`, `tsbpd_mode`, `periodic_nak_report`, `enforced_encryption` | `SRTO_TLPKTDROP`, `SRTO_TSBPDMODE`, `SRTO_NAKREPORT`, `SRTO_ENFORCEDENCRYPTION` | 0/1 |
| `payload_size_bytes`, `flow_control_packets` | `SRTO_PAYLOADSIZE`, `SRTO_FC` | |
| `peer_srt_version`, `local_srt_version` | `SRTO_PEERVERSION`, `SRTO_VERSION` | `0xXXYYZZ` |
| `km_state`, `send_km_state`, `receive_km_state` | `SRTO_KMSTATE`, `SRTO_SNDKMSTATE`, `SRTO_RCVKMSTATE` | `SRT_KM_STATE` values (2 = secured) |
| `key_length_bytes`, `km_refresh_rate_packets`, `km_preannounce_packets` | `SRTO_PBKEYLEN`, `SRTO_KMREFRESHRATE`, `SRTO_KMPREANNOUNCE` | |
| `packet_filter` | `SRTO_PACKETFILTER` | negotiated FEC configuration; empty without FEC |
| `stream_id` | `SRTO_STREAMID` | **only when `capture_stream_id = 1`** |

Every field has an availability bit. A failed option read is unavailable, not
zero. When the socket no longer exists, the snapshot still carries the state
(`NONEXIST`) and the reject reason, so a disconnect is reported rather than
silently missing. Text options longer than the buffer are unavailable rather
than truncated.

Stream IDs often identify customers or channels. The monitor never reads them
unless the host opts in.

## Changes and events

Each poll compares the snapshot with the previous one. `changed_fields` lists
every field whose value or availability changed, and `events` summarises:

| Event | When |
| --- | --- |
| `FIRST_SNAPSHOT` | first poll |
| `STATE_CHANGED` | any state change |
| `CONNECTED` | state became connected (also on the first poll if already connected) |
| `DISCONNECTED` | state left connected |
| `REJECTED` | a non-zero reject reason appeared (also on the first poll) |
| `KM_STATE_CHANGED` | any of the three encryption states changed |
| `SETTINGS_CHANGED` | a negotiated setting changed (latency, bandwidth, filter, ...) |
| `PEER_VERSION_CHANGED` | the peer's SRT version changed |

`KM_STATE_CHANGED`, `SETTINGS_CHANGED` and `PEER_VERSION_CHANGED` come only
from value changes of fields readable in both polls. A closing socket makes
every option unreadable; that shows up in `changed_fields` and as
`DISCONNECTED`, not as a renegotiation.

`REJECTED` means the reject reason changed to a non-zero `SRT_REJ_*` value.
Those values also cover timeouts and closes, not only refusals, and a repeated
rejection with the same code is not reported again. Monitor caller or accepted
sockets: on a listening socket SRT keeps only the latest incoming rejection.

`previous_state` and `previous_km_state` let a timeline show "connecting →
connected" or "securing → secured" without keeping its own history. Recording
events in a bounded journal is part of Phase 2.

Failed option reads (for example on a closed socket) set the calling thread's
`srt_getlasterror` in both providers. Hosts that rely on that value should poll
the monitor from a different thread or read the error first.

## Provider differences found

Observed on loopback with Haivision SRT v1.5.5 and Robotweax SRT `main`:

| Situation | Haivision SRT | Robotweax SRT |
| --- | --- | --- |
| After a connect timeout or rejection | state 2 (`OPENED`) | state 6 (`BROKEN`) |
| Wrong passphrase | reject reason 10 (`BADSECRET`) | reject reason 10 (`BADSECRET`) |
| Peer closes | state 6 (`BROKEN`) | state 6 (`BROKEN`) |
| `SRTO_CONGESTION` readback | `"live"` | not readable (write-only), so not part of the snapshot |
| Sender key state right after connect | `SECURED` | briefly `SECURING`, then `SECURED` |

Dashboards should therefore treat "not connected and a non-zero reject reason"
as a rejection regardless of the exact state value.

## Tests

- `robotweax_srt_telemetry_public_srt_connection_tests`: injected functions;
  unit conversion, 1/4/8-byte option widths, stream-ID opt-in, every event
  (including a later rejection and a peer version change), teardown without
  false renegotiation events, vanished sockets, bounded text, validation.
- `robotweax_srt_telemetry_public_srt_connection_events_test`: live loopback on
  each provider: plain connection and peer close, encrypted connection reaching
  `SECURED` with a 16-byte key, wrong passphrase reported as `BADSECRET`, and a
  negotiated FEC filter string. The monitor starts after the connection is
  established, so the live test does not observe a `SECURING → SECURED`
  transition; the unit test covers that event.
