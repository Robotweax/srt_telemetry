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

## Phase 1: complete metric model (2 EW): done

- every distinct observation of `SRT_TRACEBSTATS` (54) accounted for per role
  in the [field map](architecture/srt-stats-field-map.md): 52 mapped through
  `raw_sample_v1` plus `ext_sample_v1`, 2 deliberately excluded;
- buffer gauges read with `instantaneous=1`;
- FEC capability gating and the group field set (live FEC and bonding values
  move to Phase 4); and
- the [connection monitor](architecture/connection-monitor.md): socket state,
  reject reason, negotiated options, encryption state, packet filter and
  opt-in stream ID, with change detection and derived events.

**Gate:** the field-coverage test makes all 52 mapped observations available
on both providers, and the connection-events test passes plain, encrypted,
wrong-passphrase, FEC and peer-close scenarios on both: **passing** on
Haivision SRT v1.5.5 and Robotweax SRT `main`.

## Phase 2: sampler, windows, health engine (3 EW): done

- [metric catalog and min/max/p95 windows](architecture/metrics-and-windows.md)
  at 1 s, 10 s and 60 s: **done**;
- [in-SDK sampler](architecture/sampler.md) at 100 ms per socket with host
  labels, on its own thread or driven by the host: **done**;
- [event journal and a compact 1 s record](architecture/history-and-events.md)
  (112 bytes, 24 h ≈ 9.7 MB per socket): **done**; and
- [eight health indicators](architecture/health-engine.md) producing per-role
  verdicts (OK, Watch, At risk, Failing, Unknown) with hysteresis, reason codes
  and knob advice: **done**. The [fault-lab gate](qualification/fault-lab.md)
  passes on both providers: 12 of 12 fault classes per role, no wrong OK, and
  the right knob on every At risk or Failing read (added 2 October 2026).

**Gate:** the right verdict in at least 9 of 10 lab fault classes per role, and
never a wrong OK.

## Phase 3: outputs and dashboards (3-4 EW): in progress

- streaming callback, Prometheus, OTLP and a WebSocket feed sharing one
  versioned metric naming scheme
  ([naming scheme, Prometheus and JSON renderers](architecture/metric-names.md):
  **done**; [embedded HTTP server and WebSocket feed](architecture/http-server.md):
  **done**; [OTLP renderer and push exporter](architecture/otlp.md): **done**);
- embedded web UI with sender, receiver and fleet views
  ([built-in web UI](architecture/web-ui.md): **done**);
- Grafana dashboards and alert rules
  ([alert rules and dashboards](architecture/alerting.md): **done**); and
- reference integrations: srt-live-transmit
  ([patch and build](../integrations/srt-live-transmit/README.md), 28 host
  lines on the [embedding library](architecture/embedding.md): **done**),
  GStreamer, Robotweax gateway.

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
