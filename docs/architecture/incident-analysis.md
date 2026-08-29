# Local incident analysis

`tools/analyze_incident.py` is the first deterministic diagnostic proof for the
SDK. It correlates paired sender and receiver evidence into a machine-readable
timeline and evaluates three bounded hypotheses:

- recoverable sender-to-receiver transmission-path loss;
- sender-side pressure; and
- receiver-side pressure.

Each hypothesis reports supporting, contradictory, and missing evidence. The
analyzer selects path loss only when aligned sender loss, receiver loss, and
retransmission evidence support it without endpoint-drop contradictions.
Otherwise the selection is `insufficient-evidence`. Unavailable values are
never interpreted as observed zero.

## Inputs and correlation contract

The analyzer accepts either a golden-incident manifest or exactly two versioned
JSONL recordings:

```sh
python3 tools/analyze_incident.py \
  --manifest testdata/golden-incidents/incident-001-constant-loss/manifest.json

python3 tools/analyze_incident.py \
  --jsonl sender.jsonl receiver.jsonl \
  --output analysis.json
```

JSONL inputs must contain one sender and one receiver recording with the same
non-empty `flowId`, contiguous zero-based sample indexes, and equal sample
counts. Correlation is currently exact by sample index. The exporter retains
each endpoint's wall and monotonic timestamps, but the analyzer does not use
cross-host wall-clock ordering as evidence until the session contract exposes
clock-synchronization quality.

`--output` refuses to overwrite an existing file. Without it, formatted JSON is
written to standard output; `--compact` produces one compact line. The result
contract is [`incident-analysis-result.schema.json`](../../schema/incident-analysis-result.schema.json).

## Diagnostic boundary

The timeline identifies the first and last sample with a relevant positive
delta, the first fully observed zero-loss recovery sample, data-quality
boundaries, and evidence totals. Source artifact hashes and exact provider
provenance remain attached to the result.

This is a local, rule-based candidate rather than a production RCA engine. It
does not identify an exact failing network hop, infer missing endpoint metrics,
resolve sequence adjacency inside a sample window, or establish the product's
operator-value claim. Those limits are emitted as non-claims or missing
evidence rather than hidden behind a generic score.
