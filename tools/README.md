# Tools

| Tool | Status | Purpose |
| --- | --- | --- |
| `analyze_incident.py` | Frozen | Paired sender/receiver incident analyzer with five bounded hypotheses |
| `operator_evaluation.py` | Frozen | Counterbalanced raw-statistics vs incident-report study harness |
| `operator_reference_review.py` | Frozen | Rubric checker for the operator-evaluation packets |

As of 2026-10-01 these tools are frozen. Cross-endpoint diagnosis is SRT
Doctor's job; SRT Telemetry focuses on single-endpoint sender and receiver
dashboards (see [roadmap](../docs/roadmap.md)). The tools stay in place
because existing tests, the retained golden incidents and
[Flight Recorder](https://github.com/Robotweax/srt_flight_recorder) use them.
Bug fixes are welcome; new hypotheses or study features belong in SRT Doctor.

Known limitations to fix before Doctor reuses the analyzer: samples are aligned
by index rather than time, `interval_nano` is unused, thresholds are absolute
and taken from samples 1 and 2, and any sender drop vetoes the path-loss
hypothesis.

## Active tools

| Tool | Purpose | Run by |
| --- | --- | --- |
| `metric_names_doc.py` | Regenerates the metric names document (below) | By hand after a name change |
| `otlp_validate.py` | Checks OTLP/JSON against the official protobuf definitions | CTest (`otlp_proto_test`) |
| `check_dashboards.py` | Dashboards and alert rules use known metrics and select a `stat`; dashboards match their generator; with `--prometheus`, every query returns data | CI (evidence job), `observability_e2e.py` |
| `observability_e2e.py` | Demo, OpenTelemetry Collector, Prometheus with the alert rules and Grafana, end to end | CI (Haivision adapter job) |
| `ui_smoke_test.py` | Built-in web UI in headless Chrome against the dashboard demo | CTest (label `ui`) when a browser is configured |
| `count_host_lines.py` | Host lines a reference-integration patch adds (gate: 30) | CI (Haivision adapter job) |
| `srt_live_transmit_e2e.py` | Patched srt-live-transmit end to end: grading, loss, reconnection, refusal | CI (Haivision adapter job) |

See the [handoff](../HANDOFF.md#build-and-test) for the commands.

## Metric names document

`metric_names_doc.py` regenerates `docs/architecture/metric-names.md` from the
naming tables in `sdk/src/export.cpp`. Run it after changing a name; the
export tests fail when the document does not list every name.
