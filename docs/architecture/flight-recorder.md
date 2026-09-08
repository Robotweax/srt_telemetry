# Flight Recorder: local first milestone

Flight Recorder preserves an incident window from instrumented SRT applications
and presents explainable evidence to support and operations teams. Its product
hypothesis is faster or more accurate fault localization than the same ordinary
statistics. Technical tests do not establish that human benefit or demand.

The implementation lives in this repository because it directly consumes the
existing recorder, public adapter, JSONL contract, analyzer, and operator study.
It is an experimental local product tool, not a production agent or SDK release.
No backend, new remote repository, license change, or second SRT library is needed.

## Run the local case workflow

Requires Python 3.9+; no Python packages or network services are needed. From the
repository root, choose a **new** output directory:

```sh
python3 tools/flight_recorder_demo.py --output /tmp/flight-recorder-demo
python3 tools/flight_recorder.py inspect /tmp/flight-recorder-demo/retained-loss-recovery.frcase
python3 tools/flight_recorder.py view /tmp/flight-recorder-demo/retained-loss-recovery.frcase --output /tmp/loss-review.html
```

Open a generated `.html` locally. The case view shows endpoint/epoch lanes,
trigger markers, values and unavailable fields, time boundaries, observations
supporting a finding, contradictions, missing evidence, and a suggested next
measurement. Its statistics toggle uses the same observations but is **not** a
blinded study: the whole report remains in the document. The separate operator
harness prepares genuinely separated, counterbalanced study views.

The demo generates three retained real captures (loss/recovery, receiver
pressure, exhausted recovery/playout margin), plus two **synthetic derivatives**:
one removes receiver loss availability; one adds a receiver drop contradicting
full unique delivery. Both must yield insufficient evidence. The source manifest
and original recording digests remain attached. Random case-scoped pseudonyms
and IDs intentionally differ each time; metric values and outcomes reproduce.

## Instrument an actual socket host

The native demo extends `adapters/public_srt/tests/public_srt_incident_peer.cpp`.
It creates caller/sender and listener/receiver sockets and injects its own
`srt_bistats` into the existing public adapter. `clear=0, instantaneous=1`
preserves other interval consumers. It never attaches to arbitrary external
processes. The host selects and links exactly one provider library; compile the
adapter against that provider's public headers. Stop sampling and destroy the
session before closing the socket. Each new connection needs a new telemetry
session and trigger window; do not reuse counter baselines.

Build against an installed provider with the existing CMake interface:

```sh
cmake -S . -B ../build-telemetry -DCMAKE_BUILD_TYPE=Release \
  -DROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS=ON \
  -DROBOTWEAX_SRT_TELEMETRY_BUILD_PUBLIC_SRT_ADAPTER=ON \
  -DROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR=/absolute/provider/include \
  -DROBOTWEAX_SRT_TELEMETRY_SRT_LIBRARY=/absolute/provider/lib/libsrt.dylib \
  -DROBOTWEAX_SRT_TELEMETRY_RUNTIME_TEST_PROVIDER=robotweax-srt
cmake --build ../build-telemetry --parallel
ctest --test-dir ../build-telemetry --output-on-failure
python3 qualification/run_flight_recorder_capture.py --help
```

The retained current-provider qualification uses Robotweax
`ac9c14d7815bfd6f76f8b60308c5465095625eb7`, 0.2.3, shared Release/OpenSSL,
unencrypted IPv4 loopback Live/Message, 1,316-byte generated messages,
120 ms observed TSBPD delay, no FEC or groups. Adapter mapping and
capture tests only justify this narrow development profile. The old retained
cross-provider incidents keep their own exact Robotweax and Haivision revisions.
They do not migrate incident qualification to the current provider. GoSRT needs
its own adapter; no GoSRT support follows from this C API.

Run fresh captures using the flags shown by the runner, including exact provider
revision, library and peer executable. Then combine them with the demo:

```sh
python3 tools/flight_recorder_demo.py --output /tmp/flight-recorder-with-live --capture-root /absolute/new-capture-run
python3 tools/flight_recorder.py pack /absolute/new-capture-run/loss-recovery/sender.jsonl \
  /absolute/new-capture-run/loss-recovery/receiver.jsonl \
  --evidence-class new-experiment \
  --capture-metadata /absolute/new-capture-run/loss-recovery/capture-manifest.json \
  --output /tmp/live-loss.frcase
```

The instrumented command host accepts additive `ARM <pre-records> <post-ms>`,
`SAMPLE_NOW <index>`, `TRIGGER_NOW`, and `DONE`; the previous logical `SAMPLE`,
transfer and export protocol remains supported. A single owner drives sampling,
triggering, and lifecycle. Eight pre-trigger records and 1,000 ms post-trigger
are used in the demo. The first trigger copies the bounded prehistory into a
fixed capture ring; further triggers increment a counter without extending the
deadline. Capacity freezes the case instead of overwriting incident evidence.
A regressing clock freezes capture; early connection end explicitly truncates
it. Without new samples, the owner must finish on shutdown; no background timer
or durable daemon is implied. Re-arm by creating a new window/epoch.

Sampling is command-driven with nominal 50 ms pauses; transfer commands can
stretch actual intervals. Actual timestamps/intervals, retained coverage, and
command bounds are recorded. This is not a guarantee of continuous 20 Hz polling
under arbitrary host load. Counter deltas integrate over the recorded interval.

