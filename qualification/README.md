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

## Loss-shape observability runner

The incident peer also supports a paced continuous transfer while telemetry is
sampled concurrently. Run the bidirectional distributed-versus-concentrated
comparison against the exact provider-linked peers:

```sh
python3 qualification/run_loss_shape_observability.py \
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
  --runtime-qualification qualification/results/runtime.json \
  --output qualification/results/loss-shape.json
```

The runner sends 240 paced application messages and samples both endpoints 26
times at a wall-clock-paced 20 Hz. It compares eight one-packet loss windows
with one eight-packet loss window in both sender directions. Aggregate loss,
retransmission, physical, unique, and delivery totals must remain equal between
the shapes. Proxy stages are switched atomically, every transfer must complete,
no full 50 ms sampling window may be missed, and the maximum measured interval
must remain below 80 ms.

The retained candidate discriminates the coarse 50 ms loss shape but not exact
packet adjacency within a window. Its cost figures are projections from an
uncontrolled runtime candidate, not a release gate. Validate it with
`qualification/validate_loss_shape_observability.py`.

Retained results:

- [2026-08-29 macOS arm64 20 Hz loss-shape observability](results/2026-08-29-macos-arm64-loss-shape-observability.json)

## Packet-loss capture runner

Build `robotweax_srt_telemetry_public_srt_incident_peer` once against each exact
provider. The runner starts the selected sender/receiver pair and a user-space
UDP proxy between them. Robotweax-to-Haivision remains the default direction:

```sh
python3 qualification/run_constant_loss_capture.py \
  --direction robotweax-to-haivision \
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
  --record-jsonl \
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

With `--record-jsonl`, both provider-linked peers feed their real adapter
samples into the SDK bounded recorder and atomically publish `sender.jsonl` and
`receiver.jsonl`. The runner then writes `analysis.json` through the local
analyzer and adds all three hashes to the manifest. `--incident-id` may assign a
new retained identity; it never changes the provider direction or fault plan.

Use `--direction haivision-to-robotweax --messages-per-interval 201` for the
retained reverse-direction gap-loss profile. The extra, non-dropped message
after the tenth fault makes the final injected gap visible to the receiver.
That profile does not qualify flight-tail recovery when no later DATA follows.

Select `--loss-pattern burst` to discard one contiguous range instead. The
retained burst profile uses the defaults `--burst-start 81 --burst-length 20`
with 200 messages. Positions 101 through 200 expose the gap after the burst;
the public one-second counters quantify its severity and recovery but do not
independently encode that the lost sequences were adjacent.

Retained captures:

- [2026-08-28 Robotweax sender to Haivision receiver constant loss](../testdata/golden-incidents/incident-002-captured-constant-loss-robotweax-haivision/manifest.json)
- [2026-08-28 Haivision sender to Robotweax receiver constant loss](../testdata/golden-incidents/incident-003-captured-constant-loss-haivision-robotweax/manifest.json)
- [2026-08-29 Robotweax sender to Haivision receiver burst loss](../testdata/golden-incidents/incident-004-captured-burst-loss-robotweax-haivision/manifest.json)
- [2026-08-29 Haivision sender to Robotweax receiver burst loss](../testdata/golden-incidents/incident-005-captured-burst-loss-haivision-robotweax/manifest.json)
- [2026-08-29 Robotweax sender to Haivision receiver direct JSONL analysis](../testdata/golden-incidents/incident-006-jsonl-constant-loss-robotweax-haivision/manifest.json)
- [2026-08-29 Haivision sender to Robotweax receiver direct JSONL analysis](../testdata/golden-incidents/incident-007-jsonl-constant-loss-haivision-robotweax/manifest.json)

## Operator comparison

`tools/operator_evaluation.py` prepares and runs the counterbalanced comparison
between normalized public SRT statistics and the deterministic incident
report. It measures time to first deviation, time to a useful failure domain,
accuracy, unsupported hypotheses, missing-evidence recognition, and operator
confidence. See the [operator evaluation protocol](../docs/qualification/operator-evaluation.md)
for the immutable packet, privacy rules, evidence classifications, commands,
and claim limits.

`qualification/test_operator_evaluation.py` exercises packet creation, both
arms, scoring, aggregation, overwrite refusal, inference isolation in the raw
condition, and tamper detection. Its responses are synthetic test fixtures and
do not count as operator or design-partner evidence.
