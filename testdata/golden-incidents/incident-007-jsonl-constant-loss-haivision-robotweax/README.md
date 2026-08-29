# Incident 007: real Haivision-to-Robotweax JSONL analysis

Operator question: does the direct recorder-to-analyzer path preserve the same
diagnostic boundary when provider roles are reversed?

This captured-provider run uses an exact Haivision sender and Robotweax
receiver. It sends 201 messages per interval so a non-dropped DATA sequence
follows the tenth deliberate gap. During samples 3 through 6 both public loss
counters and retransmission increase by ten packets per sample. Each endpoint
atomically publishes nine real adapter samples through the SDK JSONL exporter.

## Result

- first deviation: sample 3;
- last deviation: sample 6;
- first fully observed recovery: sample 7;
- sender loss: 40 packets;
- receiver loss: 40 packets; and
- retransmissions: 40 packets.

The analyzer selects `transmission-path-loss-with-recovery` while retaining the
exact-hop, flight-tail, deployment-profile, and unavailable-pressure limits as
non-claims or missing evidence.

## Retained artifacts

- `sender.jsonl` and `receiver.jsonl` are direct SDK recorder exports;
- `analysis.json` is reproduced from those two files;
- `samples.csv` independently replays the delta contract; and
- `manifest.json` pins hashes, exact provider provenance, and the fault-proxy
  ground truth.

Validate and reproduce the result with:

```sh
python3 qualification/validate_golden_incident.py \
  testdata/golden-incidents/incident-007-jsonl-constant-loss-haivision-robotweax/manifest.json
python3 tools/analyze_incident.py --jsonl \
  testdata/golden-incidents/incident-007-jsonl-constant-loss-haivision-robotweax/sender.jsonl \
  testdata/golden-incidents/incident-007-jsonl-constant-loss-haivision-robotweax/receiver.jsonl
```
