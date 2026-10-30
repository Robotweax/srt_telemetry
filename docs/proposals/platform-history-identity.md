# Answers to the platform history contract: sample identity, durability, live identity

Status: owner answers, docs only. Written against `main` `5ec7e44` for the
platform's history contract (`srt_network_lab` PR #360,
`docs/contracts/history-v1alpha1` section 3 rule 4 and H3, and
`docs/contracts/live-read-v1alpha1` section 6). Nothing here changes code, the
ABI or a schema. Changes proposed in this page are **options for the
maintainer**, not commitments; each public ABI or concurrency change needs the
independent review this repository requires before its PR. Costs marked
*estimate* are derived from the code and the documented sizes, not measured.

## 1. Sample identity (contract item 12)

**Confirmed, with three corrections.** `overwrittenCount + sampleIndex` is an
absolute ordinal only inside one recorder epoch, and the embed gives every
recording exactly one epoch. The corrections are about what the epoch is, how
health lines are keyed and what a reader can check from the file alone.

### What the code does

- `sampleIndex` is the sample's position in the exported file (0 for the
  oldest kept sample), for sample lines and for health lines alike
  (`sdk/src/recorder.cpp`, `serialize_record`, `serialize_health`).
- The recorder counts `pushed` (samples since creation or clear) and
  `overwritten_count` (incremented on every push into a full ring, saturating
  at `UINT64_MAX`). The first kept sample is push number `pushed − size`. While
  the count has not saturated, `overwritten_count == pushed − size`, so
  `overwrittenCount + sampleIndex` is the sample's push number since the
  recorder was created or last cleared.
- `recorder_clear()` resets the ring, `pushed`, `overwritten_count` and the
  health transitions to zero. **The embed never calls it.** It creates one
  recorder per recording at `start_recording` and never reuses it; a new
  recording always gets a new recorder and a new file name
  (`srt-telemetry-<UTC start>-<stream>-<role>[-<path>]-<64-bit random>.jsonl`).
- Saturation needs 2^64 overwrites, which at 10 samples/s is out of reach.
  It is a format edge, not an operational case.

### The recorder epoch today

| Host | Epoch boundary | Signal a reader can use |
| --- | --- | --- |
| Embed, any schema | creation of the recording (attach of a new source; with `1alpha3`/`1alpha4` also every new connection of a continued source) | the **file name**: one name, one recorder, never cleared, never reused. Every live write renames a longer version over the same name |
| Embed, process restart | new recorders, new file names | new file name; in `1alpha4` also a new `observerInstance` |
| Direct SDK host (own recorder, may call `recorder_clear`) | creation or any clear | **none in the file**. A clear followed by enough pushes produces a file whose counts look like a continuation |

So for the embed, P1's `recording_id` (server-assigned from the file name and
binding) **is** the epoch, provided the file name reaches the importer
unchanged. The file content carries no epoch: the header has no recording ID,
and the name is not covered by the content digest. A host that renames or
copies files under new names breaks that link.

### Reconnects and schemas

| Schema | Reconnect of a continued source | Ordinal across the reconnect |
| --- | --- | --- |
| `1alpha1`, `1alpha2` (default) | same recorder, same file | continues; the connection change is **not** marked in the file (only the gap in `sample_monotonic_nano`/`observed_at_unix_nano` and the health lines show it) |
| `1alpha3`, `1alpha4` | previous recording written as `FINAL`, new recorder and file | restarts at 0 in the new file; connection identity equals recording identity |

P1's condition "same connection identity" is therefore automatically true for
`1alpha3`/`1alpha4` and **not checkable** for `1alpha1`/`1alpha2`, where one
epoch can span several connections. The ordinal stays valid there; only
session semantics across the gap are unknown.

### Health lines

A health line's key is **not** just its `sampleIndex`:

- several transitions can be graded on the same sample (a tick whose sample
  failed grades the last good sample again), so the key is
  `(ordinal of its sample, position among the non-carried lines of that sample)`,
  cross-checked by the whole line's content;
- a line with `"carried": true` is the transition in force at the first kept
  sample, graded on an older, overwritten sample. It sits at `sampleIndex` 0 but
  is **not** a transition of that sample; deduplicate it by content against
  earlier revisions, never key it by ordinal;
- `healthOverwrittenCount` counts transitions dropped from the front (older
  than the one in force, or over the `2 × capacity + 2` limit). A later
  revision can have fewer old transitions than an earlier one; that is
  overwrite, not conflict.

### Checks an importer can make from the files

Merge two revisions as one epoch only if all hold; otherwise keep them apart
with overlap `unknown`, exactly as P1 proposed:

1. same file name (recording) and, for `1alpha4`, same `observerInstance`;
2. `overwrittenCount` not lower in the later revision, neither equal to
   `18446744073709551615`;
3. every ordinal present in both has identical sample lines (including
   `raw.sample_monotonic_nano`); one difference is a `conflict`;
