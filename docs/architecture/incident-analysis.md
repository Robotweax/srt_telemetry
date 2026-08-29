# Local incident analysis

`tools/analyze_incident.py` is the first deterministic diagnostic proof for the
SDK. It correlates paired sender and receiver evidence into a machine-readable
timeline and evaluates four bounded hypotheses:

- recoverable sender-to-receiver transmission-path loss;
- a recoverable transmission-path RTT step;
- sender-side pressure; and
- receiver-side pressure.

Each hypothesis reports supporting, contradictory, and missing evidence. The
analyzer selects path loss only when aligned sender loss, receiver loss, and
retransmission evidence support it without endpoint-drop contradictions. It
selects an RTT step only when both endpoints cross conservative warm-baseline
thresholds for at least two samples and later recover without public loss or
endpoint-drop contradictions. Warm RTT baselines exclude sample zero because
the SRT estimators can still expose their bootstrap value there; the threshold
is the larger of twice the baseline and baseline plus 20 ms. Receiver pressure
is supported when receiver-buffer growth spans at least two samples, overlaps
sender-buffer or flight growth, and later recovers. Pressure baselines use the
maximum of warm samples 1 and 2; byte gauges must rise by at least 65,536 bytes
and packet gauges by at least eight packets as well as doubling the baseline.
Supported receiver pressure is a competing endpoint explanation and therefore
prevents an overlapping RTT step from being selected as a path-delay incident.
An RTT estimator may remain above its threshold for a bounded sample after the
receiver buffer has drained; that observation remains in the overall timeline
instead of being relabeled as continuing buffer pressure. Otherwise the
selection is `insufficient-evidence`. Unavailable values are never interpreted
as observed zero.

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

The provider-linked incident peer now supplies this format directly. Every
successful public-adapter sample is pushed into an endpoint-local bounded
recorder; the `DONE` barrier atomically publishes the JSONL snapshot. Retained
Incidents 006 through 009 prove this path for constant loss and an RTT step
with exact Robotweax and Haivision libraries in both sender directions. The
receiver-pause candidate additionally consumes public buffer and flight gauges
from the same recorder path. Their CSV views remain an independent
delta-contract replay rather than an input to the retained JSONL analyses.

`--output` refuses to overwrite an existing file. Without it, formatted JSON is
written to standard output; `--compact` produces one compact line. The result
contract is [`incident-analysis-result.schema.json`](../../schema/incident-analysis-result.schema.json).

## Diagnostic boundary

The timeline identifies the first and last sample with a relevant positive
loss/drop delta, paired RTT-threshold deviation, or receiver-buffer pressure,
the first subsequent non-deviating recovery sample, data-quality boundaries,
and evidence totals.
An RTT estimator needs later traffic observations to converge after a delay
step; elapsed wall time alone is not treated as recovery evidence. Source
artifact hashes and exact provider provenance remain attached to the result.

This is a local, rule-based candidate rather than a production RCA engine. It
does not identify an exact failing network hop, infer one-way delay or its
direction from round-trip measurements, infer missing endpoint metrics or exact
application scheduling from public SRT buffer gauges, resolve sequence
adjacency inside a sample window, or establish the product's operator-value
claim. Those limits are emitted as non-claims or missing evidence rather than
hidden behind a generic score.
