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
record between calls. Consumers that require a stable incident snapshot must
stop writes or add an external synchronization boundary before exporting it.

This component is deliberately not yet a persistent recorder. JSONL/protobuf
serialization, callbacks, trigger windows, disk spooling, and data-loss
accounting remain Phase 1 work.
