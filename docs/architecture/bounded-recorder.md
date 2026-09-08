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

The SDK recorder is still not a persistent recorder. Streaming sample callbacks,
protobuf export, an SDK-level trigger API, a bounded disk spool, and persistent
data-loss accounting remain open.
