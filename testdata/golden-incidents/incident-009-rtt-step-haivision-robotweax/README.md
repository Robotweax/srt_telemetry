# Incident 009: Haivision-to-Robotweax RTT step

Operator question: does the same public-API RTT diagnosis hold when provider
roles are reversed?

This captured-provider run uses an exact Haivision sender and Robotweax
receiver. A monotonic-clock UDP proxy adds 25 ms one-way delay in both
directions during samples 3 through 6. It sends 1,000 application messages in
each of 12 intervals and retains recovery traffic after delay removal.

## Result

- first deviation: sample 3;
- paired sender/receiver RTT threshold crossing: samples 4 through 7;
- recovery: sample 8;
- unique sent and received: 12,000 messages each;
- sender and receiver loss: zero; and
- retransmissions: zero.

The analyzer selects `transmission-path-rtt-step-with-recovery` with no
contradictory public loss, drop, or undecryptable evidence. The result confirms
the common public fields in the reverse provider direction, but makes no claim
about one-way delay, its direction, an exact failing hop, physical devices, or
wide-area behavior.

## Retained artifacts

- `sender.jsonl` and `receiver.jsonl` are direct SDK recorder exports;
- `analysis.json` is reproduced from those two files;
- `samples.csv` independently replays the same raw samples through the delta
  contract; and
- `manifest.json` pins all hashes, exact provider provenance, the delay plan,
  monotonic timing source, and per-stage proxy observations.

Validate and reproduce the result with:

```sh
python3 qualification/validate_golden_incident.py \
  testdata/golden-incidents/incident-009-rtt-step-haivision-robotweax/manifest.json
python3 tools/analyze_incident.py --jsonl \
  testdata/golden-incidents/incident-009-rtt-step-haivision-robotweax/sender.jsonl \
  testdata/golden-incidents/incident-009-rtt-step-haivision-robotweax/receiver.jsonl
```
