# Built-in web UI

The [embedded HTTP server](http-server.md) serves a dashboard for every source
of its sampler. It answers the operator's question from one screen: **is it
fine, and if not, which knob?** The UI is plain JavaScript without
dependencies or a build step. The files (`server/ui/`) are compiled into the
HTTP library (`cmake/EmbedUiAssets.cmake`), so a product ships no extra files,
and no request path ever reaches the file system.

```c
robotweax_srt_telemetry_http_server_config_default(sampler, &config);
config.port = 9810;
robotweax_srt_telemetry_http_server_start(&config, &server);
/* open http://127.0.0.1:9810/ */
```

Turn it off with `ROBOTWEAX_SRT_TELEMETRY_HTTP_NO_UI` in `config.flags`; the
data endpoints stay.

Try it with real SRT streams: `robotweax_srt_telemetry_public_srt_dashboard_demo`
(built with the public SRT adapter) runs five streams through impairing relays,
so the dashboard shows healthy, lossy, latency-starved, congested and
interrupted streams:

```sh
./build/robotweax_srt_telemetry_public_srt_dashboard_demo --port 9810
```

Options: `--token T`, `--bind ADDRESS`, `--streams N`, `--duration SECONDS`,
`--frame-ancestors "https://console.example.com"`.

## Fleet view (`#/`)

- **Tallies:** counts per verdict. Each tally filters the list.
- **Headline:** "N of M endpoints need attention" (At risk or Failing), or
  "N of M endpoints cannot be graded" (Unknown), or "M endpoints: no action
  needed". It never reports all clear while an endpoint is ungraded.
- **Table:** one row per source, worst first:
  - verdict, and how long it has held;
  - endpoint name and host labels;
  - role;
  - **What to do**: the advice in plain words, such as "Raise the latency to
    580 ms (SRTO_LATENCY)" or "Lower the bitrate to 2.3 Mbit/s";
  - bitrate, RTT, latency, loss before repair, dropped packets;
  - a two-minute bitrate sparkline.
- **Filters:** by name or label, and by role.
- **Recent events** from the journal.

## Source view (`#/source/{id}`)

- **Verdict and advice:** the verdict with the reason in operator language and
  how long it has held; next to it, the suggested change with its value. Watch
  never asks for a change: its advice is shown as "If it persists: …".
- **Now:** current values for the role, from the latest window.
  - Sender: sending rate, retransmission share, loss reported, drops,
    RTT, the receiver's latency (also as a multiple of RTT), send buffer
    against the drop limit, flow window, link estimate when the provider has
    one.
  - Receiver: receiving rate, loss before repair, unrecovered packets,
    repairs and late arrivals, RTT, latency, receive buffer and headroom,
    undecryptable and FEC counts when they occur.
- **Health indicators:** each applicable indicator with its value, state, what
  it measures and why it is not OK. The role's required indicator is marked.
- **History:** a verdict timeline and four charts for the last 5 min, 15 min or
  1 h, loaded from `/history` and extended by the live feed.
  - Sender: throughput (original and retransmitted), loss and recovery,
    timing (RTT and send buffer against the latency), margins (drop limit and
    flow window used).
  - Receiver: throughput, loss and recovery (lost, unrecovered, repairs),
    timing (RTT and receive buffer against the latency), recovery headroom.

  A missing window breaks the lines, so an outage shows as a gap, never as a
  smooth line. Hovering any chart shows the values at that second in all
  charts.
- **Events** of this source, and **all metrics** of the current window with
  every statistic, the lifetime counters and the source details.

## Data flow

1. `GET /api/v1/snapshot?window=1` (the default window when 1 s windows are
   not configured), then `GET /api/v1/events` page by page.
2. The WebSocket feed `/api/v1/live?window=1` delivers a snapshot every
   interval and new events.
3. If the feed drops, the page polls the snapshot and events every 2 s and
   reconnects with backoff (1 s up to 30 s). After a reconnect it fetches the
   events and the history it missed. A feed that stays silent for 10 s is
   treated as dead.
