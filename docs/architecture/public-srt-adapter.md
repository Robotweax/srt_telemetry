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
must stop sampling before closing its socket; close/sample race behavior still
requires provider-specific qualification.

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
sampling overhead, and close/sample races remain qualification work.
