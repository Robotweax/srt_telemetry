# History and events

Two stores turn the sampler's output into something a dashboard can scroll
back through without a backend. Header: `robotweax/srt_telemetry_history.h`;
sampler integration in `robotweax/srt_telemetry_sampler.h`.

## Compact history

`compact_record_v1` stores one window of one socket in 112 bytes:

| Group | Fields |
| --- | --- |
| Identity | window start, window length, sample count, window quality flags, availability bits |
| Throughput | unique sent, retransmitted and unique received bit/s (window rates); link capacity (mean) |
| Path | RTT mean and max |
| Margins | recovery headroom min and mean, send buffer max, receive buffer min, send buffer / drop threshold max, flight / flow window max |
| Counts (window totals) | sender loss, receiver loss, retransmitted, received retransmitted, sender drop, receiver drop, undecryptable, belated, FEC recovered, FEC unrecovered |

Floats keep about 7 significant digits; counts saturate at 2³² − 1.

`sampler_enable_history(sampler, n)` keeps a ring of `n` records per source,
one per closed window of the shortest configured window (1 s by default).
`n = 86,400` holds 24 h at about 9.7 MB per source, so any product embedding
the SDK can offer a 24 h statistics download without a backend.
`sampler_history_read(sampler, id, since, ...)` returns records in the order
their windows closed, which is time order unless the wall clock stepped back.
It cannot be called from the sampler's own callbacks (`INVALID_STATE`).
A source's history is discarded when it is removed. A host that re-adds a
socket after every reconnect should read the history out first if it wants
to keep it.

## Event journal

`event_journal` is a bounded, thread-safe ring of `event_v1` entries:
timestamps, source ID, kind, severity, previous and current value, a
kind-specific code, and a short printable detail. Each accepted event gets the
next sequence number. A reader keeps the last sequence it has seen and asks for
anything after it, which is how a live UI will poll without missing or
repeating events. A cursor older than the oldest retained event reads from the
oldest one, and the overwritten count says how many were lost. When full, the
oldest events are overwritten and counted. Unknown kinds or severities are
rejected; the detail is kept up to its terminator, with control characters
replaced and the rest zeroed.

With `sampler_enable_journal(sampler, capacity)` the sampler keeps its own
journal, read with `sampler_journal_read` from any thread. Replacing or
re-enabling the journal drops its events but continues the sequence numbers,
so readers' cursors stay valid. The sampler records:

| Event | When | Severity |
| --- | --- | --- |
| `source_added` | a source is registered; detail lists its labels, code holds its role | info |
| `source_removed` | a source is removed | info |
| `source_failed` | a source starts failing, or fails with a different error; code holds the native result | error |
| `source_recovered` | a failed source has sampled without failure for 1 s | info |
| `sampling_gap`, `clock_step`, `counter_reset` | a window of the shortest length carries a flag the previous one did not; code holds the window start | warning |

Hosts add their own events with `sampler_record_event`, which is safe from any
thread, including callbacks. For connection changes, the adapter converts a
connection-monitor poll into events with
`robotweax_srt_telemetry_public_srt_connection_events`:

| Event | Values | Severity |
| --- | --- | --- |
| `connected` | previous / current socket state | info |
| `disconnected` | previous / current socket state | warning |
| `rejected` | current = `SRT_REJ_*` reason | error |
| `km_state_changed` | previous / current KM state | warning for no secret, bad secret or bad crypto mode, else info |
| `settings_changed` | code = changed settings fields | info |
| `peer_version_changed` | current = peer version | info |

`verdict_changed` is reserved for the health engine.

Events describe episodes, not ticks:

- A failing source produces one `source_failed` and, once it has been healthy
  for a second, one `source_recovered`. A source that fails every other tick
  stays one open failure instead of filling the journal. Statistics that
  cannot be turned into metrics count as a failure too.
- A backward clock step flags two windows (the one it closes and the next) but
  produces one `clock_step`. A long overload produces one `sampling_gap`.
- Window events are stamped with the time the window closed, so they sort with
  the events around them.
- The interval that spans an outage shows as a `sampling_gap`, not as a clock
  step.

## Tests

- `robotweax_srt_telemetry_history_tests`: compact conversion (values,
  availability, saturation, 112-byte layout); journal ring (sequences,
  overwrite, read-after including stale cursors, kind and severity
  validation, detail truncation and zeroing, 4 concurrent writers); sampler
  history ring and time filter; source added/failed/recovered/removed, gap and
  host events; sequence continuity across journal replacement; a flapping
  source; a backward clock step. Also clean under TSan and ASan/UBSan.
- `robotweax_srt_telemetry_public_srt_connection_tests`: poll results converted
  to connected, KM warning, settings, disconnected and rejected events.
- `robotweax_srt_telemetry_public_srt_sampler_test` (live, each provider): the
  receiver's history holds its 500 ms records at about 3.3 Mbit/s. When the
  caller closes, the connection monitor's disconnect lands in the sampler
  journal next to both `source_added` events.
