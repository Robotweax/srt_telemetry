# Robotweax SRT Telemetry

Robotweax SRT Telemetry is an implementation-neutral telemetry foundation for
qualified Robotweax SRT and Haivision SRT versions. It preserves the semantics
of the shared public SRT API while allowing capability-gated Robotweax
extensions to expose additional diagnostic evidence.

## Status

**Pre-alpha / evidence foundation.** The repository currently contains the
telemetry contract, provider model, versioned C ABI, a non-destructive delta
engine, initial schemas, and the validation plan. It intentionally does not yet
contain a production agent, backend, dashboard, alert engine, or RCA engine.

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
        Recorder / callback (next)
                  |
          OTLP agent (after evidence gate)
```

The SDK never brings a second SRT library into a host process. Adapters consume
the SRT implementation already selected by the host.

## Current invariants

- Collection does not clear SRT interval counters.
- Deltas are derived from available lifetime counters.
- Sender and receiver loss remain separate observations.
- Physical, unique, and retransmitted traffic remain distinguishable.
- Missing provider capabilities are unavailable, never synthetic zeroes.
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

The installed target is `RobotweaxSRTTelemetry::core`. The public headers are:

```c
#include <robotweax/srt_telemetry.h>
```

```cpp
#include <robotweax/srt_telemetry.hpp>
```

## Start here

- [Telemetry contract](docs/architecture/telemetry-contract.md)
- [Provider and capability model](docs/architecture/provider-model.md)
- [SRT statistics field map](docs/architecture/srt-stats-field-map.md)
- [Evidence plan](docs/development/evidence-plan.md)
- [Roadmap](docs/roadmap.md)
