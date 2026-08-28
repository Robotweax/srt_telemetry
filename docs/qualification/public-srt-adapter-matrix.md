# Public SRT adapter qualification matrix

Date: 2026-08-28

This matrix separates build-time compatibility from live behavioral
qualification. A pass in the first three columns does not authorize production
capabilities by itself.

| Provider candidate | Exact source | Public header/signature | Deterministic adapter behavior | Bidirectional self-loopback | Cross-implementation interop | Status |
| --- | --- | --- | --- | --- | --- | --- |
| Robotweax SRT `v0.2.0-4-g271f832` | `271f832ef4bb96fab13777a313a0af9d7e20af7e`; clean local clone | Pass | Pass | Pass | Pass, nine-field Live/Message IPv4 subset | Interop candidate |
| Haivision SRT v1.5.5 | `b6b4ae990daa8193625a4ddeaeaed03023b23125`; tag `v1.5.5`; generated installed headers | Pass | Pass | Pass | Pass, nine-field Live/Message IPv4 subset | Interop candidate |

Both builds use the same adapter source and a compile-time assertion that the
provider's `srt_bistats` address has exactly the injected callback type. The
tests also prove `clear=0`, `instantaneous=1`, candidate field mapping, unit
conversion, unavailable negative values, lifetime-counter deltas, and explicit
native failures.

The provider-linked loopback test opens real caller/listener sockets, exchanges
1,316-byte messages in both directions, samples caller and accepted endpoints,
and requires availability plus positive deltas for physical and unique packet
and wire-byte counters. RTT must also be available on both endpoints.

The two-process qualification executes each implementation once as caller and
once as listener. Both directions carry a deterministic 1,316-byte payload only
after out-of-band baseline barriers. The retained result records source
revisions, build profiles, participant observations, and SHA-256 digests of the
provider libraries, peer executables, and payloads:

- [2026-08-28 macOS arm64 public SRT interop](../../qualification/results/2026-08-28-macos-arm64-public-srt-interop.json)

## Sampling and lifecycle candidate evidence

The retained macOS arm64 run executes the providers sequentially and records
the exact binaries. Its isolated synthetic callback measured median incremental
SDK work of 40.5 ns in the Robotweax-linked build and 54.5 ns in the
Haivision-linked build. The provider-call median was 26.6 us for Robotweax SRT
and 0.211 us for Haivision SRT. The paired Robotweax provider-call difference
was negative because provider variance exceeded the much smaller SDK cost; the
synthetic measurement is the meaningful adapter-cost isolation.

Both providers passed the supported order in which the telemetry session is
destroyed before its socket is closed. The stale-handle probe returned the
explicit provider-sample failure with native `SRT_ERROR` and no available
fields. A separate time-bounded close/sample race also terminated without a
crash or hang and reported the same failure, but it remains deliberately
unsupported host usage.

Scheduler isolation and power management control were unavailable on this
host, so these timings are candidate observations rather than release budgets.
The roughly two-orders-of-magnitude provider-call difference is an
investigation signal for Robotweax SRT, not yet a regression conclusion.

- [2026-08-28 macOS arm64 sampling and lifecycle](../../qualification/results/2026-08-28-macos-arm64-public-srt-runtime.json)

## Remaining release qualification

- live caller-to-listener and listener-to-caller sampling for the remaining
  candidate fields and profiles;
- additional modes, address families, encryption, FEC, and group profiles;
- observed reset and reconnect semantics for every enabled field;
- release sampling budgets on representative controlled platforms and build
  profiles;
- longer-duration lifecycle and reconnect stress tests; and
- retained evidence manifests with exact binaries, build flags, payloads, and
  expected observations.

Until full field and deployment-profile checks pass, the candidate records
intentionally declare no release-qualified runtime capabilities.
