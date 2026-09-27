# Robotweax SRT 0.2.6 telemetry qualification candidate

Status: **candidate evidence, not a blanket provider capability claim**.
The public adapter needs no source change for this release. This record separates
the capture gate from the analyzer decision; a rejected gate is not a failed
transfer or necessarily an analyzer abstention.

## Exact inputs

| Component | Revision / profile |
| --- | --- |
| Robotweax SRT | `v0.2.6`, `50cba37bc89f423ee7c0a292c3c3eae71b5fc7f9`, Release shared OpenSSL |
| Haivision SRT | `v1.5.5`, `b6b4ae990daa8193625a4ddeaeaed03023b23125`, Release shared OpenSSL |
| Telemetry SDK | `4fdc7cf8f643d52ffcbc7def0dd818462a6ba433` |
| Network Lab runner | `eacd531b63f07cedf6bee5c09867c31320053b08` |

The macOS ARM64 public-API interoperability runner passed both caller/listener
directions. Its sampling/lifecycle runner passed for both providers. The SDK's
47 CTests passed against Robotweax 0.2.6 on Linux ARM64. Five focused public
adapter/runtime/recording tests passed against Haivision 1.5.5 on Linux. The
same 47 tests passed individually on macOS; four live tests needed a second run
with local UDP socket access because the initial sandbox denied bind.

The Linux experiment used an Ubuntu 24.04 ARM64 container on Docker Desktop's
LinuxKit kernel. The Network Lab runner created fresh sender/receiver network
namespaces and a direct veth pair per run. `tc netem` acted on the sender's
namespace-local interface for bandwidth cases. All 16 runs report successful
infrastructure, checked namespace cleanup, and complete 2,400/2,400 or
7,400/7,400 application transfers. The peer verifies each received message's
payload bytes before reporting transfer completion. The container's actual
`net.core.rmem_max` and `net.core.wmem_max` were both 4,194,304 bytes; the
earlier Network Lab used 33,554,432 bytes, so the runs are not an identical
environmental replay. Docker rejected attempts to set the larger values at
container creation.

Linux image: `robotweax-srt-sensor-netem:m4d-ttl-corrected`, image ID
`sha256:15a9b00b33bfbc85ddf127b727b2524d001f2056e16f76f38be0c31e6d83773a`.
Kernel: `7.0.12-linuxkit`; compiler: Ubuntu GCC 13.3.0; Docker client/server:
29.8.0. The Robotweax source was exported from the release tag, and the
Haivision source was cloned from the official `v1.5.5` tag. Both were built as
Release shared libraries with OpenSSL enabled. The container was started with
`NET_ADMIN` and `SYS_ADMIN` for namespace-local veth/qdisc setup; the runner
then removed all created namespaces.

## Linux results

| Scenario | Direction | Capture gate | Analyzer |
| --- | --- | --- | --- |
| Baseline | Both, one each | 2/2 pass | 2/2 insufficient evidence (expected without a fault) |
| Receiver pause | Robotweax → Haivision, three | 1 pass, 2 rejected | 1 receiver pressure, 2 insufficient evidence |
| Receiver pause | Haivision → Robotweax, three | 0 pass, 3 rejected | 2 insufficient evidence, 1 transmission-path-loss-with-recovery |
| Bandwidth, 1,400 pps | Robotweax → Haivision, three | 0 pass, 3 rejected | 3 sender pressure |
| Bandwidth, 1,400 pps | Haivision → Robotweax, three | 3/3 pass | 3 sender pressure |
| Bandwidth, 2,800 pps | Robotweax → Haivision, one | pass | sender pressure |
| Receiver pause, 250 ms samples | Haivision → Robotweax, one | rejected | insufficient evidence |

At 1,400 pps with Robotweax sending, sample 7 contained **585, 590 and 583**
`sender_drop_packet_delta` respectively. All three application transfers
completed with verified payload bytes and 2,400/2,400 unique-packet counts.
The qdisc reported zero drops. This is consistent with the documented local
TTL/TLPKTDROP retransmission-buffer removal semantics; these runs do not
individually trace packet submission and ACKs, so that mechanism is supported
by the earlier packet-level investigation rather than reproved here. The
2,800-pps follow-up had no sender drops and passed the stricter pure
queue-growth gate. Sender pressure remained supported at both rates.

Every receiver-pause run showed receiver-buffer growth in samples 3–6 and
later recovery. The rejected runs lacked the sender-buffer/flight overlap
required by the current causal rule. One Haivision → Robotweax repetition
also recorded one sender/receiver loss event and one retransmission, causing
the analyzer to select path loss. The qdisc reported no drops and application
delivery completed; this classification is a follow-up question, not proof of
a faulty physical path. No diagnostic threshold was changed to make the matrix
pass. The receiver-buffer observation remains independently visible in the
operator report.

The macOS loopback captures used the SDK's user-space proxy, not netem. Both
bandwidth directions passed their gates and selected sender pressure. Both
receiver-pause directions showed receiver-buffer growth but failed their
capture gates and selected insufficient evidence. Those four captures are kept
separately in the archive so they cannot be mistaken for the kernel runs.

## Evidence and limits

[`evidence.tar.gz`](evidence.tar.gz) contains the two macOS provider results,
four macOS incident captures, the two Linux CTest logs, and all 16 Linux run directories. Each Linux
directory retains `run.json`, paired JSONL, analyzer output, sample CSV,
qdisc/socket snapshots, artifact hashes, and cleanup readback. Archive SHA-256:

`c3e9e73e698e9e616cfbdcae3612c5dcc779ea1c6d650fc4ff9280e3515fb548`

Run `python3 qualification/results/2026-09-27-srt-026/validate.py` from this
repository to verify the archive and the recorded matrix. This candidate
does not qualify WAN behavior, encryption/FEC/groups, maximum throughput,
sampling cost as a release gate, or Haivision 1.5.7. The Linux container's
UDP maxima and Docker scheduler differ from the prior dedicated Lab VM.

Next: repeat the receiver-pause cases on the dedicated Linux Lab with its
declared UDP buffer maxima and capture actual application pause timing. Review
the single path-loss classification with packet-level evidence before changing
the analyzer or promoting 0.2.6 to a broader provider capability profile.
