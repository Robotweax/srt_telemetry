# Incident 010: Robotweax sender, paused Haivision receiver

This captured-provider incident sends 7,000 message-mode payloads from the
Robotweax SRT peer while the Haivision SRT receiver application performs no
reads during samples 3 through 6. The measured read pause was approximately
417 ms. Samples 1 and 2 establish zero-valued warm pressure baselines.

The public SRT statistics show receiver-buffer growth across all four active
samples, peaking at 9,212,000 bytes. Sender flight or send-buffer growth
overlaps samples 3 through 5. After the receiver application resumes, the
buffer returns below the threshold at sample 7. All 7,400 messages in the
complete capture arrive; public loss, drop, undecryptable, and retransmission
counters remain zero.

The retained JSONL analysis therefore supports `receiver-side-pressure` and
keeps exact application-consumption timing as missing evidence. The common
public fields localize the receiver endpoint for this candidate, so no
Robotweax-only SRT extension is required. This loopback capture does not qualify
physical networks, production scheduling behavior, encryption, FEC, or groups.
