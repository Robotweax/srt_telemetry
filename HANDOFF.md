# Project handoff (2026-09-27)

This is the entry point for continuing **Robotweax SRT Telemetry** on another
machine. It is a status snapshot, not a replacement for the linked evidence or
an authorization to broaden provider claims. Start from the current `main` of
<https://github.com/Robotweax/srt_telemetry>; this snapshot was prepared from
`8f0ff2adcec19966aae93f43b964668030f9c081` (the merge of PR #7). Check
GitHub again for newer commits and open PRs before making changes. At the time
of this snapshot, no Telemetry PR was open.

## Mission and boundaries

Build a telemetry SDK for the **two** supported implementation families:
qualified Robotweax SRT and Haivision SRT versions. The compatible public SRT
API is the common baseline. The host chooses and links its own SRT library;
the SDK must not load a second implementation into the process. Optional
Robotweax-only observations need a documented, versioned public C API and an
explicit capability, coordinated with the Robotweax SRT coding agent. An ABI
match or successful loopback test alone is not a release-qualified metric.

The SDK currently provides a versioned C ABI, non-destructive lifetime-counter
deltas, provider/capability and missing-data models, an injected public
`srt_bistats` adapter, bounded in-memory recording with versioned JSONL export,
and a deterministic local incident-analysis **candidate**. It is pre-alpha.
There is no production agent, backend, dashboard, alert engine, or production
RCA engine in this repository. The separate
[Flight Recorder](https://github.com/Robotweax/srt_flight_recorder) consumes
the SDK's JSONL/analyzer rather than defining another metric contract. The
[Network Lab](https://github.com/Robotweax/srt_network_lab) owns the dedicated
netem test environment; the
[SRT protocol](https://github.com/Robotweax/srt) and its
[knowledge base](https://github.com/Robotweax/srt_knowledge_base) are separate
repositories.

Read [README](README.md), [roadmap](docs/roadmap.md),
[provider model](docs/architecture/provider-model.md),
[field map](docs/architecture/srt-stats-field-map.md), and
[qualification matrix](docs/qualification/public-srt-adapter-matrix.md)
before changing contracts or diagnostics.

## Provenance and current evidence

- PR #6 retained the [Robotweax SRT 0.2.6 qualification candidate](qualification/results/2026-09-27-srt-026/README.md).
  The exact Robotweax release is `v0.2.6` / `50cba37bc89f423ee7c0a292c3c3eae71b5fc7f9`;
  the comparison provider is Haivision `v1.5.5` /
  `b6b4ae990daa8193625a4ddeaeaed03023b23125`. The captured Telemetry
  revision is `4fdc7cf8f643d52ffcbc7def0dd818462a6ba433`, not today's
  `main`. The candidate has bidirectional macOS interop and sampling/lifecycle
  results, 47 Linux CTests against Robotweax, five focused Linux tests against
  Haivision, and 16 Linux Docker/netem runs. All 16 completed application
  delivery with payload verification. Run
  `python3 qualification/results/2026-09-27-srt-026/validate.py` to verify the
  retained archive and matrix.
- The Docker/LinuxKit run had 4 MiB UDP receive/send maxima, versus 32 MiB in
  the earlier dedicated Lab; it is not an identical replay. Receiver-pause
  runs showed public receive-buffer growth and recovery, but most failed the
  stricter sender-overlap capture gate. One Haivision-to-Robotweax repetition
  selected a path-loss hypothesis after one loss/retransmission event despite
  zero qdisc drops and complete application delivery. Treat this as an
  investigation question, not proof of physical-path loss.
- At 1,400 pps with Robotweax sending, three runs reported 585/590/583
  `sender_drop_packet_delta` events while every application payload arrived.
  `pktSndDropTotal` counts local send/retransmission-buffer removal under
  TTL/TLPKTDROP semantics; it does **not** prove network loss or lost
  application payload. The 2,800-pps follow-up had no sender drops. Preserve
  the distinct capture-gate result, analyzer result, qdisc observation, and
  application-delivery result; do not tune thresholds merely to make a gate
  pass.
- PR #7 added explicit receiver-pause socket/drop/TTL settings, opt-in
  per-message sequence verification, and `TRANSFER_FAILED`/completed-message
  accounting to the incident peer. Its sender completion count means accepted
  send calls, not remote application reception. See
  [qualification usage](qualification/README.md).
- The [operator evaluation](docs/qualification/operator-evaluation.md) has an
  automated five-class comparison and an [immutable eight-scenario internal
  rehearsal packet](qualification/operator-evaluation/five-class-internal-rehearsal-v1/README.md).
  Eleven preflight checks pass, but no genuine participant results exist; the
  product's diagnostic-value claim remains unproven.

## Next work, in priority order

1. Coordinate a dedicated-Linux-Lab receiver-pause replay in both provider
   directions. Use the [Lab assignment](docs/qualification/receiver-backpressure-lab-followup.md)
   and the 0.2.6 candidate as the comparison baseline. Record effective SRT
   and kernel buffers, actual pause/resume and application send/read timing,
   payload integrity, qdisc and public counters, raw paired JSONL, analyzer
   output, binary revisions/hashes, and cleanup evidence. Preserve rejected
   runs. Examine the single apparent path-loss selection with packet-level
   evidence before changing the analyzer.
2. Have a human facilitator conduct at least two genuine, counterbalanced
   internal operator sessions using the immutable packet and its privacy
   controls. Store pseudonymous outputs outside the packet; automated harness
   answers are not human evidence. Only then assess the proposed diagnostic
   value claim.
3. Continue exact-version field/profile qualification and controlled sampling
   budgets before publishing common or Robotweax-specific release
   capabilities. Open items include reset/reconnect, additional address
   families and modes, encryption/FEC/groups, long-duration behavior, and
   representative deployment performance. Coordinate any new protocol signal
   with the protocol agent and qualify its public C boundary independently.

Do not promote the 0.2.6 candidate to a blanket provider capability, infer
Haivision 1.5.7 behavior from 1.5.5, or claim a human diagnostic improvement
from the automated rehearsal.

## Resume on a new machine

```sh
git clone https://github.com/Robotweax/srt_telemetry.git
cd srt_telemetry
git fetch origin
git status --short --branch
gh pr list --repo Robotweax/srt_telemetry --state open

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python3 qualification/results/2026-09-27-srt-026/validate.py
python3 tools/operator_evaluation.py rehearsal \
  --study qualification/operator-evaluation/five-class-internal-rehearsal-v1/study.json \
  --check qualification/operator-evaluation/five-class-internal-rehearsal-v1/rehearsal-readiness.json
```

These commands need Git, CMake, a C/C++ toolchain, Python 3, and `gh` for the
PR query. The basic SDK tests do not require a provider installation. For the
optional public adapter and live provider tests, use the exact header/library
options and host-selected implementation described in [README](README.md) and
[qualification instructions](qualification/README.md). Live tests need local
UDP socket permission. A Docker/netem replay additionally needs an authorized
Linux environment with namespace/qdisc privileges; do not assume that a
checkout or a temporary path from the original machine exists on the new one.
