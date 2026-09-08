# Flight Recorder development evidence — 2026-09-08

Canonical newly executed capture: `live-capture-007`. The JSONL and capture
manifests are byte-identical copies of the final experiment, including exact
provider/library/peer digests and per-observation clock mapping intervals.
`native-source-manifest.json` binds the modified host sources and build inputs.
`native-capture-verification.json` retains measurements, limitations and the
failed 11,000-message stress attempt; it does not treat that attempt as accepted
receiver-pressure qualification. The quoted test log paths refer to this folder.

The three-mode nine-trial observations are in `overhead.json`. `view-budget.json`
records a separate sequential seven-example local import/analyze/render run.
Its preliminary 1 s/example and 128 MiB process RSS budgets protect a responsive
local support workflow for these small canonical examples; they do not qualify
maximum accepted archives. Native resource budgets and sample conditions are
stated in the native verification report. No human participants were measured.

Reproduce live collection with the [capture guide](../../docs/architecture/flight-recorder-capture.md).
Reproduce case packages with:

```sh
python3 tools/flight_recorder_demo.py --output /tmp/new-flight-recorder-demo \
  --capture-root qualification/flight-recorder-2026-09-08/live-capture-007
```

Run from repository root. This second command packages the retained new
experiment, not a rerun of the network. Re-running the capture guide opens fresh
sockets. The [shipped examples](../../testdata/flight-recorder/README.md) separate
new captures, older retained captures and synthetic derivatives. Check content
digests against `evidence-sha256.json`; its inventory excludes itself and this
README. All test/benchmark observations are scoped to macOS arm64.
