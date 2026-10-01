# Telemetry contract v0.1

Status: pre-alpha, evidence baseline.

## Observation domains

Every observation retains three independent concepts:

- `observed_at_unix_nano`: wall-clock placement on a cross-endpoint timeline;
- `sample_monotonic_nano`: local interval and ordering authority; and
- SRT source/TSBPD time: transport metadata only, never wall clock.

Clock synchronization quality will be added to the session envelope before
cross-endpoint RCA is enabled.

## Counter collection

Adapters collect lifetime counters without clearing SRT interval counters. The
telemetry delta engine derives deltas from two ordered lifetime snapshots.

The engine establishes a new baseline when:

- it receives its first observation;
- the provider changes;
- monotonic time does not advance; or
- an individual counter decreases.

A counter reset is field-local. Other monotonic counters in the same sample may
still produce valid deltas.

## Semantic separations

The following values must not be collapsed:

- sender loss and receiver loss;
- sender drop and receiver drop;
- physical and unique packet/byte traffic;
- retransmission and first transmission;
- queue occupancy and configured TSBPD latency;
- flow window, congestion window, flight size, and available buffer; and
- unavailable data and observed zero.

The core does not yet derive generic loss or health scores. Those require an
evidence-backed denominator and role-aware interpretation.

Sender-drop totals and deltas count local send/retransmission-buffer removals
due to message TTL or TLPKTDROP. They are separate from network loss and
application delivery loss: a datagram already submitted to UDP may still arrive
after SRT removes its retained copy. The combined counter does not identify the
removal cause. See the [field map](srt-stats-field-map.md#sender-drop-interpretation)
for the pinned Robotweax definition and qualified timing boundary.

## Units

The core contract uses OpenTelemetry-compatible base units:

| Quantity | Contract unit |
| --- | --- |
| duration | seconds (`s`) |
| bytes | octets (`By`) |
| packet counts | `{packet}` |
| rates | bits per second (`bit/s`) |

Adapters convert SRT millisecond and megabit representations at their boundary.

## Data quality

The v1 C ABI reports:

- first-sample baseline;
- monotonic clock regression;
- provider discontinuity;
- per-counter reset; and
- per-field and per-rate availability.

The versioned JSONL recorder exporter preserves these states, including the
difference between unavailable and observed zero. Future exporters, including
OTLP, must do the same and must not drop a quality flag merely because the
destination backend has no native equivalent.

## ABI evolution

Public structures contain `struct_size` and `abi_version`. Version 1 requires
the full current structure, so growing an existing structure would reject
callers built against the older header. New observations therefore arrive in
new structures instead: `ext_sample_v1` and `ext_delta_v1` supplement
`raw_sample_v1` and `delta_v1`, the adapter adds `config_v2`,
`session_create_v2` and `session_sample_ext`, and every v1 entry point keeps its
behaviour. Existing fields are never reinterpreted. Incompatible changes require
a new ABI version.
