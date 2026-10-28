# Uninstalled remote input draft

Read the [proposal](../../docs/proposals/remote-observation-input.md) before use.
The code is an experimental owned observation/context recorder, **not a supported
SDK API, wire decoder, remote authorization mechanism or Telemetry numeric/UI
adapter**. Existing ABI1/recording1alpha1–4 are not changed.

When the normal Telemetry tests are enabled, two tests exercise this separate
static library (it is not linked into Core/HTTP). Otherwise the library is absent
unless explicitly requested:

```sh
cmake -S . -B build-remote-draft \
  -DROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS=OFF \
  -DROBOTWEAX_SRT_TELEMETRY_BUILD_HTTP_SERVER=OFF \
  -DROBOTWEAX_SRT_TELEMETRY_BUILD_REMOTE_INPUT_DRAFT=ON
cmake --build build-remote-draft --target robotweax_srt_telemetry_remote_draft
```

The header lives in `experimental/remote_input/include`, namespace
`robotweax::srt::telemetry::remote_draft`. There is no install/export entry or C ABI.
No schema name or type name is reserved by this review draft.

For tests, use `ROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS=ON` and provide Python3 with
the existing `jsonschema` contract-test dependency. Run:

```sh
cmake --build build-remote --target robotweax_srt_telemetry_remote_draft_tests
ctest --test-dir build-remote -R remote_draft --output-on-failure
build-remote/experimental/remote_input/robotweax_srt_telemetry_remote_draft_tests --jsonl
```

Without Python the schema test is unconfigured; without jsonschema it is explicitly
skipped. Either case leaves schema qualification unattempted, never passed.

The Python test validates actual C++ output against the new closed schema, checks
complete contexts/u64 precision/escaping/unknown times, rejects27 negative inputs,
and verifies that the existing recording schema rejects these draft lines.
The native fixture bytes are deliberately opaque storage test data, not a claimed
valid native Type5 producer frame or authenticated interoperability evidence.
