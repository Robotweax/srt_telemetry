# Remote numeric experiment — local evidence, 2026-10-28

Base: merged Telemetry PR65, `f43c224a90d73dd30ae8da3207bcd1bb2b8c6726`.
This is part 2, not qualification of a released SDK or active BL adapter.
`inputs.json` binds the tested implementation and tests, unchanged public header,
neutral implementation/header and recording schemas. Raw logs are gzip-compressed
without editing their content. The superseded draft1 evidence deleted by the
maintainer remains deleted; earlier review evidence is historical.

| Gate | Result and exact scope |
| --- | --- |
| macOS ARM64 Debug | 64/64 configured CTests, including both HTTP tests |
| Linux ARM64 shared Debug ASan+UBSan+leaks | 62/62 selected CTests (`-E http`); both HTTP tests deliberately unattempted this run |
| Focused numeric | 68 checks: empty qualification, all seven queue guards, provider meaning, availability/zero/gaps, invalid fields/NaN/negative, producer time, original-byte validation, duplicate/conflict/stale/backward/equal, per-field reset, retire/new baseline, unknown/exclusive/explicit clear including token0/missing/crossing |
| Legacy C delta API | 2,048 deterministic raw/ext/provider/availability/clock/reset operations byte-identical to an actual unmodified f43c224 Core executable; frozen SHA256 `9a7d6bc93045b8b3a5afc652cded7119a2b163f07afa6668094bd1d1296d85db` also passes with Linux sanitizer Core |
| Normalized catalog | 53 exact field names/types match raw/ext public observations; no mandatory wallclock added |
| ABI/package isolation | 85 exported macOS shared Core symbols identical; public header/schema bytes unchanged; real installed Core and HTTP C consumers run; tests-OFF build graph has no experimental source and installation has no draft/private numeric headers or targets |
| BL-owned origin | Eight BL owner tests plus explicit context/RWBL byte and producer/receipt/export-time roundtrip pass against **unmodified f43c224**, built separately from git archive; BL source baseline `381ba5e4c83ad7616589ef6572676d954104ba5e` |

The shared kernel is covered by existing delta/Core/export/recording/window tests,
not only tests mirroring the new implementation. The trace also alternates raw-only
and ext calls, unavailable returns, counter decreases/wrap, provider changes,
equal/regressed/zero clocks and explicit resets. It preserves existing API behavior,
including unsupported provider values accepted by the legacy entry point; the new
qualified path rejects unknown provider values for nonempty allowlists.

Linux `linux.sh` records compiler/sanitizer configuration and selected gates;
`linux-provenance.json` records actual instrumented binary/library hashes.
`linux-loader.log` resolves the candidate Core from the candidate build alongside
libasan/libubsan. Dependencies were pinned jsonschema4.26.0 and
opentelemetry-proto1.45.0. Container image: `robotweax-broadcast-ci:issue72`.
No new HTTP run was made to chase the previously recorded ARM64 ping-flood deadline
finding. See the existing review evidence/HTTP follow-up for that independent
failure and the maintainer's passing x86-64 baseline. This is not full Linux HTTP,
Windows, live provider, impairment, source-authority, GCM, UI or TSan qualification.

Reproduce configured tests with the commands in
[experimental README](../../../experimental/remote_input/README.md).
For the explicit legacy oracle, extract the exact base into a separate directory:

```sh
mkdir -p build-base-source
git archive f43c224a90d73dd30ae8da3207bcd1bb2b8c6726 | tar -x -C build-base-source
cmake -S build-base-source -B build-base -DBUILD_SHARED_LIBS=ON \
  -DROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS=OFF \
  -DROBOTWEAX_SRT_TELEMETRY_BUILD_HTTP_SERVER=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-base --parallel 4
c++ -std=c++20 experimental/remote_input/test_counter_trace.cpp \
  -I build-base-source/sdk/include -L "$PWD/build-base" \
  -lrobotweax_srt_telemetry -Wl,-rpath,"$PWD/build-base" -o build-base/legacy-trace
python3 experimental/remote_input/test_counter_trace.py \
  build-remote/experimental/remote_input/robotweax_srt_telemetry_counter_trace \
  --baseline build-base/legacy-trace
```

The legacy executable links the original shared library, not the candidate.
An initial local symbol check used a dylib path in the static test build; the
recorded 85-symbol comparison instead uses an actual separately built shared
candidate. No test expectation or production timeout was weakened.

Remaining gates: numeric design review; a default-OFF BL native Snapshot adapter
and qualified allowlist; actual host/GCM/impairment and sink/lifecycle evidence;
versioned numeric recording/import/windows/health/UI/Doctor contract. No production
runtime pin, origin decoder, native/control effect or public ABI promotion occurs.