## Case contract and time policy

A `.frcase` is a ZIP containing only `manifest.json` and one to eight telemetry
JSONL recordings. Manifest [schema 1.0](../../schema/flight-recorder-case.schema.json)
wraps the unchanged [telemetry JSONL 1alpha1](../../schema/telemetry-jsonl-line.schema.json).
It includes case/session/endpoint IDs, connection epochs, provider source
revision, exact build digests where available, observed configuration, availability
and quality flags, clock domains and uncertainty, triggers, lifecycle events,
capture gaps/window coverage, budgets and per-file SHA-256/length inventory.

The importer verifies limits, structure, hashes, record counts, stream identity,
availability masks and references before generating a view. It rejects unknown
versions/members/fields, duplicate paths/keys, traversal, symlinks, nonfinite
numbers and over-budget inputs. It never extracts an archive into a user folder,
loads scripts from a case, or trusts a stored diagnosis. The existing incident
analyzer recomputes hypotheses after conservative product eligibility checks.
Checksums prove content integrity, **not** author authenticity; reauthored valid
manifests need a trusted source or future signing policy.

- Independent endpoint monotonic clocks stay in separate domains. Wall clocks
  are retained but do not establish synchronization.
- Historical logical pairing requires its source manifest and exact recording
  digests. These timestamps are orchestration coordinates, not elapsed physical
  time or throughput measurements.
- Fresh pairing requires each sample's native stamp to be bound by measured
  coordinator dispatch/response times. Native C++ and Python monotonic origins
  actually differ on the tested host. Their per-observation offset intervals,
  not raw clock equality, map the sample into a common window. No event order
  within overlapping uncertainty is inferred; interval counters cannot reveal
  within-window ordering either.
- Missing peers, unequal windows, inconsistent pairing, counter resets or
  regressions, provider changes, connection epochs and explicit gap/boundary
  events refuse a global diagnosis. They remain visible for manual inspection.
  Ordinary overwritten prehistory is disclosed separately and does not invalidate
  a complete retained warm baseline. A wall/monotonic increment disagreement
  above 100 ms is an explicit detection boundary, not a synchronization promise.
- The analyzer's existing five bounded hypotheses and thresholds are reused.
  No packet-level causality, exact network hop, application consumption time,
  sufficient replacement latency or visible image freeze follows from transport
  counters. The displayed TSBPD delay is configuration, not measured spare margin.

## Resource and failure behavior

Provisional development budgets, not cross-platform performance promises:

| Boundary | Budget / behavior |
|---|---|
| Native trigger window | `0 < pre < capacity <= 4096`, post 1–60,000 ms; allocation is pre + capacity records; no push-time growth |
| Demonstrated native case | 8 + 64 records, 33,984 record-storage bytes per endpoint with this 472-byte ABI record |
| Product import/export | 16 MiB total, 8 MiB per recording, 128 KiB manifest, 8 recordings, 2,048 samples per recording, 256 trigger/event/gap records each |
| Sampling overload | Owner controls sampling; source intervals and failures remain explicit; no unlimited asynchronous queue |
| Export | One control-path snapshot; SDK locks only while copying, then serializes outside the lock; not a real-time operation |
| Product storage error | Exclusive complete-file publication; no overwrite, no final case on ENOSPC/EIO; temporary file cleaned; report failure and retry to a new destination |
| Import/HTML | Hard input/output byte bounds; object memory and runtime also depend on allowed count. Measured examples do not establish a hard process RSS cap at maximum input |
| Case retention | One explicit destination per invocation, no background spool/retry/automatic accumulation. Operator manages completed files and disk quota |

The product publisher flushes/fsyncs the file before no-replace publication.
It does not claim power-loss durability of directory metadata. Native source
export is a separate test-host persistence boundary; see its retained tests and
implementation. No claims for network filesystems or Windows filesystem atomicity
are made. A failed experiment is not accepted as a complete case.

## Support export and extensions

`pack` sanitizes by default. `--private` deliberately retains supplied local
stream identities; create a support copy with:

```sh
python3 tools/flight_recorder.py redact /absolute/private.frcase --output /tmp/support.frcase
```

Each support export regenerates case-scoped pseudonyms consistently across flow,
stream, endpoint, clock, trigger and bracket references, preserving association.
Free-form build descriptions are replaced by a digest; provider source revisions
and strict technical version strings plus exact library/build hashes remain.
Only declared typed configuration/events are accepted; raw capture manifests,
addresses, arbitrary notes, packet captures, credentials and media payloads are
not embedded. Original recording/source hashes preserve lineage without copying
sensitive text. Technical build hashes/timing/counter patterns are not anonymous
against all external knowledge; identity redaction is the bounded claim.

Optional analyzer packet exports remain an extension boundary, with component
license and timestamp-mapping review required. A packet observation does not
prove application delivery. Media observations similarly need their own clocks,
provenance and independent decoder/display evidence. Missing adapters do not
block the telemetry-only case workflow.

Run product regression tests with:

```sh
python3 -m unittest discover -s qualification -p 'test_flight_recorder*.py' -v
```

Executed results, repository baselines, the qualified scope and the pilot plan
are recorded in the accompanying dated Knowledge Base milestone, rather than
claiming production readiness here.
