# Operator evaluation

`tools/operator_evaluation.py` is the first executable comparison harness for
the diagnostic-value gate. It measures whether an operator identifies the
first deviation and a useful failure domain faster and with fewer unsupported
hypotheses from the deterministic incident report than from normalized public
SRT statistics alone.

This is a study instrument, not product evidence by itself. The automated test
uses synthetic answers and must remain classified as `synthetic-harness`.
Internal rehearsals use `internal-pilot`; only sessions conducted with a design
partner use `design-partner`.

## Study design

The packet requires at least two retained JSONL incidents. The automated packet
uses constant loss, an RTT step, receiver pause/backpressure, bandwidth
reduction with sender queue growth, and receiver recovery-margin exhaustion in
both provider directions. It creates two
counterbalanced arms by alternating the
condition and reversing it for every scenario in the other arm:

| Arm | Task 1 | Task 2 | Task 3 | Task 4 | Task 5 | Task 6 | Task 7 | Task 8 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A | Loss, raw | RTT, report | Robotweax→Haivision receiver pressure, raw | Haivision→Robotweax receiver pressure, report | Robotweax→Haivision sender pressure, raw | Haivision→Robotweax sender pressure, report | Robotweax→Haivision recovery margin, raw | Haivision→Robotweax recovery margin, report |
| B | Loss, report | RTT, raw | Robotweax→Haivision receiver pressure, report | Haivision→Robotweax receiver pressure, raw | Robotweax→Haivision sender pressure, report | Haivision→Robotweax sender pressure, raw | Robotweax→Haivision recovery margin, report | Haivision→Robotweax recovery margin, raw |

Each participant sees an incident only once. Across the arms, every scenario
appears under both conditions and both condition orders occur. Automatic arm
selection hashes the pseudonymous participant ID with the study ID; a
facilitator may select an arm explicitly when a pre-registered allocation
requires it. Paired participant comparisons use each participant's mean within
each condition, while the aggregate statistics retain every individual task.

The raw view contains the direct normalized public SRT snapshot and delta
fields, their roles, and provider identities. It contains no deviation marker,
failure domain, hypothesis, or other analyzer inference. The report view
contains the deterministic timeline, evidence, hypotheses, missing evidence,
and non-claims.

The session records only:

- a pseudonymous participant ID;
- evidence classification and assigned arm;
- milliseconds to the first-deviation answer and failure-domain answer;
- selected failure domain and supported hypotheses;
- identified missing evidence; and
- confidence from 1 through 5.

Do not enter names, email addresses, customer telemetry, private network
inventories, real Stream IDs, or media data. Review free-text notes before any
session artifact is retained in the repository.

## Prepare a packet

```sh
python3 tools/operator_evaluation.py prepare \
  --study-id loss-rtt-pressure-comparison-candidate \
  --manifest testdata/golden-incidents/incident-006-jsonl-constant-loss-robotweax-haivision/manifest.json \
  --manifest testdata/golden-incidents/incident-008-rtt-step-robotweax-haivision/manifest.json \
  --manifest testdata/golden-incidents/incident-010-receiver-pause-robotweax-haivision/manifest.json \
  --manifest testdata/golden-incidents/incident-011-receiver-pause-haivision-robotweax/manifest.json \
  --manifest testdata/golden-incidents/incident-012-bandwidth-reduction-robotweax-haivision/manifest.json \
  --manifest testdata/golden-incidents/incident-013-bandwidth-reduction-haivision-robotweax/manifest.json \
  --manifest testdata/golden-incidents/incident-014-recovery-margin-robotweax-haivision/manifest.json \
  --manifest testdata/golden-incidents/incident-015-recovery-margin-haivision-robotweax/manifest.json \
  --output-dir /new/operator-evaluation-packet
```

Preparation reproduces every retained analysis, verifies every input digest,
and writes hashed raw, report, and reference artifacts. It refuses to overwrite
an existing packet. Every subsequent command verifies the packet hashes before
using it.

