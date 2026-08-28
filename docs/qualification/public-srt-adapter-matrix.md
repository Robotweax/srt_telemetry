# Public SRT adapter qualification matrix

Date: 2026-08-28

This matrix separates build-time compatibility from live behavioral
qualification. A pass in the first three columns does not authorize production
capabilities by itself.

| Provider candidate | Exact source | Public header/signature | Deterministic adapter behavior | Bidirectional self-loopback | Cross-implementation interop | Status |
| --- | --- | --- | --- | --- | --- | --- |
| Robotweax SRT `v0.2.0-4-g271f832` | `271f832ef4bb96fab13777a313a0af9d7e20af7e`; clean local clone | Pass | Pass | Pass | Not run | Runtime candidate |
| Haivision SRT v1.5.5 | `b6b4ae990daa8193625a4ddeaeaed03023b23125`; tag `v1.5.5`; generated installed headers | Pass | Pass | Pass | Not run | Runtime candidate |

Both builds use the same adapter source and a compile-time assertion that the
provider's `srt_bistats` address has exactly the injected callback type. The
tests also prove `clear=0`, `instantaneous=1`, candidate field mapping, unit
conversion, unavailable negative values, lifetime-counter deltas, and explicit
native failures.

The provider-linked loopback test opens real caller/listener sockets, exchanges
1,316-byte messages in both directions, samples caller and accepted endpoints,
and requires availability plus positive deltas for physical and unique packet
and wire-byte counters. RTT must also be available on both endpoints.

## Remaining release qualification

- live caller-to-listener and listener-to-caller sampling where applicable;
- Robotweax-to-Haivision runs in both data directions;
- exact roles, modes, address families, encryption, FEC, and group profiles;
- observed reset and reconnect semantics for every enabled field;
- sampling overhead and close/sample lifecycle race tests; and
- retained evidence manifests with exact binaries, build flags, payloads, and
  expected observations.

Until full field and cross-implementation checks pass, the candidate records
intentionally declare no release-qualified runtime capabilities.
