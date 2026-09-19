# Receiver-buffer observations from kernel-netem Lab runs

These 18 JSONL files are unchanged archive members from Network Lab PR #48,
commit `61c951af68ab1cbdcac0e2ff542f5bca7f61fb38`:
https://github.com/Robotweax/srt_network_lab/pull/48

Source archive:
`results/2026-09-19-srt-telemetry-netem-followup/evidence.tar.gz`.
`provenance.json` records its SHA-256, member prefix, original member hashes
and the original analyzer/capture outcomes. The archive SHA-256 is
`1771fa406499f69ae3d5c0ab5f788e3a5a29b2e1317d90a87a2c7706e9275188`.

The nine paired runs comprise two baselines, three receiver-pause repetitions
in each provider direction and the longer Haivision-sender pause control.
All seven pauses show 9,212,000 bytes of receiver occupancy above the existing
1,048,576-byte threshold and subsequent recovery. Only the first Robotweax
sender repetition supplies sufficient overlapping upstream evidence for the
existing receiver-pressure hypothesis. The other six retain an
`insufficient-evidence` selection. The longer-pause directory name refers to
the 250-ms sampling interval; the measured application pause was about 1 s.

The operator report now promotes existing `receiver-buffer-growth` evidence
to a separate observation block even when the causal hypothesis is rejected.
No analyzer thresholds, JSON result schema or hypothesis selection changed.
The block identifies the threshold, affected samples, maximum observed
occupancy, recovery, and upstream confirmation. It does not infer application
read behavior from occupancy alone. Missing gauges/baselines are distinguished
from measured values below thresholds.

Run `python3 -B qualification/test_netem_receiver_buffer.py -v` from the SDK
root. The measured test verifies hashes, every recorded hypothesis selection,
both baseline negatives and rendering. Separate synthetic tests remove gauges
or prevent recovery; these controls are not new Lab evidence.

The full source archive retains run metadata, qdisc evidence and application
integrity checks. This compact fixture set supports offline replay only; it
does not constitute a new network or human evaluation.
