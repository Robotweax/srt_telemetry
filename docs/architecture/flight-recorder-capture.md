# Flight Recorder instrumented capture host

The existing `public_srt_incident_peer` is the initial integration host. It owns
its caller/listener sockets, links the chosen provider, and injects that library's
`srt_bistats` into the existing public SRT adapter. Sampling does not clear the
provider's interval counters. This is not an external process observer. Media
payloads are synthetic test traffic and are never written into telemetry files.

The peer checks the connected socket's Live/Message profile and reports the
public API version. The local qualification runner records the exact library
build revision supplied by the build procedure. A version string alone is not
provider qualification. The demonstrated lane is Robotweax SRT 0.2.3 commit
`ac9c14d7815bfd6f76f8b60308c5465095625eb7`, public API 1.5.7, Release shared,
OpenSSL, AEAD extension off, unencrypted IPv4 loopback caller/listener,
1316-byte messages. This does not qualify GoSRT or another provider/build.

## Compatible host command additions

The original `SAMPLE`, `TRANSFER`, `START_TRANSFER`, `WAIT_TRANSFER`, and `DONE`
protocol remains usable. `--jsonl-output` and its provenance options enable
recording. The JSONL exporter and telemetry schema are unchanged.

- `ARM pre_records post_ms`: arm once per connection epoch. The pre-ring keeps
  the newest `pre_records`; a separate capture ring reserves the configured
  `--recorder-capacity`. Require `0 < pre < capacity <= 4096` and
  `1 <= post_ms <= 60000`. Both arrays are allocated at arm time.
- `SAMPLE_NOW index`: obtain wall and monotonic timestamps within the host,
  call the existing adapter, push to the ring, and return the usual sample.
- `TRIGGER_NOW`: use the host's monotonic clock as the first trigger time.
  Copy the bounded pre-ring into the reserved capture ring. The deadline is
  first trigger plus post duration. Later triggers only increase a saturating
  counter; they never extend or replace the first deadline.
- `TRIGGER monotonic_ns`: deterministic variant for a caller that also supplies
  `SAMPLE` timestamps in that same clock domain. Do not mix with another clock.
- `DONE`: freeze and export. If the post deadline was not reached, the state
  explicitly reports `connection-ended`; with no trigger, `untriggered-end`.

A sample later than the deadline freezes capture without being appended. An
exact-deadline sample is appended and freezes it. Capacity exhaustion freezes
without overwriting pre-trigger evidence and reports `capacity`, so shortened
post coverage is visible. Non-increasing monotonic timestamps freeze with
`monotonic-boundary`. A fresh connection requires a new peer/session/window;
there is no delta across that epoch. The run includes this actual teardown and
recreation, and verifies the SDK's initial-baseline flags in both endpoints.

The ring is bounded even if sampling stops. Deadline detection and export are
coordinator-driven, not an autonomous watchdog: no incoming sample means no
additional record and no immediate file flush. A crashed coordinator loses the
in-memory capture. The coordinator's unbounded duration is not advertised as a
persistent spool. The demo owns a finite schedule and explicit completion.

Pre coverage is a record count, **not a guaranteed duration**. The nominal 20 Hz
schedule is delayed by blocking transfers and Python/OS scheduling. Captures
retain actual timestamp brackets, pre overwrite counts, and measured retained
coverage; samples must not be interpreted as evenly spaced. The exported
sample index restarts at zero, while the native monotonic timestamp allows
matching to the coordinator's original sample brackets.

## Clocks and gaps

The native host's `steady_clock` and Python's `monotonic_ns` may have different
origins even on the same macOS machine. Each command stores begin/end monotonic
and wall timestamps plus both native sample timestamps. Each native-to-
coordinator offset is an interval `[begin - native, end - native]`, not an
assumed equality. Endpoint clock domain IDs are separate. Wall timestamps are
checked against their command bracket; a detected discontinuity fails the run.
The maximum command bracket is an empirical within-run ordering uncertainty;
it is not an absolute UTC accuracy claim. Trigger commands also retain brackets.
The first retained delta can describe an interval whose earlier sample was
already overwritten; no omitted prefix sample is reconstructed.

## Resource and persistence behavior

The live demo chooses 8 pre records, 64 final records and a 1000 ms post window.
It allocates 72 records per endpoint while armed. The recorder core's record
size is emitted by the peer and the trigger state reports allocated record
bytes. Export additionally allocates the SDK's fixed snapshot and serialized
JSONL; export is outside the sampling hot path. All recording storage is bounded
by the configured record limit; strings and metadata still have bounded command
line input from the host environment. The general 4096-record ceiling requires
a separate memory budget from this 64-record demo.

Export refuses an existing destination or `.partial` file, writes a same-folder
partial with exclusive C11 creation, checks stream completion, and publishes
with a same-directory hard link that fails if the destination already exists.
Write/disk-full/publication errors return failure and clean up the owned partial
(best effort; a cleanup error is reported). Existing destinations and partials
are preserved. There is no automatic retry, disk spool, crash durability/fsync
guarantee, or overwriting of an existing capture. The local target filesystem
must support hard links; unsupported publication fails explicitly. The
product packaging layer independently caps bytes and validates integrity.

The qualification runner also measures sampling-disabled, sample-only, and
sample-and-recorder modes with identical synthetic workload and rotated order.
Its report states wall time, child CPU (including startup and `ps` probes),
command round-trip sampling time (including IPC/JSON), and twice-sampled RSS.
These are actual local observations, not isolated SDK performance or true peak
RSS. Its provisional budget is 64 MiB combined peer RSS and at most 1 ms extra
median pair command latency for recording on this machine. Variance and small
sample counts prevent a general performance guarantee.

## Reproduce

Build the provider and telemetry into separate directories, point the adapter
at that exact provider header and shared library, then run:

```sh
python3 qualification/run_flight_recorder_capture.py \
  --peer /absolute/build-telemetry/robotweax_srt_telemetry_public_srt_incident_peer \
  --provider-library /absolute/build-srt/libsrt.dylib \
  --provider robotweax-srt --provider-version 0.2.3 \
  --provider-revision ac9c14d7815bfd6f76f8b60308c5465095625eb7 \
  --build-profile release-shared-openssl-aead-off-live-message-ipv4 \
  --output /absolute/new-output-directory
```

Local UDP socket permission is required. The output includes new loss recovery
and receiver pause epochs, immutable endpoint JSONL, command logs, per-epoch
`capture-manifest.json`, `capture-run.json`, and `overhead/overhead.json`.
The loss proxy records dropped original packets and subsequent retransmission;
its metadata is controlled harness evidence, not telemetry classifier input.
The receiver pause must produce observable receive-buffer pressure. Other
acceptance classes reuse separately identified historical qualified recordings
or synthetic negative fixtures at the product level.

A separate 11000-message receiver-pause stress attempt reached a full receive
buffer and failed to drain before the native receive timeout on this build.
It is retained as unsuccessful experiment evidence; it does not extend the
1000-message demonstrator's support scope and is not a successful incident
capture. Existing qualified receiver-pressure fixtures remain the diagnostic
acceptance evidence. `--pause-messages` permits bounded follow-up experiments.
