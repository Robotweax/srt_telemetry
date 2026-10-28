# Clear-evidence mode review — 2026-10-28

Reviewed PR68 head9a586429c80fa2d1466f698b7a9df27c6312ba2f. The generic caller
could declare exclusive_no_clear while providing actual clear tokens0 then1.
With since-clear counts10 then100, that code emitted delta90/eligible with no
boundary marker. Those two evidence modes contradict each other. The BL adapter
cannot supply tokens and is unaffected, but the generic API needed a guard.

The fix rejects any present token (including0) with field diagnostic
`values.clear_boundary` in exclusive_no_clear mode **before** record or numeric
publication. Origins with tokens must select explicit_boundary. Unknown policy
can retain evidence without derived since-clear deltas. Explicit0/equal/change/
missing-boundary behavior and legacy C ABI behavior remain unchanged. This is
an experimental input-contract tightening, not source authentication or authority.

The new regression fails against the original9a archive (`baseline-new-tests.log`)
and passes the fixed processor. Seven new checks prove first/subsequent rejection,
no record/baseline advancement and valid continuation without a token. Numeric
checks now total75. macOS64/64 pass; the frozen2,048-operation legacy trace and
53-field audit still match unchanged f43c224. Sources/hashes are in inputs.json.
Linux ARM64 ASan/UBSan/leaks:62/62 selected CTests and4 focused draft/trace
tests pass. Both HTTP tests remain unattempted in this Linux run, as before.
Actual instrumented binary/library hashes are recorded in linux-provenance.json. No public SDK
C header/ABI, recording/neutral schema, counter kernel or runtime activation change.

The earlier numeric evidence remains historical at its tested inputs. This is an
author self-review follow-up; external maintainer acceptance is still required.
