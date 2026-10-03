# Embedding in an SRT application

The embedding library, `RobotweaxSRTTelemetry::embed` (header
`robotweax/srt_telemetry_embed.h`), adds SRT Telemetry to an existing SRT
application in a few lines. It runs everything the host would otherwise wire
up itself:

- the [sampler](sampler.md) with the [health engine](health-engine.md),
  history and the event journal;
- the [HTTP server](http-server.md) with the [built-in dashboard](web-ui.md),
  `/metrics` and the API;
- an optional [OTLP exporter](otlp.md);
- for each SRT socket, the statistics session and the connection monitor
  (connection state, settings, connection events).

It is built with the public SRT adapter and the HTTP server, so on Linux and
macOS. [srt-live-transmit](../../integrations/srt-live-transmit/README.md) is
its reference integration, in 28 host lines.

## Use

In a file that includes the application's `srt.h`:

```c
#include <robotweax/srt_telemetry_public_srt_host.h>

robotweax_srt_telemetry_embed *telemetry = NULL;
if (robotweax_srt_telemetry_host_embed_start("0.0.0.0:9810,token_file=/etc/srt/token",
                                             "my-encoder", &telemetry) != ROBOTWEAX_SRT_TELEMETRY_OK)
    /* report the error */;

/* Once a socket is set up (srt_connect started, or srt_accept returned): */
robotweax_srt_telemetry_embed_attach(telemetry, socket, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER, NULL, 0);
/* Before srt_close(socket): */
robotweax_srt_telemetry_embed_detach(telemetry, socket);

robotweax_srt_telemetry_embed_stop(telemetry); /* before srt_cleanup */
```

`robotweax_srt_telemetry_host_embed_start` is an inline helper in the host
header. It takes the SRT functions and the provider (Haivision SRT or
Robotweax SRT, told apart by their `srt.h`) from the host's own SRT library;
the embedding library itself never links one. Hosts that want full control
fill a `robotweax_srt_telemetry_embed_config_v1` and call
`robotweax_srt_telemetry_embed_start`.

`attach`, `detach` and `stop` accept a NULL embed and then do nothing, so the
host can call them whether or not telemetry is enabled. They are thread-safe.
The role is the media direction: `SENDER` for a socket that sends media,
`RECEIVER` for one that receives it.

## Spec

A command-line style string configures the embed
(`robotweax_srt_telemetry_embed_config_parse`), so an application needs only
one option for it:

```text
[address:]port[,option...]      9810   0.0.0.0:9810   [::]:9810,token_file=/run/srt/token
off[,option...]                 no HTTP server, e.g. off,otlp=http://collector:4318
```

| Option | Meaning |
| --- | --- |
| `name=NAME` | Label every source `stream=NAME`, the name operators know the stream by |
| `token=T`, `token_file=PATH` | Bearer token for the HTTP server; the file's first line. Prefer the file: command lines are visible to other users |
| `host=NAME` | A host name the server may be addressed by without a token; repeatable |
| `otlp=URL`, `otlp_interval_ms=N` | Push OTLP to a Collector, every N ms (default 10000) |
| `history_s=N` | History per source, default 3600 s |
| `linger_s=N` | How long a lost connection stays on the dashboard, default 60 s |
| `stream_id` | Label sources with their SRT stream ID (off by default: stream IDs can identify customers) |
| `no_ui` | Serve `/metrics` and the API, without the dashboard |

A bare port binds 127.0.0.1, `:port` every interface; `off` needs `otlp=`.
Binding every interface without a token answers
only requests addressed by IP literal, localhost or a `host=` name (DNS
rebinding protection, see [HTTP server](http-server.md#security)).

## Labels

Each source carries, in this order: the embed's labels (config `labels`),
the labels passed to `attach`, `stream` (from `name=`), `provider`, and
`stream_id` when enabled. A label the host sets wins over the automatic one
of the same key. At most 5 host labels, within the sampler's 8; they follow
the sampler's rules (keys `[A-Za-z_][A-Za-z0-9_]*`, printable values under
256 bytes, no key twice), checked at `start` and `attach`.

## Connections over time

| What happens | What the dashboard shows |
| --- | --- |
| `attach` while connecting | The source, as connecting (verdict Unknown) |
| Connected | Connected; graded as data flows |
| The peer goes away or the link breaks | Failing, reason `connection_lost`, at once (connection monitor) |
| `detach` | The source stays as lost for `linger_s`, then disappears |
| `attach` with the same role and labels during the linger | The lingering source continues on the new socket: one row, continuous history, a `connected` event; the counters restart (flagged in the windows) |
| A peer that stays down or refuses (each attempt attached, then detached) | One row, lost (Failing), until an attempt connects. The events keep the first rejection per reason; KM and settings changes count only for connected sockets, so retries do not flood the journal |

Continuation matches on role and labels only. A host with several
simultaneous connections of one role should label them apart (for example
`stream`, or `stream_id`), or a new peer can continue another peer's row.

After `detach` the embed makes no further SRT call on the socket, so the host
may close it. While a source lingers its statistics are not read
(`srt_source_up` 0, a `source_failed` event); the
[alert rules](alerting.md) leave a lost connection to `SRTStreamFailing`.

## Threading

One embed thread polls the connection monitors and ticks the sampler every
100 ms; `attach` and `detach` take the same lock, so they never race a tick.
The HTTP server and the OTLP exporter have their own threads and read the
sampler under its lock. `embed_sampler` gives access to the sampler for
thresholds, callbacks or rendering. The embed ticks it, so do not start,
stop, tick or destroy it; its callbacks run on the embed thread under the
embed's lock, so they must not call `attach` or `detach`.

`stop` stops the threads, forgets every socket (the host still closes them)
and frees the embed. Call it before `srt_cleanup`.

## Tests

- `robotweax_srt_telemetry_embed_tests` (each provider, real SRT sockets on
  loopback): spec parsing and refusals; start, port clash, missing SRT
  functions; attach and detach rules; health OK on a clean stream; `/metrics`
  with every label; connection events; the lost connection, its linger and
  its removal; a reconnection continuing the same sources; a connection
  attempt that never connects; stop with a socket attached. Clean under the
  address sanitizer; under the thread sanitizer the only reports are between
  libsrt's own threads (socket teardown in its garbage collector against
  its receive workers), with no SRT Telemetry frame.
- `robotweax_srt_telemetry_embed_package_consumer`: a C program builds and
  links against the installed package.
- `tools/srt_live_transmit_e2e.py`: the reference integration end to end.
