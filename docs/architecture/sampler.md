# Sampler

The sampler is the part a product actually runs. It samples every registered
SRT socket at a fixed period, derives the [dashboard metrics](metrics-and-windows.md),
keeps per-socket time windows, and delivers samples and closed windows
through callbacks. Header: `robotweax/srt_telemetry_sampler.h`.

```c
robotweax_srt_telemetry_sampler_config_v1 config;
robotweax_srt_telemetry_sampler_config_default(&config);   /* 100 ms, 1 s/10 s/60 s */
config.callbacks.on_window = my_window_callback;
robotweax_srt_telemetry_sampler *sampler;
robotweax_srt_telemetry_sampler_create(&config, &sampler);

/* per SRT socket, using the public SRT adapter session */
const robotweax_srt_telemetry_label_v1 labels[] = {{"site", "studio-2"}, {"channel", "news"}};
robotweax_srt_telemetry_source_config_v1 source;
robotweax_srt_telemetry_public_srt_source_config(session, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER,
                                                 labels, 2, &source);
uint64_t id;
robotweax_srt_telemetry_sampler_add_source(sampler, &source, &id);
robotweax_srt_telemetry_sampler_start(sampler);
...
robotweax_srt_telemetry_sampler_remove_source(sampler, id);  /* before destroying the session */
```

## Sources

A source is a callback that fills one sample (`source_output_v1`: v1 sample
and delta, optionally the extended sample and delta). The core never links SRT.
The public SRT adapter provides `robotweax_srt_telemetry_public_srt_source_sample`
and `robotweax_srt_telemetry_public_srt_source_config`, which make a session a
source with one `srt_bistats` call per tick. Any other statistics source, such
as a recorded file or a different transport, can implement the same callback.

Each source has a role and up to 8 labels. Keys are `[A-Za-z_][A-Za-z0-9_]*`,
at most 63 bytes. Values are printable ASCII, at most 255 bytes. Labels are
copied when the source is added and passed to every callback; exporters will
use them as metric labels.

## Running it

- `sampler_start` runs an internal thread. Ticks are scheduled on the steady
  clock at multiples of the period. If a tick overruns, the missed ticks are
  skipped, not bunched; the windows then flag `SAMPLING_GAP`.
- `sampler_tick` samples once with timestamps the host supplies, for hosts with
  their own loop and for deterministic tests. It is refused while the thread
  runs.
- `sampler_stop` joins the thread and keeps windows open; a later `start`
  continues them. Concurrent `stop` calls are safe: one joins and the others
  return at once. `start` is refused until the join has finished.

## Failing sources

When a source fails (for example the socket closed before the host removed
it), `on_error` fires at once for a new error code and at most once per second
while the same error repeats. The source's windows keep advancing on an empty
sample, so the outage shows as windows flagged `SOURCE_ERROR` with no metrics,
not as silence followed by one stale window.

## Callbacks and threading

`on_sample` (every sample, with the metrics), `on_window` (each closed window),
and `on_error` (a source failed; other sources continue). Callbacks run on the
sampling thread while the sampler holds its lock:

- they must not call functions of the same sampler; such calls return
  `INVALID_STATE` instead of deadlocking. Driving a different sampler from a
  callback is allowed, and the guard tracks nesting per sampler;
- they must not throw; exceptions are swallowed at the C boundary;
- the host must not hold a lock that a callback takes while it calls
  `remove_source`, `stop`, `tick` or `destroy`.

`destroy` must not run concurrently with other calls on the same sampler. A
`destroy` from one of the sampler's own callbacks is ignored, because it would
deadlock.

`add_source` and `remove_source` are thread-safe. `remove_source` waits for a
running tick, flushes that source's windows as `PARTIAL`, and guarantees the
source callback is never called again. That is the point at which the host may
destroy its adapter session and close the SRT socket, preserving the adapter's
session-before-socket lifecycle rule. `destroy` stops the thread and flushes
every source.

## Tests

- `robotweax_srt_telemetry_sampler_tests` (core, fake sources): labels copied
  and delivered, exact windowed rates from known counters, per-source errors,
  removal flushing `PARTIAL` windows and stopping calls, `NOT_FOUND`,
  re-entrant calls refused (also with nested samplers), label validation,
  failing sources producing flagged windows with rate-limited errors, and the
  real thread: period, 20 start/stop rounds with four concurrent stoppers,
  manual tick refused, removal while running, prompt stop. Also run under
  ThreadSanitizer and AddressSanitizer/UBSan with leak checking.
- `robotweax_srt_telemetry_public_srt_sampler_test` (live, each provider): a
  caller streams about 300 packets/s to a listener for 1.5 s while the thread
  samples both ends at 50 ms with 500 ms windows. Sender and receiver windows
  report about 300 packets/s and recovery headroom close to 1, with no source
  errors.
