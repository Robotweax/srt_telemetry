# Evidence plan

## Milestone 0: telemetry contract

Exit requires:

- every common field has source, unit, side, temporality, reset semantics, and
  missing-data behavior;
- Robotweax and pinned Haivision qualification records exist;
- collection never clears application-owned SRT interval counters;
- a recorder can preserve raw snapshots and data-quality boundaries; and
- fault scenarios are deterministic and reproducible.

Header compatibility and deterministic adapter mapping are necessary evidence,
but do not satisfy the provider qualification requirement by themselves. The
remaining gate requires live sockets, both relevant data directions, explicit
reset observations, lifecycle races, and measured sampling overhead.

The first retained cross-provider result covers live sockets and both
caller/listener role combinations for the common nine-field Live/Message IPv4
loopback subset. A second retained macOS arm64 result covers the supported
session-destroy-before-socket-close order, bounded failure probes, and an
initial sampling microbenchmark. It does not establish a release performance
budget, supported close/sample concurrency, or close the reset, encrypted,
IPv6, FEC, group, remaining-field, and multi-platform gates.

## Initial fault corpus

1. constant loss — synthetic contract fixture plus captured Robotweax-to-
   Haivision and Haivision-to-Robotweax gap-loss candidates complete;
   flight-tail behavior and deployment profiles open;
2. burst loss — captured 20-packet gap-loss candidates complete in both
   provider directions; public interval counters quantify magnitude and
   recovery but do not independently identify within-interval adjacency;
3. RTT step and RTT ramp;
4. bandwidth reduction with queue growth;
5. receiver pause/backpressure;
6. hard path outage;
7. reconnect loop; and
8. key-management or decrypt failure where the provider exposes evidence.

Every retained incident must name exact revisions, roles, directions, mode,
address family, encryption, FEC, fault plan, timing source, payload digest, and
expected observations. A Robotweax-to-Robotweax result is self-evidence, not a
Haivision interoperability qualification.

## Robotweax telemetry extension gate

Do not extend Robotweax SRT pre-emptively. First demonstrate that the common
public field set cannot answer a concrete operator question or discriminate a
required failure class.

The paired burst captures established one candidate gap: one-second public
counter deltas answer how many packets were lost and recovered, but not whether
those losses were adjacent. The retained 20 Hz comparison now answers the
narrower operator question of whether equal aggregate loss was concentrated in
one 50 ms observation window or distributed across several windows. It passes
in both provider directions, so the selected common approach is incident-mode
20 Hz SDK sampling and no Robotweax-only signal is required for that question.

Exact sequence adjacency within a 50 ms window remains unproven. Reopen this
gate only if that precision is required by a concrete operator workflow or a
new failure-class experiment cannot discriminate through public fields. Then
compare the public fallback against a versioned Robotweax event or maximum-run
metric and coordinate the selected contract with the Robotweax SRT coding
agent.

Use this request template when coordination with the Robotweax SRT coding agent
is necessary:

```yaml
operator_question: string
rca_hypothesis: string
missing_evidence: string
existing_public_fields_considered: []
proposed_signal:
  name: string
  type: counter | gauge | event | state
  unit: string
  observation_side: sender | receiver | group | process
  temporality: lifetime | interval | instantaneous
  reset_behavior: string
lifecycle: string
security_and_privacy: string
expected_sampling_frequency_hz: number
hot_path_overhead_budget: string
haivision_fallback: unavailable | weaker_hypothesis
required_tests: []
```

An accepted extension must use a versioned installed C surface and update the
Robotweax public contract, manifest, symbols, installed consumers, tests, and
documentation together.

## Diagnostic-value gate

Before a production backend is selected, compare a local incident timeline
with raw SRT statistics for real or representative incidents. Measure:

- time to detect the first deviation;
- time to identify a useful failure domain;
- incorrect or unsupported hypotheses;
- missing evidence; and
- operator confidence in the explanation.

The first local analyzer candidate now establishes the machine-testable side of
this gate: a paired timeline, three explicit hypotheses, supporting,
contradictory, and missing evidence, plus an insufficient-evidence outcome. It
passes the synthetic and captured loss incidents. Two retained cross-provider
runs now also prove the real public-adapter → recorder → JSONL → analyzer path
in both directions. This does not close the product claim. A design-partner
comparison must still measure the operator outcomes above, and real
cross-endpoint correlation must declare clock-synchronization quality before
wall-clock ordering is used as RCA evidence.

The first executable comparison harness now prepares hash-verified,
counterbalanced raw-statistics and incident-report tasks from retained
recordings, captures the two timing milestones and bounded operator answers,
and emits paired aggregate statistics. Its automated end-to-end test uses only
`synthetic-harness` responses and cannot satisfy this gate. Incidents 006 and
007 also share the same constant-loss failure class, while strategy Gate B
requires at least five real or representative failure classes. The next
evidence step is an internal facilitation rehearsal followed by controlled
design-partner sessions; neither should use cross-host wall-clock ordering
until clock quality is declared.