4. When the data is more than 5 s old, a banner says since when.

Events are merged by sequence number. When the server's journal overwrote
events before the page could read them, the event list says how many.

The fleet keeps two minutes of points per source; the source on screen keeps
an hour, and only the statistics the charts use. A fleet of 1,000 sources
renders without long tasks after the first build.

## Security

- The page builds its DOM from text only (`textContent`, `setAttribute`), so
  labels, stream IDs and event details from remote peers cannot inject markup.
  Bidirectional and invisible formatting characters are removed from names,
  so one stream cannot pose as another.
- Every UI response carries a strict Content Security Policy: no inline script
  or style, no other origin, `Referrer-Policy: no-referrer`, `nosniff`.
- **Framing.** By default the UI cannot be shown in a frame
  (`frame-ancestors 'none'`, `X-Frame-Options: DENY`), so no other page can
  overlay it. A product that embeds the dashboard in its own console lists
  that console in `frame_ancestors`, for example
  `"https://console.example.com"`. The policy then names exactly those
  sources and drops `X-Frame-Options`, which cannot list origins. Values are
  CSP sources separated by spaces (`'self'`, origins, wildcard hosts such as
  `*.example.com`). Refused: quotes other than `'self'`, `;` and `,`, so the
  setting cannot change the rest of the policy; and `*` or bare schemes
  (`https:`), which would let any site frame the UI. Inside a frame the page works as usual; if
  the browser blocks storage in third-party frames, the token is kept in
  memory for the page's lifetime.
- **Token.** The UI files hold no data and are served without the token. The
  page asks for it and keeps it in session storage for the tab. It can also be
  opened as `http://host:port/#token=<token>`: the fragment is never sent to
  the server, and the page removes it from the address bar at once (the
  browser may still keep the original URL in its history). Data requests send
  the token as `Authorization: Bearer`. The WebSocket feed takes it in the
  query (`?token=`), because browsers cannot set headers on a WebSocket; a
  reverse proxy may log it, so keep such logs private or terminate the feed
  at a proxy that strips it.
- Assets carry an ETag and `Cache-Control: no-cache`: browsers revalidate and
  get `304 Not Modified` while the library is unchanged.

## Accessibility

- A verdict is never shown by colour alone: each has its own icon shape
  and its name.
- The fleet rows are focusable and open with Enter or Space. Rows keep their
  place and focus across updates.
- The stale-data banner changes only when the data time changes, so screen
  readers are not interrupted every second.
- Light and dark themes follow the system, with a toggle. Chart colours are
  the validated categorical palette, at most three series per chart.

## Tests

- `robotweax_srt_telemetry_http_tests` (`test_ui`): page, script, styles and
  icon with their types; the security headers; ETag and 304; HEAD; only the
  embedded paths; the host check; served without the token while the data
  stays protected; `NO_UI`; unknown flags rejected; `frame_ancestors` in the
  policy without `X-Frame-Options`, and values that would break out of the
  directive refused.
- `robotweax_srt_telemetry_ui_smoke_test` (label `ui`, needs Python 3 and
  Chrome or Chromium): runs the dashboard demo and loads the page in headless
  Chromium. Checks:
  - every endpoint is listed, worst first, following the live feed;
  - the latency advice for the stream whose RTT outgrew its latency;
  - the source view's verdict, advice, values, indicators, timeline and four
    drawn charts;
  - no `null`, `undefined` or `NaN` on the page;
  - the token gate, and opening with `#token=` without the token staying in
    the page.

An independent review attacked the page with markup and bidi characters in
every server field, malformed tokens, server restarts with a new token,
health, history and journal disabled, missing 1 s windows, and 1,000 mocked
sources. Its findings are fixed: no XSS or CSP violation, focus kept, bounded
memory, a single feed connection after a token change, and no "all clear"
while endpoints are ungraded.
