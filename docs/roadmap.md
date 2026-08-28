# Roadmap

## Phase 0: evidence foundation — complete

- common provider contract;
- versioned C ABI;
- non-destructive lifetime-counter delta engine;
- availability and data-quality model;
- exact SRT field mapping;
- adapter capability schema; and
- golden-incident manifest.

## Phase 1: recorder and provider qualification — current

- bounded in-memory recorder complete; callback and JSONL/protobuf export open;
- public SRT API adapter using the host-selected library complete;
- Robotweax SRT header/signature and bidirectional self-loopback candidate
  profile complete, full profile open;
- pinned Haivision SRT v1.5.5 header/signature and bidirectional self-loopback
  candidate profile complete, full profile open;
- Robotweax-to-Haivision Live/Message IPv4 loopback interop complete in both
  caller/listener role combinations for the common nine-field subset;
- microbenchmarks and close/sample lifecycle tests; and
- first golden incident datasets.

## Phase 2: diagnostic proof

- local incident timeline;
- three explainable hypotheses;
- explicit insufficient-evidence outcome;
- paired sender/receiver correlation experiment; and
- design-partner comparison with raw statistics.

## Phase 3: pipeline pilot

- OTLP signal mapping;
- agent-versus-Collector decision record;
- bounded persistent spool;
- self-observability and data-loss accounting;
- cardinality allowlist; and
- measured storage and egress economics.

## Phase 4: on-prem beta

- incident-centric dashboard;
- alert grouping and suppression;
- calibrated deterministic RCA;
- authentication, RBAC, audit, retention, and redaction; and
- representative two-node, long-duration, and upgrade tests.
