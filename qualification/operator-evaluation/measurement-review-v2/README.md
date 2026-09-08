# Measurement-reviewed operator candidate v2

Ten cases / twenty counterbalanced tasks: eight unchanged retained provider
recordings covering five failure classes, plus two explicitly synthetic negative
derivatives. Each arm contains five statistics and five report tasks. Synthetic
cases are evaluation controls, not two additional qualified provider failures.
No participant data, new provider capture or independent human approval exists.

The [rubric](rubric.json) was authored from direct inspection of normalized
measurement traces, without taking expected answers from analyzer selection.
It pins all observations and source manifests, gives sample-level assertions,
and records the reasoning for every expected answer. The separate
`tools/operator_reference_review.py` imports no analyzer and only checks this
case-specific rubric. It is an executable technical review oracle, not another
classifier and not independent human adjudication. Human review stays pending.

The [audit](reference-audit.json) records the changes from analyzer-derived
references. Previously, `missingEvidence` combined gaps from every alternative,
including observed buffer values which simply did not cross a threshold. The
new question asks which additional evidence substantiates the indicated domain
or resolves abstention. A measured zero is available evidence. Relevant causal
unknowns remain required, such as application consumption or a latency
counterfactual. Diagnosis hypotheses and scoring formulas remain unchanged.

These are rubric-dependent candidate scores, not established causal truth.
Before using human scores, a separate domain reviewer must examine every
rationale, verify the relevant public-field contracts and measurements, settle
acceptable alternative answers, and record approval or disputes in a separately
versioned review. Do not change `humanReview` to imply that a technical tool
performed this human review. Avoid comparing or pooling v1 and v2 scores.

## Negative controls

`fixtures/incident-900-synthetic-missing` removes all measurement values and
clears availability masks while preserving clocks and endpoint labels. Expected:
unknown first deviation and recovery, insufficient evidence, no supported cause.

`fixtures/incident-901-synthetic-contradictory` adds seven receiver drops at sample
3 and adjusts later totals, but preserves the original fully delivered unique
packet counts. Expected: first deviation 3, no supported failure-domain cause,
and a request to resolve drop/delivery inconsistency. Counter consistency is
the new missing-evidence option, not a new diagnosis classifier. This deliberately
inconsistent derivative is not claimed as a physically observed provider run.

Both manifests explicitly identify synthetic provenance, their pinned source,
mutation, machine expectations and contradictions. Old capture metadata,
payload digests, CSV expectations and provider-qualification claims are not
copied into the synthetic manifests. Regenerate into a new destination:

```sh
python3 qualification/generate_abstention_fixtures.py --output .cache/negative-fixtures-new
python3 qualification/test_operator_reference_review.py
```

## Reproduction and isolation

The [study](study/study.json) includes the rubric and hashes all task/reference
artifacts. Its provenance identifies the generator source revision used for
this packet. Preparation still uses the shared evaluator with the optional
`reference_review_path` argument (`--reference-review` on its CLI). References
under the v2 policy require their matching rubric at validation and scoring.
Old v1 packets have no rubric and retain their original behavior and hashes.

The twelve reference regression tests reproduce fixtures byte-for-byte, check
actual counter inconsistency/null availability, historical readiness, correct
abstention scoring and rejection of changed measurements/rehashed answers.
The standard CTest suite includes these checks. This readiness is structural
engineering readiness only; the independent human-review gate remains open.

Full packets and rubrics are moderator-only: they reveal reference answers,
source identities and both conditions. Use the Flight Recorder's single-task
HTML exporter for participant handouts. The HTML renderer preserves all numeric
measurements in both conditions; no variant-specific cue is added to raw tasks.
