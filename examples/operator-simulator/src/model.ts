export type SourceId = "a" | "b" | "c";
export type Scenario =
  | "normal"
  | "interruption"
  | "stale"
  | "preview"
  | "reject"
  | "no-media"
  | "unknown";
export type Source = {
  id: SourceId;
  name: string;
  role: string;
  connection: "Connected" | "Disconnected" | "Reconnecting";
  media: "Arriving" | "No media";
  preview: "Healthy" | "Failed";
  fresh: boolean;
  generation: number;
};
export type Phase =
  "requested" | "accepted" | "applied" | "observed" | "rejected" | "unknown";
export type Command = {
  id: string;
  target: SourceId;
  phase: Phase;
  revision: number;
};
export type Event = {
  epoch: number;
  kind:
    "accept" | "apply" | "observe" | "reject" | "unknown" | "connect" | "media";
  generation?: number;
  commandId?: string;
};
export type State = {
  epoch: number;
  scenario: Scenario;
  sources: Source[];
  program: SourceId;
  preview: SourceId;
  armed: boolean;
  revision: number;
  serial: number;
  command?: Command;
  observed: SourceId | null;
  queue: Event[];
  log: string[];
  step: number;
};
export const scenarios: Record<Scenario, string> = {
  normal: "Normal operation",
  interruption: "Connection interruption",
  stale: "Stale telemetry",
  preview: "Preview failure",
  reject: "Rejected switching",
  "no-media": "Connected · no media",
  unknown: "Lost command response",
};
export function initial(scenario: Scenario = "normal", epoch = 0): State {
  const sources: Source[] = [
    {
      id: "a",
      name: "Camera A",
      role: "Studio · wide",
      connection: "Connected",
      media: "Arriving",
      preview: "Healthy",
      fresh: true,
      generation: 1,
    },
    {
      id: "b",
      name: "Camera B",
      role: "Studio · close",
      connection: "Connected",
      media: "Arriving",
      preview: "Healthy",
      fresh: true,
      generation: 1,
    },
    {
      id: "c",
      name: "Guest encoder",
      role: "Remote contribution",
      connection: "Disconnected",
      media: "No media",
      preview: "Failed",
      fresh: false,
      generation: 1,
    },
  ];
  const b = sources[1];
  if (scenario === "interruption") {
    b.connection = "Disconnected";
    b.media = "No media";
    b.generation = 2;
  }
  if (scenario === "stale") b.fresh = false;
  if (scenario === "preview") b.preview = "Failed";
  if (scenario === "no-media") b.media = "No media";
  return {
    epoch,
    scenario,
    sources,
    program: "a",
    preview: "b",
    armed: false,
    revision: 7,
    serial: 0,
    observed: "a",
    queue: [],
    log: [`Scenario loaded: ${scenarios[scenario]}. All evidence synthetic.`],
    step: 0,
  };
}
export function busy(s: State) {
  return (
    !!s.command &&
    ["requested", "accepted", "applied", "unknown"].includes(s.command.phase)
  );
}
export function blocked(s: State): string | null {
  const source = s.sources.find((x) => x.id === s.preview)!;
  if (busy(s)) return "Resolve the pending command before another Take.";
  if (s.command?.phase === "rejected")
    return "Recover from the rejection before arming again.";
  if (s.program === s.preview) return "Preview is already the current Program.";
  if (source.connection !== "Connected") return "Source is not connected.";
  if (source.media !== "Arriving") return "No media arriving from this source.";
  if (!source.fresh) return "Refresh stale telemetry before arming.";
  if (source.preview === "Failed")
    return "Restore preview before visual confirmation.";
  return null;
}
export type Action =
  | { type: "reset"; scenario: Scenario }
  | { type: "select"; id: SourceId }
  | { type: "arm" }
  | { type: "lock" }
  | { type: "take" }
  | { type: "next" }
  | { type: "event"; event: Event }
  | { type: "recover" }
  | { type: "navigate" };
