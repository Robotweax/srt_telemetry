# Incident 003: captured Haivision-to-Robotweax constant loss

Operator question: do the public statistics on an exact Haivision SRT sender
and Robotweax SRT receiver expose a bounded, recoverable loss interval without
collapsing proxy drops, receiver gaps, sender-learned loss, and retransmission
into one number?

This incident was captured from exact provider-linked peers. A user-space UDP
proxy sits between the Haivision sender and Robotweax receiver, forwards all
SRT control traffic, and drops every twentieth first-observed DATA sequence
during samples 3 through 6. A later packet with the same sequence is forwarded.

## Timeline and observations

| Sample indexes | Proxy behavior | Public telemetry |
| --- | --- | --- |
| 0 | connected baseline | first-sample quality flag; no deltas |
| 1-2 | 201 originals forwarded | zero sender loss, receiver loss, and retransmission |
| 3-6 | 10 of 201 originals dropped; 10 repeated sequences forwarded | sender loss 10, receiver loss 10, retransmission 10, sent physical 211, sent unique 201 |
| 7-8 | 201 originals forwarded | loss and retransmission return to zero |

All 201 application messages arrive in every interval. In this direction the
10 injected datagram drops, 10 public loss increments on each side, and 10
retransmissions happen to be equal. They remain separate observations because
that equality is profile-specific and is not true in Incident 002.

## Evidence boundary

The manifest pins provider revisions, build profiles, peer and library
SHA-256 digests, payload digest, proxy observations, and the telemetry harness
revision. It qualifies only a candidate field relationship for the declared
Haivision-sender/Robotweax-receiver, unencrypted Live/Message IPv4 loopback
profile.

The 201-message schedule deliberately leaves one non-dropped original DATA
packet after the tenth fault in each active interval. This makes every injected
gap observable by the receiver. It does not qualify recovery when the final
packet of a finite flight is lost and no later DATA follows. Logical one-second
timestamps are also not wall-clock throughput measurements. Encryption, IPv6,
FEC, groups, physical network devices, and wide-area paths remain open.

Validate the retained capture with:

```sh
python3 qualification/validate_golden_incident.py \
  testdata/golden-incidents/incident-003-captured-constant-loss-haivision-robotweax/manifest.json
```
