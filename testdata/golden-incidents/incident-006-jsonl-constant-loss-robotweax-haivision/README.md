# Incident 006: real Robotweax-to-Haivision JSONL analysis

Operator question: can exact Robotweax sender and Haivision receiver samples
travel through the public adapter, production delta engine, bounded recorder,
versioned JSONL exporter, and local analyzer without a CSV translation step?

This captured-provider run uses the same deterministic constant-loss boundary
as Incident 002. During samples 3 through 6 the proxy discards every twentieth
first-observed DATA sequence and forwards retransmissions. Each endpoint keeps
nine samples in its SDK recorder and atomically publishes its own JSONL file
when the capture completes.

## Result

- first deviation: sample 3;
- last deviation: sample 6;
- first fully observed recovery: sample 7;
- sender loss: 36 packets;
- receiver loss: 36 packets; and
- retransmissions: 40 packets.

The analyzer selects `transmission-path-loss-with-recovery`. It does not claim
the exact failing network hop or infer unavailable endpoint-pressure signals as
zero. The four-packet difference between public loss and retransmission totals
is retained as provider-specific evidence.

## Retained artifacts

- `sender.jsonl` and `receiver.jsonl` are direct SDK recorder exports;
- `analysis.json` is reproduced from those two files;
- `samples.csv` independently replays the same raw samples through the delta
  contract; and
- `manifest.json` pins all artifact hashes, provider revisions, peer binaries,
  libraries, and proxy observations.

Validate and reproduce the result with:

```sh
python3 qualification/validate_golden_incident.py \
  testdata/golden-incidents/incident-006-jsonl-constant-loss-robotweax-haivision/manifest.json
python3 tools/analyze_incident.py --jsonl \
  testdata/golden-incidents/incident-006-jsonl-constant-loss-robotweax-haivision/sender.jsonl \
  testdata/golden-incidents/incident-006-jsonl-constant-loss-robotweax-haivision/receiver.jsonl
```
