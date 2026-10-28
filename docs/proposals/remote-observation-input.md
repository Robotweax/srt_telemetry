# Remote observation input — experimental envelope, part 1

Status: **draft for joint Telemetry/Broadcast Link review, not a released API or
recording format**. Implements the first, clock/context/history slice requested
by [Broadcast Link PR75](https://github.com/Robotweax/srt_broadcast_link/pull/75)
at `848bcf5962d83b0bdcdaf6f6745ad17cc920d11d`. Telemetry base is main
`341adc0da0788135af781e876d068914b00ab751` (existing ABI1, recording1alpha1–4).
The proposed namespace/type names are review choices, not reserved versions.

## Why a separate input

BL Type5 supplies producer monotonic time and original sender statistics, but
does not supply producer Unix time, process incarnation, synchronization or
provider/field qualification. The existing raw ABI1 timestamp is a real numeric
Unix value; 0 is epoch0. A Cloud recorder cannot fill that field with unknown,
receipt wall time or a fabricated producer time. Cloud1alpha4 observer labels
identify the writer, independently of the observed media endpoint.

The experimental C++ `remote_draft::Recorder` therefore stores an independent
owned observation and immutable row/Metadata/observer context. It never constructs
an ABI1 raw sample, calls a socket getter, or changes the current recorder,
delta engine, sampler, history, health, UI, OTLP or Prometheus paths. It has no
SRT/Metadata/BL dependencies and no callbacks/threads. Existing public headers,
ABI1, package exports and schema1alpha1–4 remain unchanged.

## Proposed input and meaning

- One immutable context per row and host-generated segment. Includes device,
  route session, binary32-byte media association, resource, source generation,
  full association/component/profile/binding generations and Metadata epoch.
  Scope/slot/member-generation/label fields are explicit. This first draft
  accepts **Single/Live sender, connection row** only; play/receiver/group/member
  inputs fail instead of being normalized to this scope.
- Producer process/clock domain may be null. Cloud observer node/process may be
  null with the same64-character label rules as1alpha4. A separate, required host
  observer-clock label and segment ID describe the local monotonic lifetime.
  Labels are metadata, never authentication. The host must allocate a new unique
  segment on process/row/provider/qualification/clock or Metadata replacement;
  the module does not maintain a cross-instance identity registry.
- Provider revision, build/profile and qualification ID may be null. Their
  presence does not qualify any field. Sampling is unknown/averaged/instantaneous;
  clear policy is unknown/exclusiveNoClear. No numeric projections, since-clear
  deltas or provider options are inferred in this slice.
- Observation carries original positive u64 sequence, producer monotonic ns,
  optional producer interval begin, optional producer wall time and clear flag.
  Each present wall time explicitly has synchronization quality, provenance and
  a maximum-error bound only when synchronized. Actual epoch0 is representable;
  missing time is null. Unknown producer clocks are never filled from the Cloud.
- Original Type5/schema1.0 **RWBL** bytes are copied exactly (1..16384 bytes).
  The historical envelope identifies namespace `rwx-broadcast-01`, type5,
  schema1.0, encoding `rwblHex`. This is opaque storage, not a wire decoder or
  canonicality check. It preserves the complete original native record, including
  integer widths, sentinels and values not yet qualified for projection.

The caller MUST freshly obtain an owned, currently authorized BL Snapshot and
validate/bind the full wire/context before storage and actual publication. These
generic value types cannot authenticate a caller or grant source/lease authority.
Arbitrary remote JSON/bytes or an old Snapshot are not an admission route. No
production BL adapter invokes this draft.

## Bounded storage and lifecycle

Capacity is1..256 records (default8), one fixed vector of optional slots per
context. A full recorder overwrites its oldest history entry and increments a
distinct local count. That count and sequence discontinuities are not network
loss. The source observation is allocated/copied before replacing existing data.
Strings and payloads have explicit size/ASCII bounds; no unbounded row manager or
per-metric queue is introduced. Hosts must also bound the number of recorders.

Ordering is per immutable row/context. Equal sequence/identical producer data is
a duplicate and leaves the first receipt—including optional original Cloud wall
time—unchanged. Same sequence/different producer data conflicts; lower sequence
or backward producer time fails without advancing state. Higher sequence/equal
producer time is retained with an equality marker, never divided by zero.
Global sequence interleaving is a boolean discontinuity, never a missing-sample
or packet-loss count. All64-bit output values are canonical decimal strings.

`latest(now)` returns available/noSample/stale/retired. The maximum age is positive,
at most60s (default1s), inclusive at the exact boundary; stale results have no
current sample. It compares original first receipt to the explicit export clock
and rejects a backward export clock so old data cannot become fresh by backdating.
Producer and Cloud monotonic clocks are never compared with each other. Retirement
is irreversible; a new context requires a new recorder. `read_history` returns an
owned copy; historical export remains possible after retirement and cannot renew
any authority. There is no reset/resume or source-grant method.

## Separate draft history contract

`export_history_jsonl(now)` writes one context header and bounded observation
lines conforming to `experimental/remote_input/remote-input-line.schema.json`.
The name `remote-input.draft1` deliberately does not announce1alpha5 or change
the existing schema. Every record is explicitly historical, carries its original
receipt, export/age, lifecycle state and full-lossless original RWBL payload.
All generation/sequence/time/u64 values are decimal strings. Wall time and missing
metadata are explicit nulls; only member slot and Type5 numeric key are small JSON
numbers. The closed draft rejects unknown members/versions and overflowing u64s.
The current1alpha1–4 schema rejects these lines. Current consumers must not accept
or silently coerce this new format; there is no GUI/Doctor/history import path.

Observer/device/session/resource/payload data may identify infrastructure. Export
is a host-owned historical artifact; operator access, retention and redaction are
required at the eventual application boundary. Do not place credentials in labels
or use these raw labels as public metric dimensions.

## Review gates and remaining slices

This part exercises the **representation/storage** portions of BL A05–A13 only.
The82 C++ assertions and closed-schema/negative checks use synthetic owned byte
fixtures; they do not authenticate native peers or demonstrate full Type5 decode /
context binding. Existing Core/ABI/recording tests remain separate regressions.

Before merging into a supported SDK or integrating BL, decide:

1. Accept/adjust the separate missing-time model, first scope, proposed names and
   historical schema; decide eventual C API/package/export support separately.
2. Add typed qualified numeric projections and reuse a shared, timestamp-agnostic
   delta implementation. Do not route through ABI1 with a fabricated wall time.
   Cover field-local reset, equal-time/no-rate, counter clear policy and the seven
   BL averaged versus Telemetry instantaneous queue fields. No rates/health or
   field qualification are claimed by this first slice.
3. Add production admission/retirement and bounded multi-row/sink ownership on
   actual BL host/owner APIs. Recheck source/leases/grants at delayed handoff;
   qualify process replacement, slow sinks and original receipts through real GCM
   transport. These types do not replace BL authority or its replay tables.
4. Only then qualify recorder import, UI/history/health/Doctor behavior and native
   sender/Cloud/local-receiver pipelines under impairment and restarts. Separate
   producer/observer identities and unknown wall time must survive every consumer.

No automatic adaptation, SRT option write, Control command, app activation,
EdgeLink/Server/FFT integration or dependency pin update is part of this draft.
