# PR65 review follow-up — neutral draft2, 2026-10-28

Addresses the [design review of98cda94](https://github.com/Robotweax/srt_telemetry/pull/65#pullrequestreview-5685135114).
No merge/release decision or supported SDK adoption is implied. Original draft1
results remain historical at98cda94; this folder binds the revised draft2 inputs.

## Decisions and executed checks

| Review point | Resolution and evidence |
| --- | --- |
| BL vocabulary in Telemetry | Neutral producer/source/role/time/receipt context plus opaque namespaced origin context/payload. No BL generation, Metadata, native-kind, RWBL or SDK-key definition remains in the library/header/schema. BL owns its closed context contract. Foreign origin namespace/version values roundtrip opaquely without typed qualification. |
| Shared mutable read clock | Removed. Const reads validate only their own time against latest receipt.5000 then4999 succeeds; independent read-only threads do not mutate a recorder clock. Mutations must still be externally excluded. |
| Late duplicate/conflict wording | Precisely latest-record duplicate/conflict; any lower sequence is stale, even with an identical/different retained historical body. Explicit tests preserve receipt/history and ordering. |
| Generic validation message | `ValidationError` derives from invalid_argument and exposes a named field path without echoing values. Constructor, origin, time and wall-time checks name their field. |
| Consumer-target Werror | Removed from the separate draft target; warning diagnostics retained. No new workflow/job/trigger. |
| HTTP finding and unmerged handoff | Central HANDOFF is byte-identical to main341adc0. Raw ARM64 failures remain historical; the separate issue-ready note includes the reviewer's passing x86-64 result and does not claim a universal failure or root cause. |

Executed at the final local inputs:

- **62/62 macOS ARM64 CTests pass**, including existing C API/recording/HTTP and
  required OTLP-Protobuf tests. The old protected41 Core/public/HTTP/schema files
  remain byte-identical to main341adc0.
- **87 C++ assertions and35 schema negatives pass** on macOS and Linux ARM64 with
  ASan/UBSan/leak detection. Exact u64/time/null-versus-epoch0 cases and70 u64
  boundaries remain covered. No shared mutable state is written by the const
  reader methods; a two-reader test runs with no writer/retirement/destruction.
  This is not TSan evidence or concurrent-write support.
- **8 BL owner-contract tests and the explicit opaque roundtrip pass** on both
  platforms. BL checks its exact version/SDK key, full row/binding identities,
  u63 epoch, required native-kind/role and first Single/Live sender scope.
  The neutral recorder retains the BL context and original frozen RWBL frame
  bytes exactly, along with supplied independent producer/receipt/export clocks.
- The final platform golden JSONL files are byte-identical. Actual Linux probe
  and static library ASan symbols/hashes and loader are recorded. No SRT,
  Metadata or existing Core/HTTP library is linked by the neutral probe.
- The revised BL focused CTest set passed5/5 on macOS (mapping contract, origin
  contract, existing statistics-state, deferred handoff and native export vectors).

## Limits

This is representation/storage and explicit byte transport between test tools,
not authenticated runtime interoperation. The BL helper uses frozen exported
JSON vectors; production must freshly obtain an authorized native Snapshot and
bind current host identity/leases/epoch at ingestion and actual handoff. Stored
bytes and as-of freshness are never authorization. Unknown opaque origin versions
can be recorded by Telemetry but cannot be decoded/qualified by BL unless its
own exact owner contract accepts them. No numeric projection/delta reuse,
provider/sampling qualification, live GCM/impairment, sink or GUI/health gate passes
by implication. Existing public ABI1/recording1alpha1–4 remain unchanged.

The whole Linux/HTTP suite was not reclassified or rerun to turn the historical
ARM64 failure green: relevant Core/HTTP source did not change. The historical
60/61 sweep remains failed; the maintainer separately reports passing x86-64
baseline runs. See [issue-ready follow-up](HTTP_ARM64_FOLLOWUP.md). No HTTP
assertion/deadline is weakened and no issue/review reply was posted automatically.

`tested-inputs.json` binds both repositories' relevant local inputs; `SHA256SUMS`
binds this folder. `linux.sh` reproduces the focused run with the documented
existing local `/tmp/remote-build`, pinned Python dependencies in`/tmp/python`,
read-only checkouts at`/telemetry-source` and`/src/project`, and writable`/review`.
It requires those prior builds/toolchain; it is not empty-container provisioning.
The ordinary [test instructions](../../../experimental/remote_input/README.md)
need no SRT provider. The BL cross-project audit is explicit and unattempted when
no neutral probe is supplied.
