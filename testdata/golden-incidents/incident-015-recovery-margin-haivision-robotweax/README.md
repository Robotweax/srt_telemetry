# Incident 015: Robotweax receiver recovery margin

This reverse captured-provider incident sends 1,100 message-mode payloads from
the Haivision SRT sender while a deterministic UDP proxy retains the first 100
original DATA sequences and every observed repeat. The Robotweax receiver
completes after 1,000 payloads; the proxy releases its 300 retained packet
copies only after the receiver has crossed its recovery/playout boundary.
Samples 1 and 2 provide the warm baseline, and samples 7 and 8 prove normal
post-incident delivery.

Public statistics show paired sender-learned and receiver-observed loss,
sender retransmission, exactly 100 receiver drops, and a positive configured
receiver TSBPD delay. The sender records 1,500 unique packets across the whole
capture while the receiver records 1,400; the receiver drops therefore account
exactly for the delivery gap. All deviation counters return to zero in sample
4 and the following transfers deliver normally.

The retained analysis supports `receiver-recovery-margin-exhausted`. It does
not claim that increasing latency is the correct remediation: public
non-destructive statistics expose neither belated-arrival counters nor the
counterfactual outcome under another latency setting. The shared public fields
are sufficient for this bounded diagnosis, so no Robotweax-only extension is
selected.
