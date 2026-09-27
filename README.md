# Robotweax SRT Telemetry

Robotweax SRT Telemetry is an implementation-neutral telemetry foundation for
qualified Robotweax SRT and Haivision SRT versions. It preserves the semantics
of the shared public SRT API while allowing capability-gated Robotweax
extensions to expose additional diagnostic evidence.

## Flight Recorder integration

[Robotweax SRT Flight Recorder](https://github.com/Robotweax/srt_flight_recorder)
owns the local capture workflow, digest-verified `.frcase` packages, support
redaction, offline case view, and product acceptance tests. It consumes this
SDK's versioned JSONL and existing incident analyzer; it does not define a
second metric contract or analysis engine. Follow the product repository's
guide for its demo and fresh instrumented sender/receiver captures.

This repository retains the [native capture host contract](docs/architecture/flight-recorder-capture.md),
bounded trigger-window helper, provider-linked fixture peer, and their tests as
reusable telemetry integration and qualification infrastructure. The peer is an
instrumented fixture host, not a production recorder agent. Human diagnostic
value remains to be measured.

## Status

**Pre-alpha / evidence foundation.** The repository currently contains the
telemetry contract, provider model, versioned C ABI, a non-destructive delta
engine, a bounded in-memory recorder with versioned JSONL snapshot export, a
local explainable incident-analysis candidate, an injected public SRT API
adapter, schemas, and the validation plan. The adapter has deterministic
header/signature coverage and live bidirectional self-loopback coverage for
Robotweax SRT and Haivision SRT v1.5.5. Robotweax-to-Haivision interoperability
also passes in both caller/listener role combinations for the common nine-field
Live/Message IPv4 loopback subset. An initial macOS arm64 candidate also covers
sampling overhead and the required session-destroy-before-socket-close
lifecycle for both providers. Full field, controlled performance, and
deployment-profile qualification remain open. The project intentionally does
not yet contain a production agent, backend, dashboard, alert engine, or
production RCA engine. A second exact macOS arm64 candidate now covers
Robotweax SRT `e3d61f77e852001224b6586c63bcff46a3200223`, the revision
pinned by Intelligent Media Fabric, for the common loopback subset, lifecycle,
and bidirectional Haivision interoperability. It does not inherit the older
revision's incident-class claims, and Linux/IPv6/`fips0` remain open. The local
analyzer turns paired evidence into a
deterministic timeline, five bounded hypotheses, and an explicit
insufficient-evidence outcome. Exact provider-linked peers now record real
adapter samples directly through the bounded recorder and JSONL exporter;
retained constant-loss and RTT-step captures complete that path through the
analyzer in both cross-provider sender directions. Paired receiver-pause
captures now also retain public receive-buffer growth, overlapping sender
flight/send-buffer pressure, lossless recovery, and a supported receiver
endpoint hypothesis in both directions. Exact application-consumption timing
remains a non-claim, and no Robotweax-only extension is required for this
operator question. Paired egress-capacity captures additionally retain proxy
queue growth, public sender-buffer/flight pressure, a receiver buffer below its
conservative pressure threshold, lossless recovery, and a supported sender
endpoint hypothesis. Public statistics do not identify the exact restriction,
but no Robotweax-only extension is required for the bounded localization
question. Paired sequence-hold captures now add the fifth candidate: public
loss and retransmission followed by receiver drops that exactly account for the unique
delivery gap under a configured TSBPD delay. The diagnosis is deliberately
bounded to an exhausted recovery/playout margin and does not claim that a
larger latency setting is the correct remediation. The first deterministic
constant-loss fixture preserves paired sender/receiver evidence and explicit
non-claims for future diagnostic work. Captured constant- and burst-loss runs
in both sender directions additionally retain exact public loss,
retransmission, and recovery observations through a deterministic user-space
fault boundary. The captures
qualify exposed gaps; finite flight-tail recovery without later DATA remains
open. A retained 20 Hz incident-mode experiment now distinguishes eight losses
in one 50 ms window from the same aggregate loss distributed across eight
windows in both cross-provider directions. Therefore no Robotweax-only signal
is currently required for that coarse operator question. Exact within-window
packet adjacency and a controlled release cost budget remain open.
An executable, counterbalanced operator-evaluation candidate now compares eight
real cross-provider recordings as inference-free normalized public SRT
statistics versus deterministic incident reports. It records pseudonymous
timing, accuracy, unsupported hypotheses, missing-evidence recognition, and
confidence. The automated comparison now exercises constant loss, an RTT step,
receiver pause/backpressure, bandwidth reduction with sender queue growth, and
receiver recovery-margin exhaustion. This closes the automated five-class
coverage check, but not the human diagnostic-value gate.
Automated answers are explicitly harness-only;
no design-partner or diagnostic-value product claim has been made.
An immutable eight-scenario internal-rehearsal packet now passes eleven
machine-checked preflight controls, including raw-view inference isolation and
interactive field parity. It contains no participant answers; genuinely
conducted internal sessions remain the next evidence step.

The first product claim to prove is:

> A Robotweax incident timeline identifies the first deviating signals and a
> useful failure domain faster than raw SRT statistics alone.

## Scope

The term implementation-neutral has a bounded meaning in this project:

- Haivision SRT versions qualified against the selected public API profile;
- Robotweax SRT versions qualified against the same profile; and
- optional public Robotweax telemetry extensions declared through explicit
  capabilities.

No support is implied for arbitrary SRT-like products or undocumented private
implementation interfaces.

## Architecture direction

```text
Host-selected SRT implementation
    |
    +-- Public SRT API adapter --------- common evidence baseline
    |
    +-- Robotweax extension adapter ---- optional enhanced evidence
                  |
                  v
          Telemetry core contract
                  |
             Delta engine
                  |
          Bounded recorder
             /          \
  Versioned JSONL     OTLP agent
          |        (after evidence gate)
  Local incident analyzer
```

The SDK never brings a second SRT library into a host process. Adapters consume
the SRT implementation already selected by the host.

## Current invariants

- Collection does not clear SRT interval counters.
- Deltas are derived from available lifetime counters.
- Sender and receiver loss remain separate observations.
- Physical, unique, and retransmitted traffic remain distinguishable.
- Missing provider capabilities are unavailable, never synthetic zeroes.
- Diagnostic hypotheses expose missing and contradictory evidence; unavailable
  signals cannot silently become supporting zeroes.
- Monotonic clock regressions, provider changes, and counter resets form
  explicit data-quality boundaries.
- The common contract uses OpenTelemetry-compatible base units.
- Robotweax-only evidence is capability-gated and must use a public, versioned
  C boundary.

## Build and test

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

To build the optional adapter, point CMake at an installed or generated public
SRT header tree. The adapter accepts `srt_bistats` from the host as an injected
function pointer and does not link another SRT implementation.

```sh
cmake -S . -B build-adapter \
  -DCMAKE_BUILD_TYPE=Release \
  -DROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS=ON \
  -DROBOTWEAX_SRT_TELEMETRY_BUILD_PUBLIC_SRT_ADAPTER=ON \
  -DROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR=/path/to/srt/include
cmake --build build-adapter --parallel
ctest --test-dir build-adapter --output-on-failure
```

Live provider tests are enabled only when both
`ROBOTWEAX_SRT_TELEMETRY_SRT_LIBRARY` and
`ROBOTWEAX_SRT_TELEMETRY_RUNTIME_TEST_PROVIDER` (`robotweax-srt` or
`haivision-srt`) are supplied. These tests open an ephemeral UDP loopback port.

The installed targets are `RobotweaxSRTTelemetry::core` and, when enabled,
`RobotweaxSRTTelemetry::public_srt_adapter`. The public headers are:

```c
#include <robotweax/srt_telemetry.h>
```

```cpp
#include <robotweax/srt_telemetry.hpp>
```

```c
#include <robotweax/srt_telemetry_public_srt.h>
```

## Start here

- [Project handoff for another machine](HANDOFF.md)
- [Telemetry contract](docs/architecture/telemetry-contract.md)
- [Provider and capability model](docs/architecture/provider-model.md)
- [SRT statistics field map](docs/architecture/srt-stats-field-map.md)
- [Public SRT API adapter](docs/architecture/public-srt-adapter.md)
- [Loss-shape observability decision](docs/architecture/loss-shape-observability.md)
- [Bounded recorder](docs/architecture/bounded-recorder.md)
- [Local incident analysis](docs/architecture/incident-analysis.md)
- [Operator evaluation](docs/qualification/operator-evaluation.md)
- [Five-class internal rehearsal packet](qualification/operator-evaluation/five-class-internal-rehearsal-v1/README.md)
- [Golden incident datasets](docs/architecture/golden-incidents.md)
- [Provider qualification matrix](docs/qualification/public-srt-adapter-matrix.md)
- [Robotweax SRT 0.2.6 qualification candidate](qualification/results/2026-09-27-srt-026/README.md)
- [Retained cross-provider evidence](qualification/results/2026-08-28-macos-arm64-public-srt-interop.json)
- [Retained sampling and lifecycle evidence](qualification/results/2026-08-28-macos-arm64-public-srt-runtime.json)
- [Fabric-pinned Robotweax revision interop](qualification/results/2026-08-30-macos-arm64-public-srt-interop-e3d61f7.json)
- [Fabric-pinned Robotweax revision sampling and lifecycle](qualification/results/2026-08-30-macos-arm64-public-srt-runtime-e3d61f7.json)
- [Retained 20 Hz loss-shape evidence](qualification/results/2026-08-29-macos-arm64-loss-shape-observability.json)
- [Constant-loss golden incident](testdata/golden-incidents/incident-001-constant-loss/README.md)
- [Retained constant-loss analysis](testdata/golden-incidents/incident-001-constant-loss/analysis.json)
- [Captured Robotweax-to-Haivision constant-loss incident](testdata/golden-incidents/incident-002-captured-constant-loss-robotweax-haivision/README.md)
- [Captured Haivision-to-Robotweax constant-loss incident](testdata/golden-incidents/incident-003-captured-constant-loss-haivision-robotweax/README.md)
- [Captured Robotweax-to-Haivision burst-loss incident](testdata/golden-incidents/incident-004-captured-burst-loss-robotweax-haivision/README.md)
- [Captured Haivision-to-Robotweax burst-loss incident](testdata/golden-incidents/incident-005-captured-burst-loss-haivision-robotweax/README.md)
- [Direct Robotweax-to-Haivision JSONL analysis](testdata/golden-incidents/incident-006-jsonl-constant-loss-robotweax-haivision/README.md)
- [Direct Haivision-to-Robotweax JSONL analysis](testdata/golden-incidents/incident-007-jsonl-constant-loss-haivision-robotweax/README.md)
- [Robotweax-to-Haivision RTT-step analysis](testdata/golden-incidents/incident-008-rtt-step-robotweax-haivision/README.md)
- [Haivision-to-Robotweax RTT-step analysis](testdata/golden-incidents/incident-009-rtt-step-haivision-robotweax/README.md)
- [Robotweax sender to paused Haivision receiver](testdata/golden-incidents/incident-010-receiver-pause-robotweax-haivision/README.md)
- [Haivision sender to paused Robotweax receiver](testdata/golden-incidents/incident-011-receiver-pause-haivision-robotweax/README.md)
- [Robotweax sender through reduced egress capacity](testdata/golden-incidents/incident-012-bandwidth-reduction-robotweax-haivision/README.md)
- [Haivision sender through reduced egress capacity](testdata/golden-incidents/incident-013-bandwidth-reduction-haivision-robotweax/README.md)
- [Robotweax sender to Haivision receiver recovery-margin exhaustion](testdata/golden-incidents/incident-014-recovery-margin-robotweax-haivision/README.md)
- [Haivision sender to Robotweax receiver recovery-margin exhaustion](testdata/golden-incidents/incident-015-recovery-margin-haivision-robotweax/README.md)
- [Evidence plan](docs/development/evidence-plan.md)
- [Roadmap](docs/roadmap.md)
