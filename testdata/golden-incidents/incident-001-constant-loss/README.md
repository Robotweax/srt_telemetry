# Incident 001: constant packet loss

Operator question: can a paired sender/receiver timeline keep receiver-observed
gaps, sender-learned loss, and retransmission traffic distinct, and can it show
when that evidence returns to normal?

This is a deterministic synthetic contract fixture. It models a Robotweax SRT
sender and Haivision SRT receiver, but it was not captured from either runtime
and therefore does not qualify provider behavior.

## Timeline

| Sample indexes | State | Expected evidence |
| --- | --- | --- |
| 0 | baseline | first-sample quality flag; no deltas |
| 1-2 | healthy | zero loss and retransmission deltas |
| 3-6 | constant loss | five receiver gaps, five sender-learned losses, and five retransmissions per second |
| 7-8 | recovered | loss and retransmission deltas return to zero |

During the active window, sent physical packets and bytes include the five
retransmissions while unique traffic remains at 100 packets per second. Drop
and undecrypt counters are deliberately unavailable; the fixture does not turn
that absence into zero.

The fixture cannot locate real network loss, qualify clock synchronization, or
prove that the named provider versions emit these exact observations under a
real impairment. Those claims require a captured provider run.

## Reproduce and validate

```sh
python3 testdata/golden-incidents/incident-001-constant-loss/generate.py --check
python3 qualification/validate_golden_incident.py \
  testdata/golden-incidents/incident-001-constant-loss/manifest.json
python3 tools/analyze_incident.py \
  --manifest testdata/golden-incidents/incident-001-constant-loss/manifest.json
```

The CTest suite additionally feeds `samples.csv` through the production delta
engine and checks every declared delta, rate, availability bit, and quality
flag. It also reproduces [`analysis.json`](analysis.json), which records the
first deviation at sample 3, the recovery at sample 7, three explainable
hypotheses, and the analysis non-claims.
