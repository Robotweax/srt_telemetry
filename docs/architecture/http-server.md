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
| `GET /api/v1/sources/{id}/history[?since_ms=T]` | Compact per-window history of one source from `T` (Unix ms; default the last hour), at most 3,600 records per response. When `more` is true, fetch the next page with `since_ms=next_since_ms`. Pages follow the order windows closed, which is time order unless the wall clock stepped back. `next_since_ms` always moves forward, so following it cannot loop. It needs `sampler_enable_history` |
| `GET /api/v1/events[?after=N]` | Up to 1,000 journal events after sequence `N`, with `overwritten` and `more`. It needs `sampler_enable_journal` (else 404) |
| `GET /api/v1/live[?window=S]` | WebSocket (RFC 6455) feed |
| `GET /healthz` | `ok`, without authentication or host check, for liveness probes |

HEAD is served for every GET endpoint except the feed.

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
(answered with close). Protocol errors close the feed with 1002 (or 1009 for
frames over 64 KB):
- reserved bits or opcodes;
- unmasked frames;
- control frames over 125 bytes or fragmented;
- a one-byte close payload.

The server pings a quiet client every 30 s and drops one that has sent nothing
(not even a pong) for 75 s.

A client that falls behind:
- with 4 MB queued: snapshots and events are skipped, its pings go
  unanswered, and its input is no longer read;
- if it stays behind for 30 s, or its queue would exceed 16 MB: it is
  disconnected.

At most `max_connections` − 4 feed clients are accepted (503 beyond), so
scrapes always find a slot. Stopping the server sends close code 1001 (going
away).

## Security

- **Local by default.** `bind_address` defaults to `127.0.0.1`. Exposing the
  server (`0.0.0.0`, `::`, a LAN address) is the host's decision.
- **Bearer token.** With `bearer_token` set, every request except `/healthz`
  needs `Authorization: Bearer <token>`. Browsers cannot set headers on a
  WebSocket, so the feed also accepts `?token=<token>`. The query is
  percent-decoded, and `+` stays a plus. A token in a URL can end up in proxy
  and browser logs, so prefer the header where the client allows it. Tokens
  are compared in constant time. There is no TLS: put a reverse proxy in
  front of the server when it is exposed across networks.
- **DNS rebinding.** A web page on `attacker.example` can point its own name at
  127.0.0.1 and then read a local server as "same origin". Without a token,
  the server therefore answers only requests whose `Host` is one of:
  - `localhost` (or `*.localhost`);
  - an IP literal;
  - a name listed in `allowed_hosts`.

  Any other host gets 421. A trailing dot (`name.`) is ignored. With a token,
  any `Host` is accepted, because the page cannot know the token.
- **Cross-site feed.** A WebSocket request whose `Origin` names another host
  or port is refused (403), as is `Origin: null`. The default ports (80, 443)
  are filled in before comparing. A `Host` without a port, as a reverse proxy
  (often TLS) forwards it, is compared by name only.
- **Read-only and bounded.**
  - Only GET and HEAD are served (405 otherwise). A request needs exactly
    one `Host` header (400 otherwise; HTTP/1.0 may omit it).
  - Requests over 16 KB get 413; unknown paths get 404.
  - A client has `request_timeout_ms` (default 5 s) from accepting the
    connection to send its request head, then gets 408. Trickling bytes does
    not extend this.
  - A response may stall for at most `request_timeout_ms` while the client
    does not read; slow but steady readers are not cut off.
  - At most `max_connections` (default 32) connections. When they are all
    taken, the oldest connection still waiting for its request head is
    closed to make room. A new client gets 503 only when every connection is
    a feed client or already being answered. Slow clients therefore cannot
    keep scrapes out: 32 trickling sockets that reconnected every 9 s left
    50 of 50 `/metrics` requests answered, against 18 of 50 before.
  - WebSocket input is read in bounded amounts per loop, frames are at most
    64 KB, and output is bounded as described under Live feed.
  - Descriptors are close-on-exec, so processes the host starts do not
    inherit the listening socket. When the process runs out of descriptors,
    `accept` backs off instead of spinning.
  - `::` binds IPv6 only on every platform; bind `0.0.0.0` for IPv4. Link-local
    addresses with a scope ID are not supported.
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
- every endpoint, HEAD, and every error status (400, 401, 403, 404, 405, 408,
  413, 421, 503);
- the WebSocket handshake against the RFC 6455 example key, snapshot cadence,
  live events, ping and pong, and the close handshake;
- RFC 6455 protocol errors, and frames sent right behind the upgrade;
- the window choice, token handling (including `+`), the feed-slot reserve,
  eviction of the oldest idle connection when full, and going-away on stop;
- a feed behind a TLS proxy (`https` Origin, `Host` without a port),
  duplicate `Host`, and trailing dots;
- history pages;
- the abuse cases found in review:
  - DNS-rebinding hosts;
  - a ping flood from a client that never reads (others still answered
    within 1 s);
  - a trickled request (408 at the deadline);
  - clients that never read their responses (slots freed at the deadline).

The review's own attack scripts were rerun against the hardened server:
- 32 clients that never read a history response: RSS stayed at 15.7 MB, and
  `/metrics` was answered throughout. Before: 1.15 GB and 503 for 48 s.
- A flood of 64 KB pings is closed as a protocol error.
- A flood of legal 125-byte pings leaves `/healthz` answering in 3 ms.

It passes under TSan and ASan/UBSan, and with gcc and clang. Installed-package
consumers link the server from C, as a static and as a shared library.
An end-to-end run with real SRT (Robotweax SRT 0.2.6, 5.4 Mbit/s) served
`/metrics` (26 KB) in 0.4 ms, with health OK on both ends, journal events,
13 history records and the live feed.
