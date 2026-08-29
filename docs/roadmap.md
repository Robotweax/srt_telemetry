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

- bounded in-memory recorder and versioned stable-snapshot JSONL callback export
  complete; streaming callbacks and protobuf export open;
- public SRT API adapter using the host-selected library complete;
- Robotweax SRT header/signature and bidirectional self-loopback candidate
  profile complete, full profile open;
- pinned Haivision SRT v1.5.5 header/signature and bidirectional self-loopback
  candidate profile complete, full profile open;
- Robotweax-to-Haivision Live/Message IPv4 loopback interop complete in both
  caller/listener role combinations for the common nine-field subset;
- macOS arm64 sampling microbenchmark and close/sample lifecycle candidate
  complete; controlled multi-platform release budgets and stress profiles open;
- wall-clock-paced 20 Hz loss-shape candidate complete in both cross-provider
  directions; common incident sampling selected for the coarse 50 ms question,
  exact within-window adjacency and a release cost gate open; and
- constant- and burst-loss captured cross-provider gap candidates complete in
  both sender directions; flight-tail/profile coverage and the remaining
  initial corpus open.

## Phase 2: diagnostic proof — current

- deterministic local incident timeline candidate complete;
- four explainable hypothesis candidates complete, including a public-API RTT
  step with warm-baseline thresholds and observed recovery;
- explicit insufficient-evidence outcome complete;
- synthetic correlation plus direct provider-linked recorder-to-analyzer
  candidates complete in both cross-provider sender directions; real
  cross-endpoint clock-quality contract open; and
- versioned, counterbalanced raw-statistics versus incident-report evaluation
  harness complete; internal rehearsal, design-partner observations, and
  coverage of at least five real or representative failure classes open.

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
