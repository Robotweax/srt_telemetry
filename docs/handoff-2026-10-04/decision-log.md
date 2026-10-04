# Decision log

Decisions and findings behind the code, in the order they were made, for
whoever continues the project. The architecture docs describe what the code
does; this log records why, and what was tried and rejected. Dates are 2026.

## Direction and scope

- **30 Sep — portfolio positioning (maintainer).** Three separate projects:
  - **SRT Telemetry** (this repository): endpoint-side, public-API based
    (`srt_bistats`), role-specific statistics feeding dashboards.
  - **SRT Expert Analyzer:** wire-side, packet-based protocol dissector core,
    used by SRT Doctor and a Wireshark plugin.
  - **SRT Doctor:** consumes both, Telemetry as endpoint evidence and the
    Expert Analyzer as wire evidence.

  An earlier proposal (29 Sep) to merge Telemetry, Flight Recorder and the
  Expert Analyzer into one "SRT Doctor" was dropped for these two.
- **30 Sep — analysis that set the direction.** The repository had a good
  foundation (contract, non-destructive deltas, availability bits, recorder,
  JSONL, provider-neutral adapter) followed by a detour into diagnosis: five
  hypotheses, a paired-endpoint analyzer and an operator-study harness, with
  no sampler, exporter or dashboard. Recommendation, adopted: dashboard
  claim first, hygiene, build the dashboard path, freeze the analyzer and
  study harness for Doctor, keep "unavailable instead of fake zero" in every
  panel.
- **1 Oct — maintainer decisions:** MIT licence; the built-in web UI ships in
  1.0; single-endpoint verdicts with knob advice belong here, cross-endpoint
  causation to SRT Doctor (formal confirmation still open); the analyzer and
  operator-evaluation harness are frozen.
- **1 Oct — attribution:** commits as Marc Cymontkowski
  (`117546249+SRTPioneer@users.noreply.github.com`), no Claude trailers or
  footers; `.claude/settings.json` disables Claude Code attribution.
- **2 Oct — fix before moving on:** issues found while building a part are
  fixed before the next part starts.
