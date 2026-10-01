# Embedded HTTP server

An optional library, `RobotweaxSRTTelemetry::http` (header
`robotweax/srt_telemetry_http.h`), serves a sampler's state to Prometheus, to
the built-in UI (next) and to any HTTP or WebSocket client. Its properties:
- one thread;
- no dependencies beyond POSIX sockets;
- read-only (GET only).

The core library stays free of networking. CMake option
`ROBOTWEAX_SRT_TELEMETRY_BUILD_HTTP_SERVER`: on by default on Linux and macOS,
off on Windows (not supported yet).

```c
robotweax_srt_telemetry_http_server_config_v1 config;
robotweax_srt_telemetry_http_server_config_default(sampler, &config); /* 127.0.0.1, free port */
config.port = 9810;
robotweax_srt_telemetry_http_server *server;
robotweax_srt_telemetry_http_server_start(&config, &server);
/* ... */
robotweax_srt_telemetry_http_server_stop(server);   /* before destroying the sampler */
```

## Endpoints

| Request | Response |
| --- | --- |
| `GET /metrics` | Prometheus text 0.0.4, default window ([names](metric-names.md)) |
| `GET /api/v1/snapshot[?window=S]` | JSON snapshot, `srt-telemetry.snapshot.v1`; `S` must be a configured window length in seconds |
| `GET /api/v1/sources/{id}/history[?since_ms=T]` | Compact per-window history of one source from `T` (Unix ms; default the last hour). It needs `sampler_enable_history` |
| `GET /api/v1/events[?after=N]` | Up to 1,000 journal events after sequence `N`, with `overwritten` and `more`. It needs `sampler_enable_journal` (else 404) |
| `GET /api/v1/live[?window=S]` | WebSocket (RFC 6455) feed |
| `GET /healthz` | `ok`, without authentication, for liveness probes |

History records carry, per window:
- `start_unix_ms`, `window_ms`, `samples`;
- `quality` (window flag bits);
- `health` (worst verdict and reason);
- `metrics`, keyed by the [OpenTelemetry names](metric-names.md): `rate`,
  `mean`, `min` or `max` for values, `total` for packet counts.

Events carry:
- `sequence`, `observed_unix_ms`, `source_id`;
- `kind`, `severity`;
- `previous`, `current`, `code`, `detail`.

## Live feed

After the upgrade, the server sends text messages:

- `{"type":"snapshot","data":<snapshot>}`: at once, then every
  `live_interval_ms` (default 1 s), rendered from the requested window. Use
  `window=1` for a lively UI.
- `{"type":"events","events":[...]}`: journal events added since the client
  connected, within about 100 ms of being recorded. Clients load earlier
  events from `/api/v1/events`.

Client messages are ignored, except ping (answered with pong) and close
(echoed). A client that falls behind is handled in two steps:
- more than 4 MB queued: snapshots are skipped;
- more than 16 MB queued: it is disconnected.

Stopping the server sends close code 1001 (going away).

## Security

- **Local by default.** `bind_address` defaults to `127.0.0.1`. Exposing the
  server (`0.0.0.0`, `::`, a LAN address) is the host's decision.
- **Bearer token.** With `bearer_token` set, every request except `/healthz`
  needs `Authorization: Bearer <token>`. Browsers cannot set headers on a
  WebSocket, so the feed also accepts `?token=<token>`. Tokens are compared in
  constant time. There is no TLS: put a reverse proxy in front of the server
  when it is exposed across networks.
- **Cross-site feed.** A WebSocket request with an `Origin` that does not name
  this host is refused (403). Other sites' pages therefore cannot read the
  feed, even from a browser on the same machine.
- **Read-only and bounded.**
  - Only GET is served (405 otherwise).
  - Requests over 16 KB get 413; unknown paths get 404.
  - At most `max_connections` (default 32) connections; one more gets 503.
  - Idle HTTP connections close after 30 s.
  - Client WebSocket frames must be masked and at most 64 KB.
- **No file serving yet.** The UI assets arrive with the next part, embedded
  in the library, so no path from a request ever reaches the file system.

Each response closes its connection (`Connection: close`) after a graceful
shutdown, so a client still sending gets its response rather than a reset.

## Threading

The server thread renders through `sampler_render_to` and reads history and
the journal through the public sampler calls. Each takes the sampler lock
briefly. A WebSocket tick renders once per distinct window. Stop the server
before destroying the sampler, and never from a sampler callback.

## Tests

`robotweax_srt_telemetry_http_tests` drives the server over real sockets:
- configuration errors and port in use;
- every endpoint and error status (400, 401, 403, 404, 405, 413, 503);
- the WebSocket handshake against the RFC 6455 example key, snapshot cadence,
  live events, ping and pong, and the close handshake;
- the window choice, token handling, the connection limit, and going-away on
  stop.

It passes under TSan and ASan/UBSan, and with gcc and clang. Installed-package
consumers link the server from C, as a static and as a shared library.
An end-to-end run with real SRT (Robotweax SRT 0.2.6, 5.4 Mbit/s) served
`/metrics` (26 KB) in 0.4 ms, with health OK on both ends, journal events,
13 history records and the live feed.