For a retained or shared packet, pass `--repository-revision` so its provenance
is pinned. Repository manifests are stored as portable relative labels rather
than host-specific absolute paths.

## Internal facilitation rehearsal

The retained [five-class rehearsal packet](../../qualification/operator-evaluation/five-class-internal-rehearsal-v1/README.md)
contains eight scenarios across the five required classes. Before any internal
session, reproduce its readiness result:

```sh
python3 tools/operator_evaluation.py rehearsal \
  --study qualification/operator-evaluation/five-class-internal-rehearsal-v1/study.json \
  --check qualification/operator-evaluation/five-class-internal-rehearsal-v1/rehearsal-readiness.json
```

The preflight checks packet hashes, pinned provenance, portable sources,
five-class coverage, equal condition counts within both arms, complete
counterbalancing, inference isolation in raw tasks, interactive public-field
parity, report/reference alignment, terminal rendering, and privacy
guardrails. The raw terminal view explicitly includes receiver undecryptable
and configured TSBPD fields because the diagnostic candidates consume them.

Readiness does not create a session and is not participant evidence. Conduct at
least two genuine `internal-pilot` sessions with explicit opposite arms, keep
reference files hidden from participants, and retain outputs outside the
immutable packet. The full facilitator checklist and non-claims are versioned
in `rehearsal-readiness.json`.

## Run one session

```sh
python3 tools/operator_evaluation.py run \
  --study /operator-evaluation-packet/study.json \
  --participant-id partner-001 \
  --evidence-class design-partner \
  --output /new/session-partner-001.json
```

Omit `--arm` for deterministic allocation, or pass `--arm arm-a` or
`--arm arm-b` when following an external allocation schedule. The timer starts
when each task is revealed. A completed session contains answers, timing,
scores, and the hash of its reference answer; outputs are never overwritten.

The non-interactive `score` command accepts a versioned response JSON. This is
intended for another controlled presentation UI and for automated harness
tests, not for fabricating human observations.

## Summarize completed sessions

```sh
python3 tools/operator_evaluation.py summarize \
  --study /operator-evaluation-packet/study.json \
  --session /results/session-partner-001.json \
  --session /results/session-partner-002.json \
  --output /new/operator-evaluation-summary.json
```

The result reports per-condition medians and accuracy, missing-evidence
precision/recall, unsupported hypotheses, confidence, and paired differences.
Positive `rawMinusReport` timing means the incident report was faster.

Claim status is deliberately bounded:

- synthetic sessions produce `harness-only`;
- internal sessions without design-partner data produce
  `internal-pilot-only`;
- design-partner sessions covering fewer than five failure classes remain
  `design-partner-evidence-incomplete`; and
- satisfying the machine checks only makes a result eligible for human gate
  review. It never automatically proves the product claim.

The current automated packet uses Incidents 006, 008, and 010 through 015. It
validates the counterbalanced method across constant-loss, RTT-step,
receiver-pressure, sender-/egress-pressure, and receiver recovery-margin cases.
It covers the five real or representative failure classes required by strategy
Gate B, but the responses remain synthetic harness fixtures and cannot close
the human evidence gate.
Incidents 007 and 009 separately retain the reverse provider direction for the
first two classes.

## Versioned contracts

- `schema/operator-evaluation-study.schema.json` describes `study.json`.
- `schema/operator-evaluation-rehearsal.schema.json` describes the reproducible
  internal-facilitation preflight.
- `schema/operator-evaluation-result.schema.json` describes completed sessions
  and aggregate summaries.
- raw task, report task, reference, and imported response documents use
  `schemaVersion: 1alpha1` and are validated by the harness before scoring.

Real cross-host studies remain subject to the open clock-quality contract. The
current retained incidents use aligned logical sample indexes and therefore do
not support wall-clock ordering claims between independent machines.
