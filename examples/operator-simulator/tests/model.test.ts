import { describe, it, expect } from "vitest";
import {
  initial,
  reducer,
  simulatedApi,
  blocked,
  type State,
  type Action,
} from "../src/model";
const run = (s: State, ...actions: Action[]) => actions.reduce(reducer, s);
const take = (s = initial()) => run(s, { type: "arm" }, { type: "take" });
describe("simulation safety contracts", () => {
  it("rejects unmatched API requests without passthrough", () => {
    expect(() =>
      simulatedApi(initial(), "GET", "https://production.invalid/api"),
    ).toThrow("Unmatched");
  });
  it("preview selection cannot change Program; repeated Take has one command", () => {
    let s = run(
      initial(),
      { type: "select", id: "b" },
      { type: "arm" },
      { type: "take" },
    );
    expect(s.program).toBe("a");
    expect(reducer(s, { type: "take" })).toBe(s);
    s = reducer(s, { type: "next" });
    expect(s.command?.phase).toBe("accepted");
    expect(s.program).toBe("a");
    s = reducer(s, { type: "next" });
    expect(s.program).toBe("b");
    expect(s.observed).toBeNull();
    s = reducer(s, { type: "next" });
    expect(s.observed).toBe("b");
  });
  it("reject preserves Program and allows deliberate recovery", () => {
    let s = reducer(take(initial("reject")), { type: "next" });
    expect(s.program).toBe("a");
    expect(s.command?.phase).toBe("rejected");
    s = run(
      s,
      { type: "recover" },
      { type: "arm" },
      { type: "take" },
      { type: "next" },
      { type: "next" },
      { type: "next" },
    );
    expect(s.program).toBe("b");
  });
  it("back navigation disarms only before submission", () => {
    expect(run(initial(), { type: "arm" }, { type: "navigate" }).armed).toBe(
      false,
    );
    const s = run(
      take(),
      { type: "navigate" },
      { type: "lock" },
      { type: "next" },
    );
    expect(s.command?.phase).toBe("accepted");
    expect(s.queue.length).toBe(2);
  });
  it("reconnect is distinct from actual media; old session events ignored", () => {
    let s = run(initial("interruption"), { type: "recover" }, { type: "next" });
    expect(s.sources[1].connection).toBe("Connected");
    expect(s.sources[1].media).toBe("No media");
    expect(blocked(s)).toContain("No media");
    expect(
      reducer(s, {
        type: "event",
        event: { epoch: s.epoch, kind: "media", generation: 1 },
      }),
    ).toBe(s);
    s = reducer(s, { type: "next" });
    expect(s.sources[1].media).toBe("Arriving");
  });
  it("stale telemetry and failed preview do not imply media failure", () => {
    for (const scenario of ["stale", "preview"] as const) {
      const s = initial(scenario);
      expect(s.sources[1].media).toBe("Arriving");
      expect(blocked(s)).not.toBeNull();
    }
  });
  it("reset clears pending events and invalidates delayed old epoch", () => {
    const s = take();
    const event = s.queue[0];
    const reset = reducer(s, { type: "reset", scenario: "normal" });
    expect(reset.queue).toEqual([]);
    expect(reset.command).toBeUndefined();
    expect(reducer(reset, { type: "event", event })).toBe(reset);
    expect(reducer(reset, { type: "next" })).toBe(reset);
  });
  it("unknown response blocks further Take until reconciled", () => {
    let s = run(take(initial("unknown")), { type: "next" }, { type: "next" });
    expect(s.command?.phase).toBe("unknown");
    expect(blocked(s)).toContain("pending");
    s = reducer(s, { type: "recover" });
    expect(s.program).toBe("b");
    expect(s.observed).toBeNull();
    expect(reducer(s, { type: "next" }).observed).toBe("b");
  });
  it("out of order apply cannot fabricate a commit", () => {
    const s = take();
    expect(
      reducer(s, { type: "event", event: { epoch: s.epoch, kind: "apply" } }),
    ).toBe(s);
  });
});
