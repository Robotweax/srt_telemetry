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

Future recorder and OTLP exporters must preserve these states. They must not
drop a quality flag merely because the destination backend has no native
equivalent.

## ABI evolution

Public structures contain `struct_size` and `abi_version`. Version 1 requires
the full current structure. Future revisions may append fields but must not
reinterpret existing fields. Incompatible changes require a new ABI version.