4. the later revision's `overwrittenCount + recordCount` is not lower than the
   earlier one's.

Check 3 also catches a clear-and-refill by a direct SDK host, because samples
after the clear carry later monotonic times. Read `overwrittenCount` and
counters as unsigned 64-bit integers; parsers that map JSON numbers to
doubles lose precision above 2^53.

### Additive change that would make the epoch explicit (option, not started)

A new schema minor (`1alpha5`): the line schema is closed
(`additionalProperties: false`), so a header field cannot be added to
`1alpha4` in place. Proposed header additions:

- `recordingId`: 128 random bits as 32 lowercase hex, drawn when the recorder
  is created and again on every `recorder_clear()`, so a file names its own
  epoch and clears become visible;
- `firstSampleOrdinal`: the push number of the first kept sample, as a
  decimal string, which does not depend on the overwrite count's saturation.

Cost: one ABI addition in the recorder (epoch drawn inside the SDK, read by
the exporter), a schema version, tests, and SRT Doctor reading it. It only
helps once Doctor and srt_server move to the new schema, the same path
`1alpha4` is on (#60). Until then, the file name is the epoch.

## 2. Durability and live-write cadence (P1 finding b)

### The real durability boundary

The embed never rewrites a file on its own during a connection. It writes at
detach, at removal after the linger, at `stop`, and when the host calls
`embed_write_recording`; with `1alpha3`/`1alpha4` also when a continued
source starts a new connection (the old recording's `FINAL`). Any periodic
cadence is the host's timer.

A recording is durable on the host when the embed reports
`ROBOTWEAX_SRT_TELEMETRY_EMBED_RECORDING_WRITTEN` for it: the file was written
to a fresh temporary file, `fsync`ed, renamed over the previous version, and
the directory `fsync`ed (`adapters/public_srt/src/embed.cpp`,
`write_recording`). Two limits:

- **Known limitation:** the directory `fsync` result is ignored (and a
  directory that cannot be opened is not synced at all), yet the event still
  reports `WRITTEN`. A consumer that treats `WRITTEN` as its durability
  boundary, as the platform's history contract does, therefore has a
  boundary that holds for the file's content but not necessarily for the
  rename after a power loss. Treat `WRITTEN` as "published locally", and say
  so in the RPO; a fix (report the sync failure as `FAILED` or a separate
  flag) is a small embed change that needs the maintainer's go;
- on macOS `fsync` does not flush the drive cache.

Data since the last `WRITTEN` is lost on a crash, or everything if no write
completed. The importer should bind its digest to the bytes it uploads: open
the file inside the callback (cheap, and the next write of that recording
cannot start before the callback returns, since both run on the writer
thread), then hash and upload from that descriptor later. A later rename does
not change what an open descriptor reads.

### What a periodic write costs

A host can call `embed_write_recording` on a timer today; no SDK change is
needed. Requests for the same recording merge while one is pending, so a
slow writer never builds a backlog per source. Each write:

1. copies the whole ring under the recorder's lock (488 bytes per sample:
   2.9 MB for the default 600 s ring). The embed pushes every source's sample
   inside one tick on one thread while holding the embed lock, so **while one
   source's ring is copied, the next tick of every source waits**. The
   documented pause is a few ms at the default ring and up to about 150 ms
   (a gap-flagged window) at 36,000 s;
2. rewrites the **whole** file, not the new samples: about 1.9 kB per sample,
   11.6 MB per write at 600 s;
3. runs on the single writer thread, one recording after the other.

Write volume is therefore ring length × sources / interval
(*estimate*, from the documented sizes):

| Interval (600 s ring) | Per source | 50 sources |
| --- | --- | --- |
| 10 s | 1.2 MB/s, 6 lock pauses/min | 58 MB/s |
| 60 s | 0.19 MB/s, 1 pause/min | 9.7 MB/s |
| 300 s | 0.04 MB/s | 1.9 MB/s |

### Recommendation

- Use a cadence target of **60 s with the default 600 s ring** as the starting
  proposal. Stagger the requests across sources rather than firing them all on
  one timer edge, so that lock pauses and file writes spread out.
- The interval must stay well below the ring length, or samples are
  overwritten before any write sees them; the header's `overwrittenCount`
  growth between revisions then exceeds the samples written in between, and
  the import reports it as a gap.
- Local RPO per source is roughly interval + writer queue time (all sources
  ahead of it) + one write, and is unbounded while writes fail. Only
  `WRITTEN` events move the boundary; `FAILED` and coalesced requests do not.
- Do not shorten the interval as a way to get durability. Below about 10 s the
  full-rewrite cost dominates; a real fix is a different write path (below).
- The numbers in the table are estimates. Qualifying them (tick jitter,
  gap-flagged windows, write latency with N sources) needs a run the
  maintainer approves; none was made for this page.

### Options beyond cadence (maintainer decision, not started)

- **Copy without stalling the tick**: copy only samples pushed since the last
  write, or copy in slices, so the recorder lock is held briefly. Concurrency
  change, needs independent review.
- **Append-only spool**: write each new sample to a per-recording append file
  and seal it at `FINAL`. Gives an RPO of seconds without rewrites, but is the
  streaming spool the [embedding](../architecture/embedding.md#recordings) page
  lists as open, with its own crash-recovery and retention rules.

## 3. Identity in the compact HTTP path (live read)

### Today

| Read | Identity it carries |
| --- | --- |
| `GET /api/v1/snapshot` | per source `id`, `role`, `labels`, `up`, `connection` |
| `GET /api/v1/sources/{id}/history` | `source_id` only |

`sessionRef` and the observer live in the embed's recordings; the HTTP server
reads the sampler and sees neither, and the embedding page states that the
observer is not exported to the snapshot. Two consequences for the live read:
source IDs restart at 1 in a new process, so after a restart the same ID can
name a different source; and a continued source keeps its ID across
reconnections, so neither read shows a new connection.

### What could be added without breaking `srt-telemetry.snapshot.v1`

New fields only, nothing renamed or removed. P1's reader and the built-in UI
must ignore unknown fields (the UI does today; P1's contract should say so).

| Field | Where | Meaning | Note |
| --- | --- | --- | --- |
| `observer: {node, instance}` | top level of snapshot and history responses | the embed's `set_observer_v1` values, `null` when unset | answers "was there a restart": a different `instance` means a new process, so `read_generation` can be bumped on proof rather than inference. Opt-in, because it exposes the node name to every token holder |
| `connection_seq` | per source in snapshot, top level in history | counter that increases each time a continued source gets a new connection | makes reconnects visible; with `1alpha3`/`1alpha4` it matches the recording boundary |
| `connected_at_unix_ms` | per source in snapshot | current connection's start, `null` while lost | same derivation as the `1alpha3` header |
| `session_ref` | per source in snapshot, top level in history | the host's `embed_set_session_ref`, `null` when unset | never derived; same value the recording carries |

`flowId` and `streamId` need no new field: the embed derives a recording's
`streamId` from the source's `stream` label (the embed's `name=`) and its
`flowId` from the `flow` label (else the stream), and the snapshot already
returns both labels. They are a join hint, not a key: `flowId` repeats across
observer instances (srt_server's `session_id` restarts at 1), and member rows
append `/<path>` to the recording's `flowId`.

History records span connections of a continued source. Marking the
boundary per record needs a field in `compact_record_v1`; no release tag
exists, so v1 may still grow, but it is a larger ABI change and not needed if
the server compares `connection_seq` between reads.

Implementation shape: the embed owns the identity, the sampler does not, so
either the sampler gains a per-source identity setter that the renderers read,
or the HTTP server takes an identity callback from the embed. Both are public
ABI changes and need independent review; neither is started.

Until then P1's live-read text is right as written: `session.session_ref` is
`null`, `observer.instance` is `null` unless a host sets a label, and a
restart between two reads can go unseen.

## 4. Metric ids and `FINAL`

- **Metric ids** (`srt.connection.rtt`, ...) are versioned by
  `ROBOTWEAX_SRT_TELEMETRY_METRIC_SCHEMA_VERSION`, currently 1, returned as
  `metric_schema` in every JSON snapshot
  ([metric names](../architecture/metric-names.md)). The rule is that a name
  never changes meaning; a change gets a new name and a new schema version.
  They are independent of the recording schema: recordings carry raw and
  delta field names, not catalog ids, so `1alpha4` says nothing about them.
  A live reader should require `metric_schema == 1` and treat any other value
  as incompatible. No release tag exists yet, so "released" is not yet a
  formal state; I will treat the 32 ids at `5ec7e44` as frozen for the
  platform and announce any addition.
- **`FINAL`** is set on exactly one recording event per recording, when the
  source is removed after its linger or at `stop`, and the embed never writes
  that file again. Exceptions: running out of memory while queueing the final
  request loses that event (counted in the failure counter); a source whose
  attach failed gets no events; a crashed or killed process sends none, which
  P1's `abandoned_unconfirmed` state already covers. `FINAL` can arrive with
  `WRITTEN`, `UNCHANGED` or `FAILED`; only `FINAL | WRITTEN` and
  `FINAL | UNCHANGED` (after an earlier `WRITTEN`) mean the file on disk is
  the last complete version; `FINAL | EMPTY` means the source never sampled
  and no file exists.

## 5. What the maintainer decides

- Whether to add the `1alpha5` epoch fields (section 1) and when.
- The live-write cadence proposal (60 s, default ring) as a starting point,
  and whether to approve a run that measures it.
- Whether to stop ignoring the directory `fsync` result.
- Whether the HTTP reads expose observer, connection sequence and session
  reference (section 3), and whether the observer is opt-in.
