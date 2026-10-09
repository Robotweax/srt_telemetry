# Connection groups (group session)

The group session observes one SRT connection group, Broadcast or Main/Backup,
together with all of its member sockets. One call per sampling tick reads the
group and every member, so they are linked and timestamped alike. Header:
`adapters/public_srt/include/robotweax/srt_telemetry_public_srt_group.h`.

This is part A of group support: the adapter. Sampler sources, labels,
journal events and history for groups (part B), the member health profile and
group verdict (part C), and the Network Lab's multi-path qualification
(part D) build on it.

## What one call reads

`robotweax_srt_telemetry_public_srt_group_session_sample` makes, through the
functions the host injects:

| Call | Count | Gives |
| --- | --- | --- |
| `srt_getsockstate(group)` | 1 | `group_state` |
| `srt_group_data(group)` | 1; up to 3 when the group outgrew the session's buffer | member list: socket, `sockstate`, `memberstate`, `result`, `weight`, `token`, peer address |
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

A path is named once, at the first call that sees the member CONNECTED, and
the name never changes. The sources, in order:

1. The host's `path_name` callback: 1–159 characters from 0x21–0x7E
   (printable ASCII without space), NUL-terminated. It is called once per
   member socket with the token, weight and addresses, and must not throw.
   An invalid name falls back to the automatic one.
2. `token-<n>` from the caller's `SRT_SOCKGROUPCONFIG.token`. It stays stable
   across reconnects only when the host passes the same token again.
3. `<local>><peer>` addresses, e.g. `10.0.0.9:9000>10.0.2.1:42000` ("?" for
   an unknown side; a scoped IPv6 address includes `%scope`). The path field
   holds two of the longest addresses, so automatic names are never cut.

On the accepting side SRT reports token −1, so only host names or addresses
are available. The peer port changes with every reconnect, so a reconnected
path gets a new name unless the host names it. The handshake does not carry a
path identity; this is reported as a protocol improvement.

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
