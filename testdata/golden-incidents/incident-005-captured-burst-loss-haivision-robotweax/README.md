# Incident 005: captured Haivision-to-Robotweax burst loss

Operator question: do the public statistics on an exact Haivision SRT sender
and Robotweax SRT receiver expose the severity and recovery of one contiguous
packet-loss burst?

This incident was captured from exact provider-linked peers. During samples 3
through 6, a user-space UDP proxy discards first-observed sender DATA positions
81 through 100 as one contiguous 20-packet burst. Retransmissions and all SRT
control traffic are forwarded.

## Timeline and observations

| Sample indexes | Proxy behavior | Public telemetry |
| --- | --- | --- |
| 0 | connected baseline | first-sample quality flag; no deltas |
| 1-2 | 200 originals forwarded | zero sender loss, receiver loss, and retransmission |
| 3-6 | one 20-packet burst dropped; 20 repeated sequences forwarded | sender loss 20, receiver loss 20, retransmission 20, sent physical 220, sent unique 200 |
| 7-8 | 200 originals forwarded | loss and retransmission return to zero |

All 200 application messages arrive in every interval. Proxy drops,
protocol-observed loss, retransmission, and physical-versus-unique traffic
remain distinct evidence even though their packet counts align in this run.

## Evidence boundary

The manifest pins provider revisions, build profiles, peer and library hashes,
payload digest, the contiguous fault window, proxy observations, and telemetry
harness revision. It qualifies only the declared Haivision-sender/Robotweax-
receiver, unencrypted Live/Message IPv4 loopback profile.

One hundred non-dropped original DATA packets follow the burst, so this run
qualifies an exposed gap rather than finite flight-tail recovery. One-second
public counter deltas quantify loss and recovery but do not by themselves prove
that the 20 losses were adjacent; the proxy fault plan supplies that ground
truth. Logical timestamps are not wall-clock throughput measurements.

Validate the retained capture with:

```sh
python3 qualification/validate_golden_incident.py \
  testdata/golden-incidents/incident-005-captured-burst-loss-haivision-robotweax/manifest.json
```
