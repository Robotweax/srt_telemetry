# Incident 002: captured Robotweax-to-Haivision constant loss

Operator question: do the public statistics on an exact Robotweax SRT sender
and Haivision SRT receiver expose a bounded, recoverable loss interval without
collapsing proxy drops, receiver gaps, sender-learned loss, and retransmission
into one number?

This incident was captured from exact provider-linked peers. A user-space UDP
proxy sits between the Robotweax sender and Haivision receiver, forwards all
SRT control traffic, and drops every twentieth first-observed DATA sequence
during samples 3 through 6. A later packet with the same sequence is forwarded.

## Timeline and observations

| Sample indexes | Proxy behavior | Public telemetry |
| --- | --- | --- |
| 0 | connected baseline | first-sample quality flag; no deltas |
| 1-2 | 200 originals forwarded | zero sender loss, receiver loss, and retransmission |
| 3-6 | 10 of 200 originals dropped; 10 repeated sequences forwarded | sender loss 9, receiver loss 9, retransmission 10, sent physical 210, sent unique 200 |
| 7-8 | 200 originals forwarded | loss and retransmission return to zero |

All 200 application messages arrive in every interval. The difference between
10 injected datagram drops and 9 public loss increments is retained rather than
normalized away: the public loss counters represent protocol-observed loss,
not a generic count of every UDP datagram discarded below SRT. Retransmission
and physical-versus-unique traffic provide the complementary evidence.

## Evidence boundary

The manifest pins provider revisions, build profiles, peer and library
SHA-256 digests, payload digest, proxy observations, and the telemetry harness
revision. It qualifies only a candidate field relationship for the declared
Robotweax-sender/Haivision-receiver, unencrypted Live/Message IPv4 loopback
profile.

Logical one-second timestamps make the checked-in delta-rate replay
deterministic; they are not wall-clock throughput measurements. This capture
does not qualify the reverse provider direction, encryption, IPv6, FEC, groups,
physical network devices, or wide-area paths.

Validate the retained capture with:

```sh
python3 qualification/validate_golden_incident.py \
  testdata/golden-incidents/incident-002-captured-constant-loss-robotweax-haivision/manifest.json
```
