# Public SRT API adapter

The public adapter samples either Robotweax SRT or Haivision SRT through the
shared v1.5.5-compatible `srt_bistats` surface. The host injects the function
pointer and socket handle. Consequently, the telemetry library neither chooses
nor links a second SRT implementation into the process.

## Integration contract

The host creates a `robotweax_srt_telemetry_public_srt_config_v1` with:

- one of the two declared providers;
- a live socket owned by the host;
- the provider's `srt_bistats` function;
- evidence-backed capabilities; and
- only fields qualified for the exact provider/build/profile.

The socket and function pointer must remain valid until the session is
destroyed. Calls using the same session must be externally synchronized. A host
must stop sampling and destroy the telemetry session before closing its socket.
The adapter cannot synchronize a host-owned socket lifetime, so concurrent
close/sample behavior is not supported.

Every sample calls the injected function as:

```c
bistats(socket, &statistics, 0, 1)
```

`clear=0` preserves application-owned interval counters. `instantaneous=1`
requests current queue gauges. The adapter uses lifetime counters and monotonic
timestamps for its own deltas and rates.

## Availability and failure behavior

`qualified_fields` is an allowlist, not a promise that every runtime sample has
a value. Signed provider counters or gauges that are negative are unavailable.
Invalid duration values are likewise unavailable; neither case is emitted as a
synthetic zero. A failed native sample returns
`ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE`, retains the native return
code, and exposes no raw fields.

The `ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_V1_5_5_CANDIDATE_FIELDS` mask is a
mapping candidate only. Production configurations must use the relevant
qualification record rather than enabling that mask by assumption.

## Extended statistics

`robotweax_srt_telemetry_public_srt_session_create_v2` takes a
`config_v2` with an additional `qualified_ext_fields` allowlist.
`robotweax_srt_telemetry_public_srt_session_sample_ext` then fills the v1
result and an `ext_result_v1` (extended sample plus extended deltas) from the
same `srt_bistats` call. Together they cover all 52 mapped observations listed
in the [field map](srt-stats-field-map.md).

Creation is rejected when the extended allowlist:

- contains bits outside the configured provider's candidate mask, which
  includes the other provider's interpretation of `mbpsBandwidth`
  (`ESTIMATED_LINK_CAPACITY` for Haivision, `DATA_ARRIVAL_RATE` for Robotweax);
- qualifies packet-filter counters without `CAP_FEC_STATS_V1`; or
- is non-empty without `CAP_COMMON_STATS_V1`.

Sessions created through the v1 call report no extended fields. Use the
provider-specific candidate masks
(`..._HAIVISION_V1_5_5_EXT_CANDIDATE_FIELDS`,
`..._ROBOTWEAX_EXT_CANDIDATE_FIELDS`) and the group masks
(`..._GROUP_CANDIDATE_FIELDS`, `..._GROUP_EXT_CANDIDATE_FIELDS`) as starting
points for a qualification record, not as blanket claims.

A group handle can be sampled by a statistics session, but the
[group session](public-srt-group.md) reads the group together with its
members, their states, addresses and path names in one call.

## Current evidence boundary

Automated checks compile the exact callback type and the mapping implementation
against Robotweax SRT and Haivision SRT v1.5.5 headers. Deterministic fake
statistics verify non-destructive sampling, missing-value handling, unit
conversion, physical-versus-unique traffic, delta generation, and native error
propagation.

Provider-linked tests also establish real loopback connections and exchange
payloads in both directions. They sample both endpoints and require physical
and unique packet and wire-byte deltas plus RTT availability. These are
self-provider runtime checks.

A separate two-process harness also passes Robotweax-to-Haivision exchange in
both caller/listener role combinations. That retained qualification covers the
nine-field common subset on unencrypted Live/Message IPv4 loopback. The
remaining candidate fields, IPv6, encrypted/FEC/group profiles, reset behavior,
and release performance budgets remain qualification work.

A retained macOS arm64 candidate run now covers sampling microbenchmarks and
the lifecycle contract for both providers. It also contains a deliberately
unsupported, time-bounded close/sample race probe: both providers returned an
explicit native sampling failure without exposing fields. This containment
observation does not relax the required session-destroy-before-socket-close
order.

Two retained constant-loss captures exercise the cross-provider public loss,
retransmission, and physical-versus-unique mappings with each implementation
as sender once. The Haivision-sender schedule leaves a later DATA sequence
after every injected loss; finite flight-tail recovery without later DATA is
therefore still outside the qualified boundary.

Two additional captures apply one contiguous 20-packet burst with each
implementation as sender. Both directions recover all application messages
and preserve loss, retransmission, and physical-versus-unique magnitudes. The
public interval counters do not expose sequence adjacency, so burst-shape
classification requires retained fault ground truth or a future, separately
qualified signal when sampled only once per second.

A subsequent wall-clock-paced 20 Hz experiment compares equal eight-packet
aggregate loss in eight separate windows with one eight-packet window. Both
public implementations expose the coarse shape in both sender directions with
zero complete-window deadline misses. This supports an incident-mode 20 Hz SDK
profile for that operator question and defers a Robotweax-only extension. Exact
sequence adjacency inside a 50 ms window remains unavailable through the
public counters.
