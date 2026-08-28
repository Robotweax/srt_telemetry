# Golden incident datasets

Golden incidents are small, deterministic telemetry timelines used to preserve
semantic behavior before a dashboard or RCA engine exists. They answer a
specific operator question and encode both supporting evidence and important
contradictions.

## Evidence classes

Every manifest declares one of two provenance classes:

- `synthetic-contract-fixture` exercises contract and analysis semantics but is
  not evidence of provider runtime behavior;
- `captured-provider-run` contains observations produced by exact provider
  binaries under a retained fault plan.

The `capturedFromProvider` flag must agree with the provenance class. Synthetic
fixtures may name the provider profiles whose semantics they model, but they
must not be used to qualify those providers.

## Dataset format v1

Each incident directory contains:

- `manifest.json`, validated against the golden-incident schema;
- `samples.csv`, whose SHA-256 digest and row count are retained by the
  manifest;
- a generator or capture runner; and
- a short incident-specific explanation.

CSV rows contain raw lifetime observations followed by `expected_*` delta and
rate columns. An empty cell means unavailable. A numeric zero is an explicitly
available zero. The first sample for each role establishes a baseline and must
not invent deltas.

`machineExpectations` make cross-row evidence testable. They currently support
exact values, unavailable fields, and equality to another field plus a fixed
value over an inclusive sample range.

## Validation boundary

Validation has independent layers:

1. the Python validator checks the manifest, dataset digest, aligned timeline,
   provider/role identity, contiguous samples, and machine expectations;
2. the C++ golden test sends every raw row through the production delta engine
   and compares quality flags, availability, counter deltas, and rates; and
3. a synthetic generator's `--check` mode proves byte-for-byte reproducibility
   from its declared model. Captured runs instead retain exact binary hashes,
   fault-proxy observations, and semantic expectations; runtime gauges such as
   RTT need not be byte-identical across reproductions.

A passing synthetic incident proves deterministic SDK behavior only. Provider
qualification still requires a captured run with exact artifacts and a real
fault injection boundary.

The initial capture boundary is a user-space UDP proxy. It classifies an SRT
DATA datagram by the clear high bit in the first header word and keys the first
observation by the 31-bit packet sequence. A fault stage may drop a deterministic
subset of first observations while forwarding later packets with the same
sequence. The proxy records originals, repeats, drops, control traffic, and
reverse traffic for every stage.

The first captured incident demonstrates why injected drops are not renamed as
generic SRT loss. Ten deliberately discarded original datagrams produced ten
retransmissions but nine new sender- and receiver-side loss observations per
interval on the tested profile. All three quantities remain separate evidence.
