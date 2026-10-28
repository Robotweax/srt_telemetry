# Uninstalled neutral remote input draft

Read the [proposal](../../docs/proposals/remote-observation-input.md). Draft2 owns
neutral identity/time/receipt/freshness/history only. Origin-specific context and
per-observation bytes are opaque, namespaced and owned by the producer. No BL
layout, SRT wire codec, source-authority check or numeric/UI adapter is defined here.

With normal Telemetry tests enabled, two tests exercise this separate static
library; Core/HTTP do not link it. Otherwise it is absent unless requested:

```sh
cmake -S . -B build-remote-draft \
  -DROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS=OFF \
  -DROBOTWEAX_SRT_TELEMETRY_BUILD_HTTP_SERVER=OFF \
  -DROBOTWEAX_SRT_TELEMETRY_BUILD_REMOTE_INPUT_DRAFT=ON
cmake --build build-remote-draft --target robotweax_srt_telemetry_remote_draft
```

Header: `experimental/remote_input/include/remote_input_draft.hpp`, namespace
`robotweax::srt::telemetry::remote_draft`. It is uninstalled, has no C ABI/package
export, and reserves no release/recording version. Existing ABI1/1alpha1–4 stay intact.

With tests enabled and Python3/jsonschema available:

```sh
cmake --build build-remote --target robotweax_srt_telemetry_remote_draft_tests
ctest --test-dir build-remote -R remote_draft --output-on-failure
build-remote/experimental/remote_input/robotweax_srt_telemetry_remote_draft_tests --jsonl
```

The schema test is unconfigured without Python; explicitly skipped without
jsonschema. Either is unattempted qualification. With dependencies installed it
checks closed output, exact u64s, unknown wall time/epoch0, foreign opaque origins
and negative vectors. C++ tests cover latest-record duplicate/conflict versus
stale history, field diagnostics and two independent read-only threads. All writes
and retirement still require external exclusion; no concurrent-write support or
TSan qualification is claimed. Reads validate only their own clock against receipt,
not a shared high-water mark. Their as-of freshness is not authorization to publish.

For an explicit producer-owned byte roundtrip, the test executable accepts:

```sh
PROBE --opaque-origin NAMESPACE ORIGIN_SCHEMA CONTEXT_FILE PAYLOAD_FILE \
  SEQUENCE PRODUCER_NS INTERVAL_BEGIN_NS RECEIPT_NS EXPORT_NS
```

It treats files as opaque bytes and emits a context header plus one observation;
the optional integer arguments preserve separate source/observer times. This test
driver does not decode/authenticate bytes or qualify a source. A producer such as
BL supplies and validates its own closed origin schema before invoking the recorder.