- **3 Oct — part 5 scope:** start the reference integrations with
  srt-live-transmit only; after its merge the maintainer paused ("just merge
  for now").

## Phase 1: metric model

- `SRT_TRACEBSTATS` has 82 fields with the same layout in Haivision 1.5.5 and
  Robotweax SRT: 54 distinct observations, 52 mapped through `raw_sample_v1`
  and `ext_sample_v1`, 2 interval rates excluded (derived instead).
- `mbpsBandwidth` means different things: a link-capacity estimate in
  Haivision, a measured arrival rate in Robotweax. It maps to two distinct
  contract fields, each qualified only for its provider.
- Connection monitor: state, reject reason, about 24 options, versions, KM
  state, packet filter, stream ID (opt-in, because stream IDs can identify
  customers); change mask and events. After a failure Haivision reports
  OPENED, Robotweax BROKEN; the health context treats OPENED after a
  connection as lost. `BADSECRET` = 10 in both.

## Phase 2: sampler, windows, health

- 32 metrics (rates, gauges, ratios). Recovery headroom = receive-buffer span
  ÷ latency, only with at least 8 packets per latency period. The send-buffer
  ratio uses the sender drop threshold max(latency, 1 s) + 20 ms.
- Windows are tumbling and wall-clock aligned (1 s, 10 s, 60 s) with min, max,
  mean, last, p95 and exact rate totals; quality flags mark partial windows,
  gaps, clock steps, counter resets and source errors.
- Sampler: sources are callbacks with at most 8 labels; own thread or manual
  tick; `remove_source` guarantees no further calls; a failed source recovers
  after 1 s of good samples.
- History: compact record of 112 bytes per window with the worst verdict and
  its reason. Event journal: bounded ring with contiguous sequences.
- Health: 8 indicators; required indicator per role (unrecovered ratio for
  receivers, sender backlog for senders); count window 10 s, gauge window
  1 s; hysteresis enter 2 s, leave 10 s; Unknown for missing data, stopped
  streams, failed reads and unknown role; a lost connection is Failing at
  once. Default thresholds are uncalibrated.
- **Fault lab** instead of netem: an in-process impairing UDP relay, so the
  gate runs in CI on both providers. Gate: the right verdict in at least 9 of
  10 fault classes per role and never a wrong OK. Result: 12/12 per role,
  advice 8/8 (sender) and 7/7 (receiver). Provider differences seen there:
  Haivision retransmits heavily on reordering and during outages and breaks
  the connection when a stalled receiver's buffer overflows; Robotweax has no
  capacity estimate.

## Phase 3

### Part 1: names and renderers (PR #18)

- Two name styles from one table: OpenTelemetry `srt.<scope>.<name>` (JSON,
  OTLP, UI) and Prometheus `srt_<scope>_<name>_<unit>`. Bit rates in bit/s on
  purpose (SRT settings, encoders and operators work in bit/s); byte
  counters count wire bytes including headers.
- Window statistics are a `stat` label; every query must select one. 18
  lifetime counters are exported as exact integers. Host label keys that
  collide with exporter labels get `host_` prefixes until unique.
- JSON snapshot `srt-telemetry.snapshot.v1` with `_ms` timestamps for
  JavaScript (nanoseconds exceed 2^53). A window older than two window
  lengths is left out as stale, judged on the steady clock.

### Part 2: HTTP and WebSocket server (PR #19)

- One thread, POSIX sockets, GET/HEAD only, 127.0.0.1 by default. Bearer
  token; DNS-rebinding protection through a Host check (IP literals,
  localhost and configured names only without a token); Origin check for the
  WebSocket; request deadlines and eviction of idle connections; WebSocket
  backlog limits; 4 connections always kept free for scrapes; CLOEXEC;
  EMFILE backoff; IPV6_V6ONLY.
- The browser WebSocket token travels in `?token=` (browsers cannot set the
  header); documented.

### Part 3: web UI and engine fixes (PR #20)

- Vanilla JavaScript, no build step, embedded into the library by
  `cmake/EmbedUiAssets.cmake`; strict CSP, no-referrer, ETag/304; flag
  `HTTP_NO_UI`; `frame_ancestors` lets named consoles frame it (refuses `*`,
  bare schemes and single-label wildcards).
- The fleet headline is never "all clear" while an endpoint is ungraded.
  Watch never asks for a change ("If it persists: …").
- Data flow: snapshot, then the live feed; polling with backoff as fallback;
  gaps refetched from events and history; a 10 s watchdog. The fleet keeps
  2 min per source; the viewed source keeps 1 h of chart statistics.
- Engine fixes made in two independent review rounds (each fix has a test
  that failed before):
  - drops are diagnosed when they happen and the diagnosis is kept while
    they are counted;
  - outage aftermath = 2 s + latency + max(latency, 1 s) + 2 intervals; the
    receiver remembers arrival gaps (≥ 100 ms, after the first packet, at
    most 64) beyond the count window;
  - sender acknowledgement stall: send-buffer gauge above Watch with no other
    cause, starting after a drop-free gauge window; it never overrides
    `receiving_application` or MAXBW advice;
  - weak advice (retransmission share alone, or no cause) gives way to a
    concrete diagnosis kept from the count window; kept advice loses its
    knob once the setting was changed as advised;
  - sender capacity advice needs loss reports, congestion, or RTT above
    1.5 × baseline + 5 ms.
- The fault lab now scores advice on every At risk/Failing read from 4 s
  after the first until the end of the settle phase; new scenarios
  `outage_long_latency` and `outage_then_cap` (12 fault classes).

### Part 4: OTLP, alert rules, Grafana (PR #21)

- **OTLP renderer:** one OTLP/JSON request; resource attributes validated (at
  most 32, unique keys, UTF-8 without control characters) with defaults per
  key; scope `robotweax.srt_telemetry` with `srt.metric_schema`; window
  gauges carry a `stat` attribute; counters are cumulative monotonic sums
  that start at the connection time (sample time minus the provider's
  connection age); units are annotations (`{verdict}`) so a Collector adds
  no `_ratio` suffix.
- **Push exporter:** plain HTTP only (send to a local Collector, which adds
  TLS); percent-decoded headers; IPv6 zones; retries on connection errors,
  timeouts and 429/502/503/504 within the interval; name lookups on a helper
  thread so `stop()` returns within about 100 ms.
- **Decision:** dashboards and alerts target the native Prometheus names.
  Through a Collector, window and counter names differ (for example
  `srt_receiver_unique_received_bitrate_bit_per_second`) and `job`/`instance`
  name the Collector; the stream alerts still work because they match on
  all labels.
- **Alert rules:** Failing (10 s), At risk (30 s), Ungraded (2 min, verdict
  Unknown while statistics read), statistics failing (1 min, not for lost
  connections), target down (recording rule `srt_telemetry:target` keeps the
  mark while a target is down, however long). Descriptions carry the reason,
  the advice and the value formatted for the knob.
- **Grafana:** fleet and endpoint dashboards generated by a script (Grafana
  12, schemaVersion 41); table queries wrapped in `max without (stat)` with a
  merge transform so rows join.
- The independent review found 10 issues, all fixed: Collector label
  collisions, counter start time, resource validation, blocking name
  lookups, target-down alerts expiring after 1 h, retry budget, header
  decoding, early error responses, IPv6 parsing, doc claims.

### Part 5a: embedding library and srt-live-transmit (PR #22)

- **Why a new library:** wiring one socket by hand took about 70 host lines,
  far over the gate of 30. The embed object runs the sampler, health,
  history, journal, HTTP server, optional OTLP and, per socket, the
  statistics session and connection monitor.
- One embed thread polls the monitors and ticks the sampler (manual tick)
  every 100 ms under the embed mutex; `attach`/`detach` take the same mutex,
  so they never race a tick. Hosts must not start the sampler themselves or
  call `attach`/`detach` from sampler callbacks.
- A command-line style spec (`[addr]:port|:port|port|off` plus `name=`,
  `token=`, `token_file=`, `host=`, `otlp=`, `otlp_interval_ms=`,
  `history_s=`, `linger_s=`, `stream_id`, `no_ui`) keeps a host to one
  option.
- **Connections over time:** a detached socket stays as "connection lost" for
  the linger (default 60 s); an attach with the same role and labels during
  the linger continues that source on the new socket, so a reconnection, or
  a peer that stays down, is one row with continuous history. A continued
  socket reads as lost until it connects. Events keep the first rejection per
  reason; KM, settings and peer-version changes count only for connected
  sockets (a wrong-passphrase caller retrying about 10 times per second had
  produced 250 events in 9 s).
- The provider is detected at compile time from Robotweax SRT's `srt.h`
  guards in the host header.
- **srt-live-transmit patch:** telemetry is declared after the source and
  target so it stops before they close and before `srt_cleanup`; sockets are
  attached when they exist (caller and rendezvous while connecting, listener
  on accept) and detached in the auto-reconnect branch. 28 host lines, lines
  no longer than the host's own.
- The independent review found 9 issues, all fixed: sockets closed before
  telemetry stopped on exit, invisible connection attempts, undocumented
  sampler rules, continuation identity (documented), label validation, a
  header example, line length, build parallelism, spec usability.

## Earlier findings still relevant to SRT Doctor

- The frozen analyzer aligns two-sided samples by index (timestamps never
  compared), ignores `interval_nano`, uses absolute thresholds from samples
  1 and 2, and lets any sender drop veto the path-loss hypothesis.
- Retained golden incidents 002–015 use synthetic 1 s timestamps (stated in
  their manifests), so their bit/s values are not physical.
- The market already had raw-counter export (srt-prometheus-exporter);
  differentiation comes from role semantics, availability, drop cause,
  unique vs retransmitted, and provider neutrality.
