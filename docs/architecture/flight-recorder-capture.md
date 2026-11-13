# Flight Recorder instrumented capture host

This document specifies telemetry's reusable native fixture-host integration.
The [Flight Recorder product repository](https://github.com/Robotweax/srt_flight_recorder)
owns the Python capture coordinator, case packages, redaction, view, and product
experiments. It consumes the telemetry contract and analyzer from this
repository; it introduces no separate metrics or diagnosis engine.

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

### Opaque media relay and peer idle timeout

For experiments that must observe the same SRT stream with Telemetry and with
an independent downstream process (for example a separate media decoder), the
peer can relay opaque UDP datagrams instead of its numbered synthetic payload:

- `--media-udp-input HOST:PORT` (caller only): bind a local UDP input; each
  datagram of at most 1316 bytes becomes one live SRT message. Larger
  datagrams are counted as `oversizeDatagrams` and not sent.
- `--media-udp-output HOST:PORT` (listener only): send each received SRT
  message as one UDP datagram to that destination. IPv6 uses `[addr]:port`.
- `--peer-idle-timeout-ms 1000..300000`: set `SRTO_PEERIDLETIMEO` before
  connect/listen, so a session can be kept across an underlay outage longer
  than the 5 s default. `PROFILE` reports the effective value as
  `peerIdleTimeoutMillis` and the relay direction as `mediaRelay`.
- `START_MEDIA` / `STOP_MEDIA`: start and stop the relay worker;
  `MEDIA_STARTED {"direction":…}` and `MEDIA_STOPPED {…}` reply. The input
  socket is bound before `MEDIA_STARTED`, so a source started afterwards
  cannot race the bind. `SAMPLE` and `SAMPLE_NOW` keep working while the relay
  runs; `START_TRANSFER` or `DONE` before `STOP_MEDIA` fail the peer.

The relay never inspects or validates the payload. Gaps are recorded, not
fatal: a receive timeout (`--receive-timeout-ms`) is counted and retried, and
only a lost connection ends the relay early (`connectionBroken: true`).
`MEDIA_STOPPED` reports message and byte counts, send, receive-timeout and
output failures, and the first/last delivery time and longest inter-message gap
on the peer's `steady_clock` (the same caveat on clock origins as below
applies). The listener's `STOP_MEDIA` returns within one receive timeout.
Recording and the JSONL schema are unchanged: the exported file is still
`1alpha1`.

### Connection group mode

For experiments that compare one SRT connection with a connection group over
separate paths, the peer can open or accept a Broadcast or Backup group and run
the same commands over the group handle. It needs a provider with connection
groups. Without one the caller fails with "created no connection group" and
the listener fails at "group admission" before `READY`.

- `--group broadcast|backup` on both peers.
- Caller: `--port PORT` and 2 to 8 times `--member LOCAL_IP,REMOTE_IP[,WEIGHT]`.
  Each member binds its local address and connects to its remote address on
  the common port; the weight (0 to 32767, higher preferred) selects the active
  Backup member. `--remote-address` is not used.
  `--group-min-stable-timeout-ms 60..5000` sets `SRTO_GROUPMINSTABLETIMEO` on a
  Backup group before it connects.
- Listener: `--group-members N` (2 to 8), the number of members to wait for.
  One listener accepts all members, so it must be bound to an address every
  member can reach (for example `::`). Its replies leave by the host's route to
  each caller address; the peer does not pin them to the arrival interface.

After the first member is up (the caller's connect timeout is 15 s), both
peers wait up to a further 15 s until every member is connected (running, or
idle for a Backup standby) and then print, before `CONNECTED`:

```text
GROUP {"type":…,"members":[{"index":…,"socket":…,"weight":…,"localAddress":…,"localPort":…,"peerAddress":…,"peerPort":…},…]}
```

The caller numbers members in `--member` order. The listener numbers them by
the caller's address bytes and then port, both numerically ascending, so the
two sides' indexes need not agree: match members by address and port, not by
index. An IPv4 peer of a dual-stack listener is printed as plain IPv4. `PROFILE` gains `group` and
`groupMembers`; without `--group` it is unchanged. Members are fixed at
connection time. A member that breaks is reported and never replaced.

`START_MEDIA`, `STOP_MEDIA` and the transfer commands use the group handle, so
the group delivers each message once however many members carried it.
`SAMPLE` and `SAMPLE_NOW` answer with one group line and then one
`INCIDENT_SAMPLE` per sampled member, in index order, each carrying `member`:

```text
GROUP_SAMPLE {"role":…,"type":…,"sampleIndex":…,"observedAtUnixNano":…,"sampleMonotonicNano":…,"membersListed":…,"groupCounters":{…}|null,"members":[{"index":…,"socket":…,"state":"pending|idle|running|broken|removed","sampled":…},…]}
```

Each member has its own statistics session on its member socket. `state` is the
provider's member state, `removed` once the group no longer lists the member,
or `unknown` when the member list could not be read (`membersListed: false`).
When a member's statistics can no longer be read it is reported with
`sampled: false`, a `NOTE:` line with the native result goes to stderr, and
it is not sampled again; the peer keeps running. A reader
takes as many `INCIDENT_SAMPLE` lines as the group line has `sampled: true`.
`groupCounters` are the group handle's own cumulative totals read with
`srt_bistats` (`uniqueSentPacketTotal`, `uniqueReceivedPacketTotal`,
`receiverDropPacketTotal`, `retransmittedPacketTotal`). They are raw provider
values printed by this fixture, not Telemetry observations, and are not recorded.

Recording does not change format. With `--jsonl-output PATH`, each member is
written to its own `1alpha1` file `<stem>.member-<index><extension>` beside
`PATH`, with flow ID `<flow-id>-m<index>`; `PATH` itself is not written.
`RECORDED` is printed once per member with `member`; a member that was never
sampled reports `recordCount: 0` and gets no file. This fixture does not use
the adapter's group session or the `1alpha2` group context. `ARM` and the
trigger commands are single-socket features and fail in group mode.

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
at that exact provider header and shared library, then run the capture
coordinator from a checkout of the Flight Recorder product repository:

```sh
python3 /absolute/srt_flight_recorder/qualification/run_flight_recorder_capture.py \
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
It is retained in the product repository as unsuccessful experiment evidence;
it does not extend the 1000-message demonstrator's support scope and is not a
successful incident capture. Existing qualified receiver-pressure fixtures
remain the diagnostic acceptance evidence. `--pause-messages` permits bounded
follow-up experiments.
