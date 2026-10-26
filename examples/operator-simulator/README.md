# Robotweax Operator Simulator

A standalone, synthetic product-UX laboratory: Operations → Stream detail → Program / safe switching. English UI, React/TypeScript, Vite, shadcn/ui Button composition (Radix Slot + CVA), shared density/color tokens. No production endpoints, credentials or component backend changes.

## Start and verify

Use Node 24 or newer and npm. From this directory:

```sh
npm ci
npm run dev -- --port 4311
# Open http://127.0.0.1:4311/
npm run typecheck
npm run lint
npm test
npm run build
```

The development server binds loopback only. `dist/` is a static build; no deployment is configured or authorized. The simulator's dependencies/build are independent of the telemetry CMake build and embedded `server/ui`.

## Walkthrough

1. Operations shows three sources with separate connection, media, preview and telemetry evidence. Open Camera B, then review it in switching.
2. Camera A is the initial confirmed Program. Camera B is selected Preview. Selection alone never changes Program. Arm Program 01, then Take.
3. **Manual replay limitation:** use **Next event in the simulator panel** to deliver accepted → applied → observed evidence. These are simulated backend events, not extra production operator actions. There is no automatic clock or Play/Pause in this slice; no timers exist to leak across resets. Applied changes Program while downstream remains unknown until observed.
4. Select a scenario to reset to its deterministic starting point. Recover/reconcile and Next event advance recovery; Reset/replay clears command/event queues and increments the run epoch.

| Scenario                | Expected evidence and recovery                                                                                          |
| ----------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| Normal operation        | Intent → ACK → commit → downstream; no inferred instant success                                                         |
| Connection interruption | Camera B disconnected/no media → recover requests reconnect → next event connected/no media → next event media arriving |
| Connected · no media    | Connection alone is insufficient; arming blocked until recovery                                                         |
| Stale telemetry         | Media still arriving; telemetry ungraded, packet loss unknown; recover refreshes evidence                               |
| Preview failure         | Media still arriving; failed preview independently visible; restore before visual confirmation                          |
| Rejected switching      | Simulated revision conflict preserves known Program; recover, re-arm and submit a new command                           |
| Lost command response   | Effect unknown; no blind retry; reconcile confirms commit, next event observes downstream                               |

Cancel arming affects only an unsent action. In-app navigation/back disarms and preserves submitted commands. Reloading/closing the whole standalone app ends this in-memory lab session; it is not a real remote-command cancellation mechanism. No browser persistence is claimed.

## Design and scope

See [inventory and decisions](DESIGN.md), [verification](VERIFICATION.md) and [product guidance](https://github.com/Robotweax/broadcast-platform-product/blob/main/docs/ux.md). The canonical product repository owns product decisions. Placement here is temporary co-location with existing frontend experience, **not a decision that the telemetry library owns all future platform UI**.

No real SRT, media decode, downstream receiver, latency or seamless-switching measurements. Test patterns deliberately identify synthetic content. Future identity, routing, pairing, media assets and automation are outside this slice; no working-looking placeholder controls for them.
