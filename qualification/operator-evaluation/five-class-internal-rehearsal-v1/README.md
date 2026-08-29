# Five-class internal facilitation rehearsal v1

This immutable candidate packet contains eight real cross-provider scenarios
covering five representative failure classes. It is pinned to telemetry
revision `e4f5f8c7cd1458e3d46b0fbabaa1bbcf340db72b`. Each scenario contains one
inference-free raw-statistics task, one incident-report task, and a hidden
reference answer. `study.json` hashes every task and reference artifact.

`rehearsal-readiness.json` is a reproducible preflight, not participant
evidence. It verifies packet integrity, five-class coverage, arm balance,
counterbalancing, raw-condition inference isolation, the interactive public
field surface, report/reference alignment, renderability, and privacy
guardrails. All eleven checks pass and no blocking issue is recorded.

Before a session, verify the packet from the repository root:

```sh
python3 tools/operator_evaluation.py rehearsal \
  --study qualification/operator-evaluation/five-class-internal-rehearsal-v1/study.json \
  --check qualification/operator-evaluation/five-class-internal-rehearsal-v1/rehearsal-readiness.json
```

Conduct at least two genuine internal sessions, explicitly assigning opposite
arms. Store outputs outside this immutable packet and use only study-specific
pseudonyms:

```sh
python3 tools/operator_evaluation.py run \
  --study qualification/operator-evaluation/five-class-internal-rehearsal-v1/study.json \
  --participant-id internal-001 \
  --evidence-class internal-pilot \
  --arm arm-a \
  --output ../robotweax-srt-telemetry-rehearsal-results/session-internal-001.json

python3 tools/operator_evaluation.py run \
  --study qualification/operator-evaluation/five-class-internal-rehearsal-v1/study.json \
  --participant-id internal-002 \
  --evidence-class internal-pilot \
  --arm arm-b \
  --output ../robotweax-srt-telemetry-rehearsal-results/session-internal-002.json
```

The facilitator must keep the `reference.json` files inaccessible to
participants, reveal one task at a time through the interactive command, avoid
coaching or backtracking, and review optional notes for identifying or
forbidden information before retaining a session. Internal sessions validate
study conduct only; they cannot establish the diagnostic-value product claim.
