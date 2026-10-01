# Roadmap

SRT Telemetry is the embeddable library and dashboard stack for observing an
SRT stream from the sender or the receiver side. The plan below replaces the
earlier diagnostic-proof roadmap. Decisions taken on 2026-10-01: MIT licence;
the built-in web UI ships in 1.0; cross-endpoint diagnosis belongs to SRT
Doctor.

Effort is in engineer-weeks (EW). Gate thresholds are proposals to confirm
during Phase 0.

## Phase 0: hygiene and scope (1.5 EW): done

- MIT licence, installed with the package;
- CI on Linux (GCC, Clang) and macOS for the core SDK, and the public SRT
  adapter against Haivision SRT v1.5.5 and Robotweax SRT `main`;
- installed-package consumers for static and shared builds, including a C-only
  host linking the static adapter;
- the IPv6-only JSONL recording test skips cleanly where `::1` is missing;
- README leads with the dashboard purpose; and
- incident analyzer and operator-evaluation harness frozen as reference
  material for SRT Doctor (see [tools](../tools/README.md)).

**Gate:** CI green on both providers; the IPv6-only test skips cleanly.

## Phase 1: complete metric model (2 EW): in progress

- every distinct observation of `SRT_TRACEBSTATS` (54) accounted for per role
  in the [field map](architecture/srt-stats-field-map.md): 52 mapped through
  `raw_sample_v1` plus the new `ext_sample_v1`, 2 deliberately excluded:
  **done**;
- buffer gauges read with `instantaneous=1`: **done** (already the adapter's
  behaviour);
- FEC capability gating and the group field set: **done** (live FEC and
  bonding values move to Phase 4);
- socket state, negotiated options, KM state and reject reason as events:
  next.

**Gate:** field-coverage test makes all 52 mapped observations available on
both providers: **passing** on Haivision SRT v1.5.5 and Robotweax SRT `main`.

## Phase 2: sampler, windows, health engine (3 EW)

- in-SDK sampler at 100 ms per socket with host labels, on its own thread;
- min/max/p95 windows at 1 s, 10 s and 60 s;
- event journal and a compact 1 s record (target 128 bytes or less); and
- eight health indicators producing per-role verdicts (OK, Watch, At risk,
  Failing, Unknown) with hysteresis, reason codes and knob advice.

**Gate:** the right verdict in at least 9 of 10 lab fault classes per role, and
never a wrong OK.

## Phase 3: outputs and dashboards (3-4 EW)

- streaming callback, Prometheus, OTLP and a WebSocket feed sharing one
  versioned metric naming scheme;
- embedded web UI with sender, receiver and fleet views;
- Grafana dashboards and alert rules; and
- reference integrations: srt-live-transmit, GStreamer, Robotweax gateway.

**Gate:** each integration takes 30 host lines or fewer; an operator answers
"fine, or which knob" from one screen.

## Phase 4: qualification (1.5 EW)

- 24 h soak, reconnect storms, overhead at 1,000 sockets and 10 Hz; and
- encryption, FEC and bonding scenarios added to the fault lab.

**1.0 gate:** overhead under 1 % of one core; no unexplained gaps in 24 h;
default thresholds checked on field streams.

## Completed before this plan

The evidence foundation stays the base of every phase above:

- common provider contract, versioned C ABI, non-destructive lifetime-counter
  delta engine, availability and data-quality model;
- injected public SRT API adapter working unchanged against Haivision SRT
  v1.5.5 and Robotweax SRT;
- bounded in-memory recorder with versioned JSONL export;
- cross-provider interop, sampling and lifecycle candidates, and the 0.2.6
  qualification archive;
- deterministic two-process fault lab with 15 retained golden incidents; and
- the incident analyzer and operator-evaluation harness, now frozen.
