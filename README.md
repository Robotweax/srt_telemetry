# Robotweax SRT Telemetry

Robotweax SRT Telemetry is an implementation-neutral telemetry foundation for
qualified Robotweax SRT and Haivision SRT versions. It preserves the semantics
of the shared public SRT API while allowing capability-gated Robotweax
extensions to expose additional diagnostic evidence.

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
production RCA engine. The local analyzer turns paired evidence into a
deterministic timeline, three bounded hypotheses, and an explicit
insufficient-evidence outcome. Exact provider-linked peers now record real
adapter samples directly through the bounded recorder and JSONL exporter; two
retained captures complete that path through the analyzer in both
cross-provider sender directions. The first deterministic constant-loss
fixture preserves paired sender/receiver evidence and explicit non-claims for
future diagnostic work. Captured constant- and burst-loss runs in both sender
directions additionally retain exact public loss, retransmission, and recovery
observations through a deterministic user-space fault boundary. The captures
qualify exposed gaps; finite flight-tail recovery without later DATA remains
open. A retained 20 Hz incident-mode experiment now distinguishes eight losses
in one 50 ms window from the same aggregate loss distributed across eight
windows in both cross-provider directions. Therefore no Robotweax-only signal
is currently required for that coarse operator question. Exact within-window
packet adjacency and a controlled release cost budget remain open.
An executable, counterbalanced operator-evaluation candidate now compares the
two real cross-provider recordings as inference-free normalized public SRT
statistics versus deterministic incident reports. It records pseudonymous
timing, accuracy, unsupported hypotheses, missing-evidence recognition, and
confidence. Automated answers are explicitly harness-only; no design-partner
or diagnostic-value product claim has been made.

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

- [Telemetry contract](docs/architecture/telemetry-contract.md)
- [Provider and capability model](docs/architecture/provider-model.md)
- [SRT statistics field map](docs/architecture/srt-stats-field-map.md)
- [Public SRT API adapter](docs/architecture/public-srt-adapter.md)
- [Loss-shape observability decision](docs/architecture/loss-shape-observability.md)
- [Bounded recorder](docs/architecture/bounded-recorder.md)
- [Local incident analysis](docs/architecture/incident-analysis.md)
- [Operator evaluation](docs/qualification/operator-evaluation.md)
- [Golden incident datasets](docs/architecture/golden-incidents.md)
- [Provider qualification matrix](docs/qualification/public-srt-adapter-matrix.md)
- [Retained cross-provider evidence](qualification/results/2026-08-28-macos-arm64-public-srt-interop.json)
- [Retained sampling and lifecycle evidence](qualification/results/2026-08-28-macos-arm64-public-srt-runtime.json)
- [Retained 20 Hz loss-shape evidence](qualification/results/2026-08-29-macos-arm64-loss-shape-observability.json)
- [Constant-loss golden incident](testdata/golden-incidents/incident-001-constant-loss/README.md)
- [Retained constant-loss analysis](testdata/golden-incidents/incident-001-constant-loss/analysis.json)
- [Captured Robotweax-to-Haivision constant-loss incident](testdata/golden-incidents/incident-002-captured-constant-loss-robotweax-haivision/README.md)
- [Captured Haivision-to-Robotweax constant-loss incident](testdata/golden-incidents/incident-003-captured-constant-loss-haivision-robotweax/README.md)
- [Captured Robotweax-to-Haivision burst-loss incident](testdata/golden-incidents/incident-004-captured-burst-loss-robotweax-haivision/README.md)
- [Captured Haivision-to-Robotweax burst-loss incident](testdata/golden-incidents/incident-005-captured-burst-loss-haivision-robotweax/README.md)
- [Direct Robotweax-to-Haivision JSONL analysis](testdata/golden-incidents/incident-006-jsonl-constant-loss-robotweax-haivision/README.md)
- [Direct Haivision-to-Robotweax JSONL analysis](testdata/golden-incidents/incident-007-jsonl-constant-loss-haivision-robotweax/README.md)
- [Evidence plan](docs/development/evidence-plan.md)
- [Roadmap](docs/roadmap.md)
