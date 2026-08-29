# Incident 013: Haivision sender egress pressure

This reverse captured-provider incident offers 2,000 unpaced message-mode
payloads from the Haivision SRT sender while a deterministic UDP proxy limits
downstream data release to 1,400 packets per second during samples 3 through 6.
The proxy then drains its retained queue at 20,000 packets per second. Samples 1
and 2 provide warm pressure baselines.

The proxy queue peaks at 1,970 packets. Public SRT statistics independently show
the sender buffer reaching 2,439,840 bytes and flight size reaching 1,794
packets, while the Robotweax receiver buffer remains below 3 KiB. Both sender
gauges recover in sample 7. All 2,400 messages arrive without public loss, drop,
undecryptable, or retransmission evidence.

The retained analysis therefore supports `sender-side-pressure`. It does not
claim that public SRT statistics identify the exact capacity restriction: the
packet-release boundary is capture ground truth, while offered application rate
and exact egress capacity remain missing evidence. The common public fields are
sufficient for the bounded sender-endpoint diagnosis, so this candidate does
not require a Robotweax-only SRT extension.
