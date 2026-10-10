# Connection groups (group session)

The group session observes one SRT connection group, Broadcast or Main/Backup,
together with all of its member sockets. One call per sampling tick reads the
group and every member, so they are linked and timestamped alike. Header:
`adapters/public_srt/include/robotweax/srt_telemetry_public_srt_group.h`.

This is part A of group support: the adapter. Part B builds on it in the
embedding layer (sources, labels `group`/`path`, journal events, member paths
capped at Watch, standby paths OK; see
[connection groups](embedding.md#connection-groups)). The group verdict from
its paths and the group panels (part C) and the Network Lab's multi-path
qualification (part D) follow.

## What one call reads

`robotweax_srt_telemetry_public_srt_group_session_sample` makes, through the
functions the host injects:

| Call | Count | Gives |
| --- | --- | --- |
| `srt_getsockstate(group)` | 1 | `group_state` |
| `srt_group_data(group)` | 1; up to 3 when the group outgrew the session's buffer | member list: socket, `sockstate`, `memberstate`, `result`, `weight`, `token`, peer address |
| `robotweax_srt_group_path_data_v1(group)` | optional; 1, up to 3 bounded growth attempts | negotiated path labels, joined to the ordinary snapshot by member socket ID |
| `srt_bistats(group)` | 1 | group counters (the group field masks) |
| `srt_bistats(member)` | 1 per member | the member's own counters |
| `srt_getsockname(member)` | on every call until a read succeeds while the member is CONNECTED | local address |

Statistics are read with `clear=0` and `instantaneous=1`, as in the
statistics session. The group fields are limited to
`ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_CANDIDATE_FIELDS` and
`..._GROUP_EXT_CANDIDATE_FIELDS`. Member fields follow the same rules as a
`config_v2` statistics session for the provider. The config must carry
`CAP_GROUP_MEMBERS_V1`; it is copied into every group and member sample, so it
is set exactly when a group session produced the sample.

The group type is declared by the host (`group_type`): Robotweax SRT 0.2.8
refuses `SRTO_GROUPTYPE` on an accepted group, while Haivision SRT returns it.

`<robotweax/srt_telemetry_public_srt_host.h>` fills the injected functions
(`robotweax_srt_telemetry_host_group_functions`). Haivision SRT built without
`ENABLE_BONDING` links the same symbols; `srt_group_data` then fails, and so
does the sample.

The session clears its member buffer before every `srt_group_data` call.
Haivision SRT lists a connecting member before it knows the peer and leaves
`peeraddr` unwritten for it; without the clear, such a member would show the
peer of whichever member used that slot before. The local address is read
again until the member is CONNECTED, because a connecting member is bound to
the wildcard address.

## What one call returns

The result carries the group: `group_state`, counters, deltas, flags,
`listed_member_count`, `usable_member_count` and `active_member`. The member
entries go into an array the caller supplies (`members`, `member_capacity`,
`member_entry_size`), so the group size has no fixed limit and the result
itself stays small. The entries come in this order:

1. Members removed since the previous call. Each is reported exactly once; if
   the array is full, it stays pending until a call has room (at most 1024).
   A member listed again before its removal was reported simply continues as
   the same member (same path name, counters continue).
2. The listed members, in provider order.

`required_member_count` says how many entries were due. When they did not
fit, the call returns `BUFFER_TOO_SMALL` and sets `GROUP_TRUNCATED`. The
result is still valid apart from the missing entries. Listed members that did
not fit are not sampled at all, so their changes, `MEMBER_NEW` and deltas carry
over: a host can grow its array and lose nothing. With `member_capacity` 0 the
call reports the group only and keeps no removals.

`ALLOCATION_FAILURE` leaves `member_count` at 0. The session commits what later
calls depend on only at the end of a successful call: reported removals,
announced members, member state changes and path names not yet handed out, the
group state and the first listing. The next call therefore reports all of them
again; only that tick's deltas are lost.

| Flag | Meaning |
| --- | --- |
| `MEMBER_NEW` | first result that reports this socket |
| `MEMBER_REMOVED` | no longer listed; the entry carries its last values and is reported exactly once |
| `MEMBER_STATE_CHANGED` | `member_state` or `socket_state` changed |
| `MEMBER_CARRYING` | stream packets advanced in this tick (per direction: the unique total if qualified, else the plain total) |
| `MEMBER_STATS_STALE` | `srt_bistats` failed now; counters are the last good ones, no delta |
| `MEMBER_STATS_NONE` | `srt_bistats` never succeeded for this socket |
| `MEMBER_USABLE` | socket CONNECTED and member not BROKEN |
| `MEMBER_PATH_ASSIGNED` | the path name was set in this call |
| `MEMBER_PATH_PENDING` | an unnamed connected member has missing, failed, malformed or provisional optional metadata; its path is empty and retried, while counters still sample |
| `GROUP_STATS` | `srt_bistats` on the group succeeded (it fails before the first connection) |
| `GROUP_FIRST_SAMPLE` | first call that listed the members (a failed listing does not count); no member or active change is reported |
| `GROUP_MEMBERS_CHANGED` | this result reports a REMOVED entry, or a NEW entry after the first listing (never with `member_capacity` 0) |
| `GROUP_MEMBER_STATE_CHANGED` | any member's state changed |
| `GROUP_ACTIVE_CHANGED` | Backup: `active_member` changed; Broadcast: the carrying set changed; never for an unknown group type |
| `GROUP_STATE_CHANGED` | `group_state` changed |
| `GROUP_TRUNCATED` | more entries than the array holds; see `required_member_count` |

A failing `srt_group_data` (closed group, no bonding) returns
`PROVIDER_SAMPLE_FAILURE` with `group_state` and the native results filled.

## Which member carries the stream

`memberstate` is not a reliable answer, for three reasons:

- Both providers update it only inside the application's send and receive
  calls.
- Changes shorter than one tick are invisible.
- The two providers disagree on the receiving side (table below).

The session therefore derives activity from the counters. A member *carries*
when its stream packets advanced since the previous call. For each direction
the unique total counts when it is qualified, else the plain total.

- **Main/Backup.** `active_member` is the carrying member with the most stream
  packets in the tick; a tie keeps the current one. A better-weight candidate
  probed in parallel therefore does not count as a switch until it carries
  more.
  `active_member` keeps naming a removed member until another member
  carries.
- **Broadcast.** The carrying set is tracked instead; `active_member` stays −1.
- **Unknown type.** Members are still marked carrying, but no active changes
  are reported.
- **Idle ticks.** A tick in which no member carries (an idle stream) changes
  nothing.

The reported `member_state` is still passed through unchanged for display.

## Path names

A path is named once while the member is CONNECTED, and the name never changes.
The ordinary sources retain their existing behavior. When the host explicitly
enables the optional path getter, the sources, in order, are:

1. The host's `path_name` callback: 1–159 printable ASCII characters without
   space, NUL-terminated. A valid host name has priority over peer labels.
2. A negotiated caller label: `pathid-` followed by lowercase hexadecimal of
   every byte, with `PATH_SOURCE_IDENTIFIER`. Embedded NUL and non-text bytes
   are represented without truncation or interpretation. A 32-byte label uses
   71 characters. Labels do not replace the local token.
3. `token-<n>` from the caller's `SRT_SOCKGROUPCONFIG.token`.
4. `<local>><peer>` addresses, with `?` for an unknown side.

On older Robotweax and Haivision providers the optional getter is unavailable;
the host leaves it disabled and naming follows host → token → addresses.
Robotweax development `main` after protocol PR #298 offers the versioned getter;
published v0.2.8 artifacts do not. Configure labeled transport in the host,
explicitly enabling the protocol on both peers. Telemetry does not change socket
options, negotiate extensions or reconnect a member.

Before creating the group session, qualify
`CAP_GROUP_PATH_IDENTIFIERS_V1` (`robotweax.groups.path-identifiers.v1`) at the actual
provider revision and build. Then, before its first sample, inject the selected
provider's `robotweax_srt_group_path_data_v1` into
`robotweax_srt_telemetry_public_srt_group_session_set_path_data_v1`. A non-null
getter without this capability, or on a Haivision session, is rejected. A null
getter disables the lookup before sampling; configuration changes after the
first valid sample invocation are rejected. Existing config, result and member
struct layouts are unchanged. The setter is an additive Telemetry C export.

For interchangeable providers, resolve the optional symbol from the exact
library that supplied the injected SRT functions and owns the handles. Header
presence, provider name or version text alone does not establish runtime support.
Do not resolve it process-wide from another loaded SRT provider. The SDK never
loads a second provider or links the optional SRT symbol. Its callback type uses
the public 44-byte v1 record contract independently of the build header, so it
also builds against older Robotweax and Haivision headers.

The two group snapshots may race with membership changes. Join by socket ID,
never index. Metadata allocation is bounded to 1024 records and at most three
reads per tick; oversized groups keep statistics but cannot assign a new label.
A confirmed unlabeled record (zero length and flags) permits the normal fallback.
A failed read, missing or duplicate socket ID, inconsistent count, invalid length,
flags or padding, or uncompleted negotiation keeps the name empty with
`MEMBER_PATH_PENDING`. It is retried on later ticks. This prevents a transient
lookup failure from permanently naming a labeled replacement by its new port.
An invalid host callback may be called again while naming is pending; callbacks
must not throw. A cached name survives later metadata failures.

A replacement using the same caller bytes gets the same displayed label even
when its source port changes. Overlapping members remain separate entries by
socket ID, with independent counters and lifecycle. A retained BROKEN member
keeps its cached name; removal reports it once, then forgets it. A recreated
Telemetry session resets its cache. Labels are untrusted caller metadata, scoped
to the host-authorized group/session; identical labels neither authenticate a
peer nor prove physical path diversity. They do not add live NAT migration.

No cross-provider enabled-label interoperability or real WAN/outage performance
is established by the loopback tests. The Network Lab must qualify those
properties in separately pinned scenarios.

## Provider behaviour observed

Live loopback, two members, application sending 500 packets/s; Haivision SRT
v1.5.7 with `ENABLE_BONDING=ON` and Robotweax SRT v0.2.8. Closing the primary
member on the caller side:

| | Haivision SRT 1.5.7 | Robotweax SRT 0.2.8 |
| --- | --- | --- |
| Token on the accepting side | −1 | −1 |
| Weight on the accepting side | the caller's weight | the caller's weight |
| `SRTO_GROUPTYPE` on an accepted group | readable | fails ("Operation not supported") |
| Caller member local address once connected | `127.0.0.1:port` | `0.0.0.0:port` (bound wildcard) |
| Receiver `memberstate`, Broadcast | RUNNING on every member that receives | RUNNING only on the member that delivered last; others IDLE while receiving |
| Receiver `memberstate`, Backup standby | IDLE, RUNNING after failover | IDLE, RUNNING after failover |
| Broken member on the accepting side | listed BROKEN about 1 s, then removed; `srt_bistats` fails at once | stays listed BROKEN (`sockstate` BROKEN); `srt_bistats` keeps answering |
| Backup failover after a clean member close | receiver drops 57–60 packets (about 120 ms) | no drops |

In every run, the caller group's unique sent count equalled the messages the
application sent, and the listener group's unique received count equalled the
messages the application read. On Haivision, received plus group receiver
drops equalled sent.

## Tests

- `robotweax_srt_telemetry_public_srt_group_tests` exercises a fake
  `srt_group_data`/`srt_bistats`/`srt_getsockname`/`srt_getsockstate`:
  - config and result validation;
  - Backup failover, with the stale counters of a broken member, removal, and
    a replacement member on the same token;
  - accepting-side path names: host, invalid host name, IPv6, unknown local
    address;
  - Broadcast carrying sets, and a Backup parallel probe and failback;
  - group state changes and failures;
  - buffer growth, truncation with pending removals, a group that keeps
    growing, and duplicate listings;
  - an unwritten `peeraddr` (no stale address), a local address that changes
    on connect, and a first listing that fails;
  - host names without NUL, empty and of maximum length, and the longest
    scoped IPv6 name;
  - an unknown group type, a removed active member, and a removed member
    without statistics;
  - carrying per direction, and a state-only session.
- `robotweax_srt_telemetry_public_srt_group_live_tests` runs Broadcast and
  Backup over loopback on the linked provider (free port, waits until both
  members are connected). It checks member listing, path sources, carrying
  and the active member, the failover after closing the primary, and counter
  parity with the application. It also passes with both providers running at
  once on two loaded CPUs. CI builds Haivision SRT
  with `ENABLE_BONDING=ON` and configures
  `ROBOTWEAX_SRT_TELEMETRY_REQUIRE_GROUP_TEST=ON`, so a library without groups
  fails instead of skipping.

## Not covered yet

- Impaired paths (loss, delay, flapping, asymmetry): the Network Lab's Docker
  multi-path setups (part D).
- Interop groups (Robotweax ↔ Haivision).
- IPv6 groups and encrypted groups.