export function reducer(s: State, a: Action): State {
  if (a.type === "reset") return initial(a.scenario, s.epoch + 1);
  if (a.type === "navigate" || a.type === "lock") return { ...s, armed: false };
  if (a.type === "select")
    return busy(s) ? s : { ...s, preview: a.id, armed: false };
  if (a.type === "arm") return blocked(s) ? s : { ...s, armed: true };
  if (a.type === "take") {
    if (!s.armed || blocked(s)) return s;
    const serial = s.serial + 1;
    const kinds: Event["kind"][] =
      s.scenario === "reject"
        ? ["reject"]
        : s.scenario === "unknown"
          ? ["accept", "unknown"]
          : ["accept", "apply", "observe"];
    return {
      ...s,
      serial,
      armed: false,
      command: {
        id: `sim-${s.epoch}-${serial}`,
        target: s.preview,
        phase: "requested",
        revision: s.revision,
      },
      queue: kinds.map((kind) => ({
        kind,
        epoch: s.epoch,
        commandId: `sim-${s.epoch}-${serial}`,
      })),
      log: [
        `Take requested: ${s.preview.toUpperCase()} → Program 01.`,
        ...s.log,
      ],
    };
  }
  if (a.type === "next") {
    if (!s.queue.length) return s;
    return reducer(
      { ...s, queue: s.queue.slice(1) },
      { type: "event", event: s.queue[0] },
    );
  }
  if (a.type === "recover") {
    if (s.command?.phase === "unknown")
      return {
        ...s,
        command: { ...s.command, phase: "applied" },
        program: s.command.target,
        observed: null,
        revision: s.revision + 1,
        queue: [{ epoch: s.epoch, kind: "observe", commandId: s.command.id }],
        log: [
          "Reconciliation: switcher commit confirmed; downstream unknown.",
          ...s.log,
        ],
      };
    const b = s.sources[1];
    if (s.queue.length) return s;
    if (b.connection !== "Connected")
      return {
        ...s,
        armed: false,
        sources: s.sources.map((x) =>
          x.id === "b" ? { ...x, connection: "Reconnecting" } : x,
        ),
        queue: [
          { epoch: s.epoch, kind: "connect", generation: b.generation },
          { epoch: s.epoch, kind: "media", generation: b.generation },
        ],
        log: ["Reconnect requested; media still absent.", ...s.log],
      };
    return {
      ...s,
      armed: false,
      scenario: "normal",
      sources: s.sources.map((x) =>
        x.id === "b"
          ? { ...x, media: "Arriving", preview: "Healthy", fresh: true }
          : x,
      ),
      command: s.command?.phase === "rejected" ? undefined : s.command,
      log: ["Recovery confirmed by synthetic readback.", ...s.log],
    };
  }
  const e = a.event;
  if (e.epoch !== s.epoch) return s;
  if (e.kind === "connect" || e.kind === "media") {
    if (e.generation !== s.sources[1].generation) return s;
    return {
      ...s,
      step: s.step + 1,
      sources: s.sources.map((x) =>
        x.id === "b"
          ? {
              ...x,
              connection: "Connected",
              media: e.kind === "media" ? "Arriving" : "No media",
              fresh: true,
            }
          : x,
      ),
      log: [
        e.kind === "connect"
          ? "Connection restored; waiting for media."
          : "Media arrival confirmed.",
        ...s.log,
      ],
    };
  }
  const c = s.command;
  if (!c || e.commandId !== c.id) return s;
  const allowed: Partial<Record<Event["kind"], Phase>> = {
    accept: "requested",
    reject: "requested",
    apply: "accepted",
    unknown: "accepted",
    observe: "applied",
  };
  if (c.phase !== allowed[e.kind]) return s;
  const phase: Phase = (
    {
      accept: "accepted",
      reject: "rejected",
      apply: "applied",
      unknown: "unknown",
      observe: "observed",
    } as const
  )[e.kind];
  return {
    ...s,
    step: s.step + 1,
    command: { ...c, phase },
    program: e.kind === "apply" ? c.target : s.program,
    revision: e.kind === "apply" ? s.revision + 1 : s.revision,
    observed:
      e.kind === "apply" ? null : e.kind === "observe" ? c.target : s.observed,
    log: [
      `${c.id}: ${phase}${phase === "rejected" ? " — simulated revision conflict; Program unchanged." : phase === "unknown" ? " — effect unknown; reconcile before retry." : ""}`,
      ...s.log,
    ],
  };
}
// Closed, in-process API boundary. No fetch, sockets, server URL or passthrough.
export function simulatedApi(
  s: State,
  method: string,
  path: string,
  body?: Action,
): State {
  if (method === "POST" && path === "/sim/action" && body)
    return reducer(s, body);
  throw new Error(`Unmatched simulation request: ${method} ${path}`);
}
