# Neutral remote observation input — experimental draft2

Status: joint-review draft; uninstalled C++ code, not a released API or recording
version. Responds to the [review of PR65 at98cda94](https://github.com/Robotweax/srt_telemetry/pull/65#pullrequestreview-5685135114)
and [Broadcast Link PR75](https://github.com/Robotweax/srt_broadcast_link/pull/75).
Base remains Telemetry main341adc0. Draft2 replaces the unreleased, BL-specific
`remote-input.draft1`; there is no alias/import or reinterpretation of that format.
ABI1, existing public headers, Core/HTTP and recording1alpha1–4 are unchanged.

## Ownership boundary

Telemetry owns neutral producer/source identity, endpoint role (explicit sender,
receiver or unknown), producer process/clock provenance, sequence, monotonic time,
optional wall time, original first receipt, freshness-as-of-read-time and irreversible
retirement. Labels are host-supplied metadata, not source authentication.

The producer owns an **opaque namespaced origin block**:

- `Context::origin.namespace_id` and `schema`: bounded opaque identifiers;
- `Context::origin.context`: immutable0..4096 bytes for that source/segment;
- `Observation::origin_payload`: original1..16384 bytes per observation.

Telemetry copies and exports these bytes without parsing their meaning. It does
not define BL route sessions, media IDs, generation fields, native kinds, sampling,
counter-clear policy, Metadata epochs, RWBL or an SDK type registry. Origin schema
changes do not require a Telemetry envelope change. An unknown origin namespace
or version can be retained opaquely; that is **not decoded/qualified acceptance**
of it for numeric projection, application authority or a native effect.

BL owns its first Single/Live sender scope, complete row/pair identity, native
kind, profile/provider/sampling/clear provenance and original Type5/schema1.0
bytes. Its own versioned origin schema and adapter must validate those facts.
Other producers can own other origin schemas without changing this Telemetry API.
No production BL adapter or supported SDK package adopts this draft.

## Typed neutral input

A recorder contains exactly one immutable producer/source/origin/observer segment.
The host supplies a unique segment ID, producer ID and opaque source ID, explicit
endpoint role (default unknown), optional producer instance/clock domain and
independent optional Cloud observer node/instance. Existing1alpha4 observer label
rules are preserved. A required observer monotonic-clock domain is separate from
the producer's domain; the two numeric clocks are never compared.

Unknown producer wall time stays null. A present wall time contains an exact
u64 Unix-ns value (including a real epoch0), synchronization quality, provenance
and a maximum-error bound only for the synchronized state. Unknown clocks are
never filled from receipt/export time or a fabricated connected-at time.
Provider/qualification/transport facts belong to the opaque origin or a separately
reviewed future numeric-projection contract. No fields are automatically qualified.

The caller must validate origin-specific content and current authority before
storage and actual publication. These generic value types authenticate no caller
and grant no source/lease/pair/native-write authority. Stored bytes or a retained
historical snapshot cannot renew that authority. Future BL code must freshly
obtain a currently admitted native Snapshot and retain its host lifecycle fences.

## Bounded history and ordering

Capacity is1..256 (default8); each slot owns an observation. A full recorder replaces
its oldest record and increments a **local history overwrite** count. Origin and
payload bounds are checked before record publication; source data is copied
before replacing current state. Hosts also bound the number of recorders.

Ordering is against the **latest accepted record**. Equal latest sequence with
identical producer data is duplicate; with different producer data it is conflict.
A lower sequence always returns stale_sequence, whether its bytes match a retained
older history entry or not. Historical lookup is not replay authority or a duplicate
cache. None of these outcomes updates first receipt or advances the latest sequence.
A backward producer clock is rejected; equal producer time/higher sequence is
retained with an equality marker, without a rate. A sequence discontinuity is a
boolean; it is never interpreted as missing samples or network packet loss.

Retirement is irreversible. A new source/origin/observer/provider/clock lifetime
needs a new recorder/segment; there is no reset/resume or source-grant method.
Owned historical copies and explicit history export remain possible after retirement,
with retirement visible. The origin owner decides which domain changes require a
new segment and must never reconstruct a retired context to erase replay history.

## Pure reads and synchronization

`latest(now)` is an **as-of query**, returning available/no_sample/stale/retired;
it validates only a positive `now`. A reader whose clock predates a receipt (it was
read before a later push was accepted) sees that record with age 0, so an ordinary
interleaving of reader and writer is never an error. Maximum age is positive,
at most60s (default1s), inclusive at the exact boundary. A stale result has no
current sample. `export_history_jsonl(now)` uses the same supplied clock domain.

Both are pure const reads: **no shared mutable export-clock high-water mark**.
`latest(5000)`, `latest(4999)` and `export_history_jsonl(4999)` can succeed for
independent readers; an earlier timestamp from one reader cannot poison another.
A caller requiring monotonically ordered reads may keep its own per-reader guard.
Freshness-as-of a supplied timestamp is not current publication authorization;
the host obtains a trusted clock and rechecks its current origin-specific grants
at actual handoff. The recorder does not pretend to authenticate a supplied clock.

Concurrent const reads are safe **only while no writer, retirement or destruction
runs**. The host externally serializes every mutation against readers/writers.
There is no internal worker, mutex, callback, socket getter, SRT/Metadata/BL library
dependency, or synchronization with live protocol owners. A two-reader regression
covers the read-only case; it is not TSan qualification or concurrent-write support.

## Diagnostics and separate closed export

Validation throws `ValidationError`, derived from `std::invalid_argument`, with
`.field()` and a message naming a field such as `origin.namespace_id`,
`producer_wall_time.maximum_error_nano` or `receipt.monotonic_nano`. Values/credentials
are not echoed. These are draft C++ diagnostics, not a reserved future C error ABI.
The draft keeps warning diagnostics but does not impose`-Werror` on consumer test
builds. It is still built separately for tests or explicit default-OFF experimentation,
never linked into Core/HTTP or installed/exported as a public SDK component.

`remote-input.draft2` is a closed, separately versioned history JSONL envelope.
A header contains generic producer/observer context and an opaque origin block.
Every observation is explicitly historical, retains original first receipt,
producer/export clocks, freshness and lifecycle markers, plus opaque origin bytes.
All u64s are canonical decimal strings. Missing wall time is null; actual epoch0
is a distinct present wall-time object. Unknown envelope fields/versions fail;
unknown **origin** identifiers remain opaque. Current1alpha1–4 readers reject the
draft. No UI/history/health/Doctor or numeric projection import path is enabled.

Origins may identify infrastructure or contain private source context. Hosts own
access, retention and redaction; raw origin labels are not public metric dimensions.
Do not put credentials into identities or labels.

## Evidence and remaining work

[Review follow-up evidence](../../qualification/results/2026-10-28-remote-input-review/README.md)
records final tests, neutral/foreign-origin roundtrips and the ownership split.
The earlier draft1 evidence is not kept in this branch (it qualified code this
draft replaced); it remains at [98cda94](https://github.com/Robotweax/srt_telemetry/tree/98cda94210696424bde9637abdfadc6a581dcced/qualification/results/2026-10-28-remote-input-draft). The ARM64 HTTP diagnostic is separated
into an issue-ready note there; the maintainer reports passing x86-64 baseline runs.
No broad sanitizer failure or universal root cause is claimed.

Before a supported integration: agree on the neutral/opaque split and interface,
then add qualified typed numeric projection and shared timestamp-independent delta
reuse. The projection boundary keeps the split: the origin owner decodes its own
payload and hands Telemetry typed, qualified values (with their units, sampling and
clear semantics) beside the original bytes. Telemetry never parses an origin
payload such as RWBL; otherwise the producer coupling this draft removed returns. The origin owner must qualify sampling/clear/reset semantics and actual
current host/owner/sink lifecycle under GCM and impairment. Only then qualify
recorder import, UI/history/health and Doctor consumers. No automatic adaptation,
Control/SRT writes, app activation, Server/EdgeLink/FFT integration or dependency
pin update is included.
