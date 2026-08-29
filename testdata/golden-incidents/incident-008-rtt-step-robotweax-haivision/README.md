# Incident 008: Robotweax-to-Haivision RTT step

Operator question: can paired public SRT RTT measurements distinguish a
recoverable transmission-path delay step without a Robotweax-only extension?

This captured-provider run uses an exact Robotweax sender and Haivision
receiver. A monotonic-clock UDP proxy adds 25 ms one-way delay in both
directions during samples 3 through 6. It sends 1,000 application messages in
each of 12 intervals; later recovery traffic lets the SRT RTT estimators
converge after the proxy delay is removed.

## Result

- first deviation: sample 3;
- paired sender/receiver RTT threshold crossing: samples 4 through 7;
- recovery: sample 8;
- unique sent and received: 12,000 messages each;
- sender and receiver loss: zero; and
- retransmissions: 776 packets, without paired public loss.

The analyzer selects `transmission-path-rtt-step-with-recovery`. It treats the
transient Robotweax-sender retransmissions as supporting delay-response
evidence because delivery is complete and both public loss observations remain
zero. Round-trip measurements cannot identify one-way delay, its direction, or
an exact failing hop.

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
  testdata/golden-incidents/incident-008-rtt-step-robotweax-haivision/manifest.json
python3 tools/analyze_incident.py --jsonl \
  testdata/golden-incidents/incident-008-rtt-step-robotweax-haivision/sender.jsonl \
  testdata/golden-incidents/incident-008-rtt-step-robotweax-haivision/receiver.jsonl
```
