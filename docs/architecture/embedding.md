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
off[,option...]                 no HTTP server, e.g. off,otlp=http://collector:4318 or off,jsonl=/var/lib/srt
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
| `jsonl=DIR` | Write a [recording](#recordings) of every source into this existing directory |
| `jsonl_s=N` | Seconds of 100 ms samples each recording keeps, default 600, at most 36000 |
| `provider_revision=REV` | Source revision of the SRT library (e.g. a commit) for the recording header |

A bare port binds 127.0.0.1, `:port` every interface; `off` needs `otlp=` or
`jsonl=`.
Binding every interface without a token answers
only requests addressed by IP literal, localhost or a `host=` name (DNS
rebinding protection, see [HTTP server](http-server.md#security)).

## Labels

Each source carries, in this order: the embed's labels (config `labels`),
the labels passed to `attach`, `stream` (from `name=`), `provider`, and
`stream_id` when enabled, and for connection groups `group` and `path` (see
below). A label the host sets wins over the automatic one of the same key,
except `group` and `path`, which only the embed sets. At most 5 host labels,
within the sampler's 10; they follow
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

## Connection groups

`robotweax_srt_telemetry_embed_attach_group` monitors a Broadcast or
Main/Backup group and all its members, with the
[group session](public-srt-group.md):

```c
robotweax_srt_telemetry_embed_group_v1 g;
robotweax_srt_telemetry_embed_group_default(&g);
g.group = group;                                  /* srt_create_group, or srt_accept */
g.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
g.group_type = ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP;
robotweax_srt_telemetry_embed_attach_group(telemetry, &g);
...
robotweax_srt_telemetry_embed_detach(telemetry, group);  /* before srt_close(group) */
```

The embed config needs `group_data` and `getsockname`;
`robotweax_srt_telemetry_host_embed_functions` fills them.

**Rows.**
- One row for the group, labelled `group=backup` or `group=broadcast`. Its
  counters are the group's own: unique packets, and what the application
  lost.
- One row per member path, also labelled `path=<name>`. A path gets its row
  when the member first connects.
- Every tick samples the group and all members once with the tick's
  timestamps, so the rows are aligned in time.
- Path names come from the host's `path_name` callback, else the caller's
  token (`token-1`), else the addresses (`local>peer`); see
  [path names](public-srt-group.md#path-names).
- Exports: the group row has `srt_group_usable_paths` and
  `srt_group_expected_paths`, each path row `srt_health_path_verdict` (its
  own grade); see [metric names](metric-names.md#health). The built-in UI and
  the Grafana dashboards show a group as one endpoint with its paths.

**Path names** must be unique within a group. A host that names its paths
(for example by interface or peer address) passes a callback:

```c
static int name_path(void *user, const robotweax_srt_telemetry_group_member_identity_v1 *m,
                     char *name, size_t capacity)
{
    (void)user;
    if (strncmp(m->peer_address, "10.1.", 5) == 0) { snprintf(name, capacity, "lte-1"); return 1; }
    if (strncmp(m->peer_address, "10.2.", 5) == 0) { snprintf(name, capacity, "sat"); return 1; }
    return 0; /* automatic name */
}
g.path_name = name_path;
```

The callback runs on the embed's thread under its lock, once per member
socket, when the member first connects. If a second socket appears on a path,
the row keeps its socket while that socket is usable (or while the new one is
not connected yet), and the new socket takes the row once the old one is
broken or gone. That happens with duplicate names, and with a reconnect under
the same name before the broken member leaves the list; Robotweax SRT keeps
broken members listed until they are closed.

**Negotiated path labels** (Robotweax SRT 0.2.9 or later, not 0.2.8): the caller labels each path in the handshake, so the accepting side
can use the same names without host code. The host enables labels on both
peers and passes the provider's getter to the embed:

```c
/* both peers */
srt_setsockflag(group_or_listener, SRTO_ROBOTWEAX_PATHID_REQUIRED, &yes, sizeof yes);
/* caller, per endpoint */
srt_config_add(endpoint.config, SRTO_ROBOTWEAX_PATHID, "lte", 3);

robotweax_srt_telemetry_host_embed_functions(&config);
robotweax_srt_telemetry_host_embed_path_data(&config); /* sets group_path_data */
```

A labelled path is named `pathid-<hex of the label>` (`pathid-6c7465` for
`lte`) on both sides, unless the host's `path_name` gives a name. The helper
exists only when `srt.h` declares the getter (`ROBOTWEAX_SRT_PATHID_NEGOTIATED`).
`group_path_data` with any provider other than Robotweax SRT is refused at
`embed_start`. Passing the getter is the host's qualification of
`CAP_GROUP_PATH_IDENTIFIERS_V1`: take it from the library that owns the
sockets, at a revision with the protocol; the group sessions then carry the
capability. See [path names](public-srt-group.md#path-names) for the order
and the pending state while a label is not negotiated yet.

**Health.**
- A member path is capped at Watch while the group has another usable path:
  losing or impairing one path is what a group is built to survive. The
  reason and advice stay, and the snapshot carries `capped` and the path's own
  `uncapped_verdict`.
- The last usable path carries the stream alone and is graded uncapped, so
  its problems alert like a single socket's.
- A connected Main/Backup member other than the active one is OK with reason
  `standby`. It carries only keepalives, so there is nothing to grade, except
  its RTT when that is measured: with the optional RTT metadata enabled
  (`embed_set_rtt_data_v1`), a standby whose latest accepted RTT observation
  is at most 1 s old gets `HEALTH_CONTEXT_RTT_MEASURED`, and a latency too
  short for that RTT grades it (capped at Watch) before a failover. The
  snapshot marks a waiting path with `health.standby` either way. Without the
  metadata (Haivision, older Robotweax) the standby stays OK / `standby`.
- The group's own row combines its counters (what a receiving application
  lost), the grade of the path carrying the stream (Backup: the active one;
  Broadcast: the best), and its redundancy, from the paths' statuses one tick
  earlier. Fewer usable paths than expected is Watch (`path_lost`); the last
  usable one of several is At risk (`redundancy_lost`); no usable path is
  Failing. See [Health engine](health-engine.md#connection-groups).
- Paths are counted by name, not by socket: a replacement that overlaps the
  old member on the same path is one path.
- Expected paths: `expected_paths` in `embed_group_v1`, or, with 0, the most
  named paths seen usable at once since attach (also since a re-attach that
  continues the row). Set it when a group may start with a path down, or when
  paths are removed on purpose (the automatic value never goes down).
- The carrying path's grade: Backup takes the active path, Broadcast the best
  usable path. A path waiting as standby never counts, and while a usable
  path is still ungraded, a Broadcast group takes no path grade worse than
  OK (the ungraded one may be delivering).
- A broken group reads lost (Failing) through the connection monitor on the
  group handle.
- Before its first connection a group has no statistics. Its row then gets
  empty samples rather than failed reads, so it shows as connecting and
  `SRTSourceStatisticsFailing` stays quiet.

**Events**, with the tick's timestamps:

| Event | On | Values |
| --- | --- | --- |
| `member_added` | group row | code = member socket, detail `path=<name>` |
| `member_removed` | group row | code = member socket, detail `path=<name>` |
| `member_state_changed` | member row | previous / current member state (`SRT_MEMBERSTATUS`), code = member socket |
| `active_member_changed` | group row | Backup: previous / current member socket, detail `path=<a> -> <b>`; Broadcast: detail `carrying=<paths>` |
| `connected` / `disconnected` | group row | the group handle's state |

**Over time.**
- A member that leaves the group stays on the dashboard as a lost path (Watch)
  for `linger_s`.
- A new member on the same path (same name) continues the path's row: one
  history per path across reconnects. That holds for a caller that reconnects
  with the same token, or with host names.
- On the accepting side SRT reports no token, so a reconnected member that
  arrives from a new port gets a new, address-named row unless the host names
  its paths.
- `detach` of the group takes its member rows along, each as a lost row, and
  a group attached again with the same role and labels during the linger
  continues the group's row.

## Recordings

With `jsonl=DIR` (config `recording_directory`) every source keeps its 100 ms
samples (raw statistics and deltas) in a [bounded recorder](bounded-recorder.md)
ring of `jsonl_s` seconds and is written as one file in the recorder's
versioned JSONL format ([line schema](../../schema/telemetry-jsonl-line.schema.json)),
the format SRT Doctor and the Flight Recorder read:

- by the embed's writer thread, never on the sampling thread or the host's
  thread: when the socket is detached (a host may exit right after it, so
  call `stop`, which waits for every write), again when the source is removed
  after the linger, and at `stop`. Each write streams into a new temporary
  file (`mkstemp`) in the same directory, syncs it, renames it over the
  previous version and syncs the directory, so a reader never sees a partial
  recording; on Linux a completed write normally survives a power loss too
  (macOS `fsync` does not flush the drive cache). Files get mode 0644 within
  the process umask read at `start`. A reconnection that
  continues the source continues its recording. A write that finds no new
  sample since the source's last written version leaves the file as it is
  (a lingering source takes no samples), so a file the host pruned after the
  detach write does not come back at the end of the linger or at `stop`;
- on request while the source goes on: `robotweax_srt_telemetry_embed_write_recording(embed, socket)`
  queues the same write a detach makes (for a group: the group and every
  current member) and returns at once. Later writes replace the file with longer
  versions, so one session stays one recording;
- as `srt-telemetry-<UTC start>-<stream>-<role>-<random>.jsonl`: sorted by
  start, unique also across containers and hosts that share a directory;
- with header `streamId` from the `stream` label (`name=`), `flowId` from a
  `flow` label (else the stream), the role and provider, `providerVersion`
  from the host's `srt.h` (`robotweax_srt_telemetry_host_embed_functions`),
  `providerRevision` from `provider_revision=`, `buildProfile` `embed`, and
  `capacity`/`overwrittenCount` when the ring was full;
- not at all for a source that never produced a sample (an attempt that never
  connected).

Label both ends of one stream with the same `flow` so a sender and a receiver
recording are recognised as one path. The ring holds 472 bytes per sample,
allocated when the socket is attached: 600 s (6,000 samples) is 2.8 MB per
source, the maximum 36,000 s is 170 MB. A written sample takes about 1.9 kB of
JSONL, so 600 s make an 11.6 MB file; a write holds a copy of the ring and
64 KiB of text, not the file. That copy is taken under the recorder's lock,
which the next sample waits for: a few ms at the default, up to about 150 ms
(a gap-flagged window) for a 36,000 s ring, as does allocating such a ring at
attach. Keep rings short where every 100 ms sample matters, and do not request
live writes of long rings more often than such a pause is acceptable.
`robotweax_srt_telemetry_embed_recordings` counts file writes done and failed
(unchanged, skipped writes are not counted).

`robotweax_srt_telemetry_embed_set_recording_callback` reports every handled
write on the writer thread, in order per source (requests still waiting for
the same recording merge into one write and one event):
`embed_recording_event_v1` carries the `source_id`, the file path, the samples
in the file and flags `WRITTEN`, `UNCHANGED`, `FAILED` or `EMPTY` (no sample
yet, no file). `FINAL` is set on exactly one event per recording, when the
source is gone (removed after its linger, or at `stop`, which returns after
the event): the embed never writes that file again, so a host can prune,
pack or analyse it without waiting for the linger to pass a second time. A
live-write request reports without `FINAL` unless it merged with the
source's removal. Only running out of memory while queueing the final
request loses that event (the failure counter counts it); a source whose
attach failed gets no events. The callback must return quickly and must not
call `embed_stop` or `set_recording_callback`; other embed calls are allowed,
and once `stop` has begun `attach`/`write_recording` return `INVALID_STATE`
and `detach` does nothing. Clearing or replacing the callback waits for a
running call, so its user data may be freed afterwards; with no callback set,
events are dropped. The
directory must exist and be writable at `start` (else `IO_FAILURE`); nothing
removes old files, so retention is the host's job. Recordings keep only the
common v1 statistics, not the provider-specific extended fields, and samples
not yet written are lost if the process crashes; a streaming disk spool
remains open.

## Threading

One embed thread (named `srt-tel-embed`; the writer is `srt-tel-writer`, the
HTTP server `srt-tel-http`, the OTLP exporter `srt-tel-otlp`, so none shows
the name of the host thread that started it) polls the connection monitors
and ticks the sampler every 100 ms; `attach` and `detach` take the same lock, so they never race a tick.
The HTTP server and the OTLP exporter have their own threads and read the
sampler under its lock. With recordings a writer thread writes the files; it
takes the recorder's snapshot without the embed's lock. `embed_sampler` gives access to the sampler for
thresholds, callbacks or rendering. The embed ticks it, so do not start,
stop, tick or destroy it; its callbacks run on the embed thread under the
embed's lock, so they must not call `attach` or `detach`.

`stop` must not overlap `attach` or `detach` on another host thread (the
recording callback, which `stop` waits for, may call them; see above). It stops the
threads, waits for the final recordings, forgets every socket (the host still closes them)
and frees the embed. Call it before `srt_cleanup`.

## Tests

- `robotweax_srt_telemetry_embed_tests` (each provider, real SRT sockets on
  loopback): spec parsing and refusals; start, port clash, missing SRT
  functions; attach and detach rules; health OK on a clean stream; `/metrics`
  with every label; connection events; the lost connection, its linger and
  its removal; a reconnection continuing the same sources; a connection
  attempt that never connects; stop with a socket attached; recordings:
  written after detach and rewritten at stop, one complete file per source
  with header metadata, a ring that overwrites and counts its oldest samples,
  a linger expiry written by the embed thread, a reconnection continuing one
  recording, no file for a socket that never sampled, a path-like stream
  label kept inside the directory, a removed directory counted as a failed
  write, and a zeroed config. Clean under the
  address sanitizer; under the thread sanitizer the only reports are between
  libsrt's own threads (socket teardown in its garbage collector against
  its receive workers), with no SRT Telemetry frame.
- `robotweax_srt_telemetry_embed_package_consumer`: a C program builds and
  links against the installed package.
- `tools/srt_live_transmit_e2e.py`: the reference integration end to end.

Group `active_member_changed` details begin with `inferred; authority=unavailable; `;
packet-counter inference does not prove a sender promotion. The UI labels these
events **Observed data path changed**. No event identifier or binary layout changes.


## Optional Standby RTT metadata

After qualifying the selected provider build, opt in independently:

```c
#if defined(ROBOTWEAX_SRT_STANDBY_RTT_VERSION) && ROBOTWEAX_SRT_STANDBY_RTT_VERSION == 1
robotweax_srt_telemetry_host_embed_standby_rtt_data(embed);
#endif
```

This helper is intentionally absent from the ordinary configuration helper.
Alternatively inject the exact compatible getter using
`robotweax_srt_telemetry_embed_set_standby_rtt_data_v1`; the library must remain
loaded. Null disables and clears observations. The setter serializes with
sampling and must not run from callbacks. Each group member is polled separately;
group aggregate rows and standalone sockets are not read and remain unavailable
(SRT negotiates Standby RTT only on Backup group members). A member without
negotiated monitoring reads available with `negotiated=false`. The built-in UI
shows the Standby RTT tile and paths column only where monitoring was
negotiated (or a read failed).

The independent metadata and snapshot JSON carry a single measured RTT, its
age/freshness and **last attempt** outcome. Neither `MEASURED` nor `PAUSED`
establishes current monitoring eligibility. These observations do not feed the
existing RTT health flag or estimator, DATA health, send authority, windows,
Prometheus or OTLP metrics. See the [field contract](srt-stats-field-map.md#optional-standby-rtt-observation)
for null ages, lifecycle, capacity and freshness boundaries.
