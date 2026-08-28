# Provider qualification

Candidate profiles describe evidence available for an exact provider revision
and build profile. An empty `capabilities` array means that the evidence is not
yet broad enough to make a release capability claim.

## Public SRT interoperability runner

Build the telemetry runtime test twice: once against Robotweax SRT and once
against the pinned Haivision SRT. Then run:

```sh
python3 qualification/run_public_srt_interop.py \
  --robotweax-peer /path/to/robotweax/runtime-test \
  --robotweax-library /path/to/robotweax/libsrt \
  --robotweax-version VERSION \
  --robotweax-revision REVISION \
  --robotweax-build-profile PROFILE \
  --haivision-peer /path/to/haivision/runtime-test \
  --haivision-library /path/to/haivision/libsrt \
  --haivision-version VERSION \
  --haivision-revision REVISION \
  --haivision-build-profile PROFILE \
  --telemetry-revision REVISION \
  --output qualification/results/interop.json
```

The runner executes each implementation as caller and listener. Baseline and
transfer barriers use stdio, so only the two deterministic 1,316-byte payloads
cross SRT after both telemetry baselines exist. The result includes exact
revisions plus SHA-256 digests of both peer executables, provider libraries,
and payloads.

Retained results:

- [2026-08-28 macOS arm64 public SRT interop](results/2026-08-28-macos-arm64-public-srt-interop.json)
