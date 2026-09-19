# Linux ARM64 measured replay regressions

These twelve recordings are byte-identical copies of the six rejected captures
in Network Lab PR #48, commit `359eb9b756dad4924786a1d4585df3f1eae62c0e`:
https://github.com/Robotweax/srt_network_lab/pull/48

Archive: `results/2026-09-19-srt-telemetry-qualification/evidence.tar.gz`

Archive SHA-256: `3154cb73013c91508cf2ac3fbc0f953788f3c21be41aceaf36a7e5f337df41bc`

Archive member prefix: `runs/qualification-9fe30a8-udp32m/`. The remaining
directory and file names are preserved here. `SHA256.json` records original
member digests. No samples were changed or inferred. Provider identities are
in the JSONL headers. Capture SDK revision:
`9fe30a833c0c5f755a5f57249103f1a955790b80`.

## Review result

The archive validator passes, but its 24/6 exit-code check does not verify
analyzer outcomes for the rejected captures. Those captures contain paired
JSONL recordings, but no retained `analysis.json`: the scenario gates rejected
them before analysis. Replaying them distinguishes two different outcomes:

| Captures (three repetitions each) | Capture gate | Analyzer replay |
| --- | --- | --- |
| Receiver pause, Haivision to Robotweax | Missing sender buffer/flight overlap | `insufficient-evidence` |
| Bandwidth reduction, Robotweax to Haivision | Sender drops violate pure queue-growth scenario | `sender-side-pressure`, supported |

Thus the Lab summary's bandwidth `diagnosis: insufficient-evidence` is not an
analyzer result. The 24/30 qualification count remains correct; the description
of all six rejections as analyzer abstentions needs correction upstream.

Receiver buffers exceed their threshold in samples 3–6 in all three pause
runs, then recover. Sender buffer/flight evidence does not cross the required
threshold, so the existing analyzer abstains. This is an observability limit
of the current rule, not proof of missing receiver pressure.

Bandwidth runs show sender buffer/flight growth in samples 3–6, followed by
415, 501 and 398 sender drops at sample 7. Sender queues recover at sample 7;
the overall incident recovers at sample 8. All three recordings nevertheless
account for 2,400 unique sent and received packets. A sender drop counter must
not be equated with application delivery loss without further evidence.
The subsequent protocol investigation resolved the counter semantics:
[SRT PR #48](https://github.com/Robotweax/srt/pull/48), merged as
`09316eda48e3360808fff2021eddc2f2bf821b06`, defines local TTL/TLPKTDROP buffer
removals. Its separately instrumented kernel-shaping runs demonstrate that
already submitted originals can arrive after the sender abandons its retained
copies. These earlier uninstrumented recordings do not themselves identify
the removal cause or per-packet deadline. The regression also checks that
reports explain this boundary without treating complete unique-packet counts
as an application payload-integrity check.

The analyzer and capture gates are identical between main `2e63753` and the
qualified revision `9fe30a8`. Telemetry PR #3 changes only the incident peer's
profile setup and triggered recording test ordering. It explicitly sets live
transport/message mode before connection, removes the unsupported connected
transport-type query, and finishes sampling both peers before sending DONE.
This is consistent with the recorded Haivision failure and its correction.
The patch was inspected; provider runtime tests were not rerun on this host.

## Reproduction

From the SDK repository root:

```sh
python3 -B qualification/test_linux_lab_replay.py -v
python3 -B tools/analyze_incident.py --jsonl \
  testdata/linux-lab-2026-09-19/bandwidth_reduction--robotweax-to-haivision--rep1/sender.jsonl \
  testdata/linux-lab-2026-09-19/bandwidth_reduction--robotweax-to-haivision--rep1/receiver.jsonl
```

The regression runs without provider libraries or network access. It checks
original recording hashes, both gate rejections, the separate analyzer
decisions and measured evidence across all six recording pairs. It does not
relax diagnostic thresholds or turn rejected captures into passing scenarios.
These runs used UDP proxies, not kernel netem. Logical timestamps do not
establish wall-clock rates or cross-host ordering. Human evaluation is open.
