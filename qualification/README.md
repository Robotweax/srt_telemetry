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

## Sampling and lifecycle qualification runner

Use the same two provider-linked runtime-test binaries and exact provider
libraries to run the sampling microbenchmark and lifecycle probes sequentially:

```sh
python3 qualification/run_runtime_qualification.py \
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
  --output qualification/results/runtime.json
```

Each provider runs separately. Paired batches compare direct `srt_bistats`
calls with adapter sampling; a synthetic callback separately estimates the SDK
work without provider cost. The runner also verifies the supported
session-destroy-before-socket-close order, a stale-handle failure, and a
time-bounded close/sample race that is explicitly not supported host usage.
Results record binary digests and whether scheduler isolation and power control
were active. An uncontrolled result is candidate evidence, not a release
performance gate.

Retained results:

- [2026-08-28 macOS arm64 sampling and lifecycle](results/2026-08-28-macos-arm64-public-srt-runtime.json)

## Constant-loss capture runner

Build `robotweax_srt_telemetry_public_srt_incident_peer` once against each exact
provider. The runner starts a Haivision receiver, a Robotweax sender, and a
user-space UDP proxy between them:

```sh
python3 qualification/run_constant_loss_capture.py \
  --robotweax-peer /path/to/robotweax/incident-peer \
  --robotweax-library /path/to/robotweax/libsrt \
  --robotweax-version VERSION \
  --robotweax-revision REVISION \
  --robotweax-build-profile PROFILE \
  --haivision-peer /path/to/haivision/incident-peer \
  --haivision-library /path/to/haivision/libsrt \
  --haivision-version VERSION \
  --haivision-revision REVISION \
  --haivision-build-profile PROFILE \
  --telemetry-revision REVISION \
  --output-dir /new/incident-directory
```

The default experiment transfers 200 application messages per interval. During
four active intervals the proxy drops every twentieth first-observed SRT DATA
sequence and forwards retransmissions. It requires complete application
delivery, exact proxy drop counts, positive public sender/receiver loss
evidence, positive retransmission evidence, and zero loss after recovery.

The output directory must not already contain `manifest.json` or `samples.csv`;
retained evidence is never overwritten. Logical one-second sample timestamps
make delta-rate replay deterministic and therefore do not measure wall-clock
throughput.
