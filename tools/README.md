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
