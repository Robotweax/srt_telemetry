# Bounded recorder

The recorder is an in-memory pre-trigger buffer for paired raw samples and
derived deltas. Its complete record array is allocated at creation. Push, read,
and clear operations do not grow the buffer.

Records are read oldest first. Once full, a push overwrites the oldest record
and increments a saturating overwrite counter. `clear` creates an empty logical
buffer and resets overwrite accounting without reallocating storage. Capacity
is fixed and currently limited to 1,000,000 records to reject accidental,
unbounded allocations.

The C and C++ APIs synchronize individual operations. A sequence of size and
read calls is not a transaction: concurrent writers may change the oldest
record between calls. The versioned JSONL exporter is the stable-snapshot
boundary. It copies the complete logical ring under the recorder lock and then
serializes and invokes the sink after releasing the lock. Concurrent pushes can
therefore continue while a fixed snapshot is exported.

## JSONL snapshot export

`robotweax_srt_telemetry_recorder_export_jsonl` emits one recording header and
then one oldest-first sample per callback. Every callback receives exactly one
newline-terminated UTF-8 JSON document. A non-zero sink result stops the export
with `ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE`.

The header retains the stream and shared flow identities, endpoint role, exact
provider build provenance, telemetry version, record count, capacity, and
overwrite count. Sample documents retain raw and delta availability masks,
quality boundaries, and every v1 value. Unavailable values are JSON `null`;
an observed zero remains numeric `0`. The corresponding line schema is
[`telemetry-jsonl-line.schema.json`](../../schema/telemetry-jsonl-line.schema.json).

Export is an incident/control-path operation, not a sampling hot-path
operation. It allocates a snapshot plus serialized lines. The caller owns
persistence, retry, and sink backpressure behavior; a failed callback may have
already accepted earlier complete lines. No disk spool is implied.

A [host-owned Flight Recorder window](flight-recorder-capture.md) now composes
these rings for bounded manual trigger capture without changing the SDK API.
The native window and fixture peer remain telemetry integration infrastructure;
the [Flight Recorder product repository](https://github.com/Robotweax/srt_flight_recorder)
owns capture orchestration, case packaging, redaction, and the case view. It
reuses this SDK's JSONL contract and incident analyzer.

## Schema 1alpha2

`robotweax_srt_telemetry_recorder_export_jsonl_v2` writes schema `1alpha2`
(srt_telemetry#47), a strict superset of `1alpha1` that SRT Doctor reads from
rule set 0.1.11. Every `1alpha1` field keeps its meaning; every line carries
`"schemaVersion":"1alpha2"`.

- **Header** adds `groupType` (`backup`, `broadcast`, `unknown`) and
  `groupRow` (`group`, `member`), both `null` for a plain socket and set
  together; `healthLines` (health was graded), `healthCount` (health lines in
  the file) and `healthOverwrittenCount` (transitions older than the one in
  force at the first kept sample, or dropped from the transition ring).
- **Samples** add `member`: on member rows `{state, standby, authority}` with
  SRT's member state (`pending`, `idle`, `running`, `broken`, `unknown`), the
  host's standby decision and the local sender authority (`active`, `probe`,
  `none`, or `null` on receivers and without a sender-authority source); `null`
  on other rows. Push it with `recorder_push_member_v1`; a sample pushed with
  plain `recorder_push` reads `unknown`.
- **Health lines** (`kind` `robotweax-srt-telemetry-health`) follow the sample
  they were graded on: `sampleIndex`, `observedAtUnixNano`, `verdict`,
  `reason`, `knob`, `knobDirection` (`none`, `increase`, `decrease`, `check`),
  `suggestedValue`, `capped`, `uncappedVerdict`, `standby`, `usablePaths` and
  `expectedPaths` (integers on a group row, else `null`),
  `verdictSinceUnixNano` and `carried`. Names are the health engine's. Push a
  transition with `recorder_push_health_v1` after the sample it was graded on;
  a status with a verdict, reason, knob or knob direction the engine does not
  name, or a non-finite suggested value, is refused (`INVALID_ARGUMENT`), so
  every kept transition can be written. The recorder keeps the transition in
  force at the first kept sample and those graded on kept samples (several
  can share one sample when its source's samples fail), at most
  2 × capacity + 2 of 64 bytes each, dropping the oldest. When the oldest
  samples are overwritten, the transition in force at the first kept sample
  comes first, marked `carried`; a transition on that sample itself
  supersedes it.

`recorder_export_jsonl` still writes `1alpha1` from the same recorder, leaving
these out. `sdk/tests/test_recording_1alpha2.cpp` checks the line order, the
carried line and the bounds; `test_recording_schema.py` validates its output
against the line schema.

The [embedding library](embedding.md#recordings) uses one recorder per source
to write recordings (`jsonl=DIR`): the same export, written as a complete file
when a socket is detached, when its source is removed and at stop. The SDK
recorder itself is still not a persistent recorder. Protobuf export, an
SDK-level trigger API, a bounded streaming disk spool, and persistent
data-loss accounting remain open.
