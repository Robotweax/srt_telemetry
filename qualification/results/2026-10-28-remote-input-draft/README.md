# Experimental remote input — 2026-10-28

Review draft, representation/history part1. See the [proposal](../../../docs/proposals/remote-observation-input.md)
and [build instructions](../../../experimental/remote_input/README.md). No production
BL adapter, released API/schema, dependency pin or existing ABI1 behavior changes.

## Executed results

- macOS ARM64: **62/62 CTests pass** at the final draft source, including the
  existing C ABI, delta/metrics/sampler/health, recording1alpha2–4, closed recording
  schema, required OTLP-Protobuf and HTTP/export regressions.
- New draft tests: **83 C++ assertions and27 schema negatives**, pass on both macOS
  and Linux ARM64. The schema test additionally checks70 u64 boundary values,
 3 actual output lines, exact contexts/escaping/opaque byte preservation, unknown
  producer wall time versus real epoch0, and old recording-schema refusal.
- Final Linux ASan/UBSan/LSan sweep: **60/61 CTests pass**; the existing HTTP suite
  fails its ping-flood response-time bound. This is NOT a green full Linux suite.
  Linux has no Node executable, so the standby UI JavaScript test is unconfigured;
  it passed in the macOS suite. Both runs require the OTLP-Protobuf test and use
  jsonschema4.26.0 / opentelemetry-proto1.45.0.
- An unmodified archive of main341adc0 was built with the same sanitizer flags.
  Its HTTP/export pair also fails the ping-flood bound; that baseline additionally
  failed a history-pagination assertion. Neither source/deadline was changed.
  The initial parallel failure and final serial failure remain in the evidence.
  These local ARM64 sanitizer results do not identify a general root cause or
  claim that every environment is affected.
- Both installed Core/HTTP C consumers build and run under Linux sanitizers.
  The **85 exported Core C symbols match the unchanged baseline**. The draft
  executable's loader graph contains system/C++/sanitizer libraries only: no SRT,
  Metadata, existing Telemetry Core/HTTP, or alternate provider.
- Experimental objects/header are absent from install/package exports, including
  a build with tests enabled. Tests-OFF/default-draft-OFF configuration has no
  experimental compilation commands. No new workflow/job/trigger or media-framework
  integration tests are introduced.

`linux-provenance.json` records actual instrumented artifact hashes and verifies
ASan symbols. `tested-inputs.json` binds the draft inputs and verifies all existing
Core/public/HTTP/schema files against immutable main341adc0. `SHA256SUMS` covers
this evidence folder. The raw installed-consumer log is gzip-compressed to retain
the build tool's trailing whitespace. The two platform golden JSONL outputs are
byte-identical.
The synthetic body bytes test opaque storage only; they are not a valid native
producer frame or authenticated context-binding/interop qualification.

## Reproduction

Normal builds/tests are in the experimental README. `linux-run.sh` reproduces the
local container layout and intentionally returns the failing whole-suite status;
its isolated draft checks run independently. It requires a Linux image with C/C++20,
CMake, Git, Python3, and a `/pip` directory exposing a pure-Python pip installation;
it is not an empty-image provisioning script. Mount this checkout read-only at
`/telemetry-source`, writable build space at`/tmp`, and this folder's scripts plus
writable output at`/evidence`. Before starting, produce the exact baseline archive:

```sh
mkdir -p build-base-source
git archive 341adc0da0788135af781e876d068914b00ab751 | tar -x -C build-base-source
```

The script installs the CI-pinned Python schema dependencies to`/tmp/python`, sets
ASan leak detection and UBSan halt-on-error, and uses the original HTTP assertions.
It neither weakens their1s response threshold nor removes the HTTP test. The
initial local image was`robotweax-broadcast-ci:issue72`; no image provider/SRT
contents are used by the remote draft executable.

## Limits and next decisions

This is a **draft for joint review**, not the complete Broadcast Link/Telemetry
integration. Review the additive clock/context/history model and naming before
supporting a public package. Then add qualified typed numeric projection and
shared timestamp-independent delta reuse, original clear/sampling provenance,
actual current BL host admission/retirement, bounded multi-row/sink behavior and
real GCM/impairment tests. UI/history/health/Doctor acceptance is a later gate.
The existing Linux HTTP sanitizer failure is an independent qualification finding
that must remain visible; there is no claim of full native/runtime approval here.
