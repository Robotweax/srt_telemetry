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

The initial packet requires exactly two retained JSONL incidents. It creates
two counterbalanced arms:

| Arm | First task | Second task |
| --- | --- | --- |
| A | Incident 1, raw statistics | Incident 2, incident report |
| B | Incident 1, incident report | Incident 2, raw statistics |

Each participant sees an incident only once. Across the arms, both incidents
appear under both conditions and both condition orders occur. Automatic arm
selection hashes the pseudonymous participant ID with the study ID; a
facilitator may select an arm explicitly when a pre-registered allocation
requires it.

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
  --study-id constant-loss-comparison-candidate \
  --manifest testdata/golden-incidents/incident-006-jsonl-constant-loss-robotweax-haivision/manifest.json \
  --manifest testdata/golden-incidents/incident-007-jsonl-constant-loss-haivision-robotweax/manifest.json \
  --output-dir /new/operator-evaluation-packet
```

Preparation reproduces both retained analyses, verifies every input digest,
and writes hashed raw, report, and reference artifacts. It refuses to overwrite
an existing packet. Every subsequent command verifies the packet hashes before
using it.

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

The current incidents 006 and 007 are both constant-loss cases. They validate
the counterbalanced method and both provider directions, but cover only one of
the five real or representative failure classes required by strategy Gate B.

## Versioned contracts

- `schema/operator-evaluation-study.schema.json` describes `study.json`.
- `schema/operator-evaluation-result.schema.json` describes completed sessions
  and aggregate summaries.
- raw task, report task, reference, and imported response documents use
  `schemaVersion: 1alpha1` and are validated by the harness before scoring.

Real cross-host studies remain subject to the open clock-quality contract. The
current retained incidents use aligned logical sample indexes and therefore do
not support wall-clock ordering claims between independent machines.
