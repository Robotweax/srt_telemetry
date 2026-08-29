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
3. RTT step — deterministic public-API capture and diagnostic candidate
   retained in both cross-provider directions; RTT ramp open;
4. bandwidth reduction with queue growth;
5. receiver pause/backpressure — public receiver-buffer plus upstream sender
   flight/send-buffer diagnostic candidates retained in both provider
   directions; physical-network and production-scheduler profiles open;
6. receiver recovery/playout margin exhaustion — public loss, retransmission,
   configured TSBPD delay, receiver-drop, and delivery-gap candidates retained
   in both provider directions; a larger-latency counterfactual remains open;
7. hard path outage;
8. reconnect loop; and
9. key-management or decrypt failure where the provider exposes evidence.

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

The receiver-pause captures provide a second extension-gate decision. Common
public receiver-buffer, sender-buffer, and flight gauges localize the receiver
endpoint and its recovery in both provider directions. They do not expose the
exact application-consumption schedule, but that missing precision is not
required for the current operator question. No Robotweax extension is selected
for this failure class.

The reduced-egress-capacity captures provide a third extension-gate decision.
Common public sender-buffer and flight gauges localize sender-/egress-side
pressure and recovery while the receiver buffer remains below its conservative
pressure threshold. The exact capacity restriction and offered application rate
remain unavailable, but the proxy retains those facts as experiment ground
truth. That precision is not required for the current endpoint-localization
question, so no Robotweax extension is selected for this failure class.

The sequence-hold captures provide a fourth extension-gate decision. Common
public loss, retransmission, configured TSBPD-delay, receiver-drop, and unique
delivery counters support a bounded exhausted-recovery-margin diagnosis in
both provider directions. Belated-arrival counters and the counterfactual under
a larger latency are unavailable, so the analyzer explicitly does not prescribe
that remediation. Those missing fields are not required for the current
failure-boundary question; no Robotweax extension is selected.

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
this gate: a paired timeline, five explicit hypotheses, supporting,
contradictory, and missing evidence, plus an insufficient-evidence outcome. It
passes the synthetic and captured loss incidents. Ten retained cross-provider
runs now also prove the real public-adapter → recorder → JSONL → analyzer path
for five failure classes in both directions. This does not close the product
claim. A design-partner comparison must still measure the operator outcomes
above, and real
cross-endpoint correlation must declare clock-synchronization quality before
wall-clock ordering is used as RCA evidence.

The first executable comparison harness now prepares hash-verified,
counterbalanced raw-statistics and incident-report tasks from retained
recordings, captures the two timing milestones and bounded operator answers,
and emits paired aggregate statistics. Its automated end-to-end test uses only
`synthetic-harness` responses and cannot satisfy this gate. The updated
automated packet contains constant loss, an RTT step, receiver
pause/backpressure, bandwidth reduction with sender queue growth, and receiver
recovery-margin exhaustion in both provider directions. The harness therefore
exercises all five real or representative failure classes required by strategy
Gate B. This closes only the automated coverage prerequisite. An immutable
eight-scenario rehearsal packet and its eleven-check machine preflight are now
retained. The next evidence step is to conduct at least two genuine internal
sessions on opposite arms, then proceed to controlled design-partner sessions.
None should use cross-host wall-clock ordering until clock quality is declared.
