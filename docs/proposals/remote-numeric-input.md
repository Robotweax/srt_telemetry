# Qualified remote numeric input — experimental part 2

Status: uninstalled C++ experiment on merged neutral-input PR65, main
`f43c224a90d73dd30ae8da3207bcd1bb2b8c6726`. No public C ABI, numeric recording
format, consumer import, BL dependency or runtime default is introduced.
The [neutral/opaque boundary](remote-observation-input.md) remains binding:
the origin owner decodes its payload and supplies typed values. Telemetry never
parses RWBL, grants source authority or infers native settings from opaque bytes.

## Input and qualification

`experimental/remote_input/include/numeric_input_draft.hpp` defines `RawValues`
(27 observations), `ExtendedValues` (26 observations covering both providers),
`NumericQualification` and `NumericProcessor`. The existing BL candidate map has
52 fields: 27 raw and 25 Robotweax extended. The additional Haivision capacity
field is a distinct meaning, not a Robotweax bandwidth alias.

Values use existing normalized units, field names and availability constants;
there are no synthetic mandatory Unix timestamps, capabilities, connected-at
time or local socket/getter. Original observation bytes and optional wall time
remain in the neutral recorder unchanged. Numeric values are host-supplied
projections; this processor does not prove their correspondence to those bytes.

Every field defaults **unqualified/unavailable**. Nonempty allowlists require an
immutable host evidence ID, known provider and explicit sender/receiver role.
The ID is a declarative reference, not authentication or attestation. The host
owns evidence for exact provider revision/build/profile/native layout, role,
configuration and each field's meaning/availability/reset behavior. It must
validate live authority at publication; accepted bytes or qualified values do
not renew any source, pair, lease or writer permission.

Effective availability is observed availability intersected with the qualified
allowlist. Missing or unqualified values have cleared bits and zero storage;
zero with a set bit remains an observed zero. Unknown availability bits fail.
Observed floating values must be finite and nonnegative, even when unqualified.
The frozen catalog audit checks all 53 normalized declarations/types against the
current public observations without changing those public structs.

The seven sampling-sensitive queue fields (five raw byte/span/space fields and
two extended packet counts) require `GaugeSampling::instantaneous`. Unknown or
averaged sampling cannot qualify them. Current BL bstats sampling is averaged;
its eventual adapter must omit these fields unless the producer is separately
changed and qualified. Averaged native values remain in original bytes.
Robotweax DATA arrival rate and Haivision estimated link capacity are separately
provider-guarded. No source/build is automatically qualified by provider name.

## Delta and clear boundaries

The existing C delta API and this experiment use one private timestamp-independent
kernel in `sdk/src/counter_engine.hpp`. Only provider, producer monotonic ns,
availability and counter values enter it. It neither accepts nor manufactures
wall time. Existing C API validation, raw/ext interleaving, first sample,
provider change, clock boundaries, field-local counter resets and four byte
rates retain their previous behavior. A frozen 2,048-operation trace was captured
from unmodified merged main f43c224 and compared against both the refactor and
an explicitly loaded baseline executable; it is synthetic behavioral evidence,
not provider/runtime qualification.

Rates use accepted producer intervals, never receipt or export cadence. Equal
producer time is retained by the neutral recorder and becomes a no-rate baseline
boundary; backward producer time is refused. Missing availability breaks that
field's delta chain. A field-local decrease marks only that field reset and does
not suppress unrelated valid deltas. Existing extended reset flags remain
separate from raw quality flags.

The two since-clear extended observations retain their qualified original values.
Their derived deltas require one immutable policy:

- `unknown`: no since-clear deltas;
- `exclusive_no_clear`: host evidence that no caller clears counters;
- `explicit_boundary`: both consecutive accepted observations carry the same
  proven clear-boundary token. Zero is a valid token; absent is unknown.

A changed or missing token suppresses since-clear deltas across that interval,
even if counters increased. Lifetime-counter rates continue. After a missing
boundary, two consecutive records with a known matching token are needed.
`since_clear_delta_eligible` reports policy/time continuity; actual per-field
availability/reset bits still decide which delta exists. No boundary is inferred
from `cleared_after_sample=false`, a decreasing counter or a Metadata epoch.

## Lifecycle and output

One processor owns an immutable neutral context and qualification plus a
single-record neutral recorder and fixed-size numeric baseline. A new producer,
source, origin, observer clock, provider or qualification lifetime requires a
new segment; retirement is irreversible. Mutations and reads require the same
external serialization as the neutral recorder. No worker, callbacks or locks
are added.

Only an accepted observation emits `ProcessedNumeric`. Duplicate, conflict,
stale sequence, regressed clock, invalid input and retirement do not advance the
numeric baseline. A latest-record duplicate with different typed values is a
conflict, even if opaque bytes match. Validation/copying occurs before recorder
publication; all subsequent state/result copies are statically checked nonthrowing.

`latest` and history export still return the original neutral draft2 data, not
persisted numeric output. Reads preserve merged-main behavior: positive reader
time before receipt yields age 0; independent readers have no shared mutable
clock. These are storage as-of queries, not current BL publication authority.
Existing public ABI1, recording1alpha1–4 and draft2 JSON schema bytes are unchanged.

## Qualification and remaining gates

[Recorded local evidence](../../qualification/results/2026-10-28-remote-numeric/README.md)
covers numeric guards, producer-time/clear/reset/order cases, exact legacy trace,
existing Core regression, package isolation and merged-baseline BL byte roundtrip.
No new CI workflow is added. Existing test-enabled builds include the separate
experiment; production tests-OFF builds leave it absent unless explicitly opted in.

Next: review this slice; then implement BL's default-OFF native Snapshot adapter
with explicit qualified allowlists and current owner/host publication fences.
Repeat component cases at actual GCM/impairment and bounded sink/lifecycle
boundaries. Numeric history/import, windows/health/UI/Doctor and both local/remote
pipelines need their own contracts and qualification. Nothing here enables
Control/SRT writes, app integration, automatic tuning, EdgeLink, Server or FFT.
