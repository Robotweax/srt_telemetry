# srt-live-transmit with SRT Telemetry

A patch for Haivision SRT v1.5.7's `srt-live-transmit` that adds one option:

```text
srt-live-transmit -telemetry <[address:]port[,options]> SOURCE TARGET
```

Each SRT side then has a live dashboard, Prometheus `/metrics`, the JSON API
and, on request, OTLP: verdict, reason and suggested knob, throughput, loss
and recovery, timing, margins, history and connection events. The options are
those of the [embedding spec](../../docs/architecture/embedding.md#spec).

```sh
# Encoder side: UDP in, SRT out (caller), dashboard on every interface with a token.
srt-live-transmit -telemetry 0.0.0.0:9810,name=cam-1,token_file=/etc/srt/token \
    udp://:5000 "srt://ingest.example.com:4201"

# Ingest side: SRT in (listener), UDP out; dashboard on localhost only.
srt-live-transmit -telemetry 9811,name=cam-1 "srt://:4201?mode=listener" udp://127.0.0.1:5001
```

Open `http://<host>:9810/` for the dashboard, scrape `/metrics` with the
[alert rules and Grafana dashboards](../../observability/README.md).

## What changes in the program

28 host lines ([the patch](srt-live-transmit-v1.5.7.patch)): 3 in
`CMakeLists.txt`, 25 in `apps/srt-live-transmit.cpp`.

- The `-telemetry` option, and a start that stops the program with an error
  when the spec is bad or the port is taken.
- Each SRT data socket is attached as soon as it exists (a caller or
  rendezvous socket while it connects, a listener's socket when accepted),
  as a sender for an SRT target and a receiver for an SRT source, and
  detached before the program drops it on reconnection. Telemetry is
  declared after the source and target, so on exit it stops before they
  close.
- Without `-telemetry` the program behaves as before.

With auto-reconnect (the default), a lost connection shows as Failing
(`connection_lost`); when it comes back, the same row continues with its
history. A caller whose peer is down or refuses it (wrong passphrase,
rejected stream ID) shows as one Failing row, with the rejection reason in
its events (once per reason, not per retry). A program that exits takes its dashboard with it: alert on
`SRTTelemetryTargetDown` for that.

## Build

SRT Telemetry first, with the public SRT adapter against the same SRT
headers, then the patched program:

```sh
cmake -S srt_telemetry -B telemetry-build -DROBOTWEAX_SRT_TELEMETRY_BUILD_PUBLIC_SRT_ADAPTER=ON \
    -DROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR=/usr/local/include -DCMAKE_INSTALL_PREFIX=/opt/srt-telemetry
cmake --build telemetry-build && cmake --install telemetry-build

git clone --branch v1.5.7 https://github.com/Haivision/srt.git
srt_telemetry/integrations/srt-live-transmit/build.sh srt /opt/srt-telemetry
./srt-live-transmit-build/srt-live-transmit -h
```

`build.sh` applies the patch (once), configures SRT with `ENABLE_APPS=ON`
and builds `srt-live-transmit`.

## Tests

CI (the Haivision SRT adapter job) counts the host lines
(`tools/count_host_lines.py`, gate 30), builds the patched program from the
installed package and runs `tools/srt_live_transmit_e2e.py`: two instances
carry a 5.3 Mbit/s UDP feed over SRT, and the test checks the sender shown
as lost before the receiver starts, the graded endpoints, labels, snapshot,
dashboard and events, the lost connection when
the receiver stops, the continued and recovered sender when it returns, a
caller refused for a wrong passphrase (one Failing row, one rejection
event), the error for a bad spec and the help text.
