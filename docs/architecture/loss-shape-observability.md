# Loss-shape observability decision

Status: accepted candidate decision, 2026-08-29.

## Operator question

Can the common public Robotweax SRT / Haivision SRT counters distinguish eight
packet losses concentrated in one 50 ms observation window from the same eight
losses distributed across eight 50 ms windows?

This is deliberately narrower than proving that lost packet sequence numbers
were adjacent inside a window.

## Evidence

The retained macOS arm64 experiment uses exact Robotweax SRT
`271f832ef4bb96fab13777a313a0af9d7e20af7e` and Haivision SRT v1.5.5
`b6b4ae990daa8193625a4ddeaeaed03023b23125` builds. A continuous 240-message
stream passes through a deterministic UDP fault proxy while the SDK samples
both endpoints at a wall-clock-paced 20 Hz. Both providers run once as sender
and once as receiver.

In all four scenarios, the aggregate observations are identical: eight sender
loss packets, eight receiver loss packets, eight retransmissions, 248 physical
sender packets, 240 unique sender packets, and 240 received application
packets. The distributed profile produces eight one-packet loss windows. The
concentrated profile produces one eight-packet loss window. All four schedules
record zero complete 50 ms window misses; their maximum observed sample
interval is 55.35 ms.

The result, exact executable and provider-library digests, proxy observations,
sampling schedule, and cost projection are retained in
[the loss-shape qualification](../../qualification/results/2026-08-29-macos-arm64-loss-shape-observability.json).

## Decision

Use `incident-mode-20hz-sdk-sampling` for this coarse loss-shape question. Do
not request a Robotweax-only protocol signal yet. Haivision and Robotweax use
the same public counter surface for this decision, so the diagnostic remains
available with either selected host library.

Twenty hertz is an incident-mode profile, not an always-on default. A projection
from the earlier uncontrolled provider-call medians estimates 0.0529% of one
core for Robotweax SRT, 0.000517% for Haivision SRT, and 0.0534% combined. That
projection is feasibility evidence only; it is not a release performance gate.
The much larger Robotweax provider-call cost remains an optimization signal for
the Robotweax SRT coding agent.

## Boundary and reopening condition

The decision does not prove packet-sequence adjacency within one 50 ms window,
finite-flight-tail recovery, representative deployment timing, or a controlled
multi-platform cost budget. It also does not authorize concurrent socket close
and sampling.

Reopen the Robotweax telemetry extension gate only when a concrete operator
question requires within-window sequence evidence or another failure class
cannot be discriminated through the qualified public fields. Any proposed
extension must retain an explicit weaker Haivision fallback and be coordinated
with the Robotweax SRT coding agent before protocol implementation.
