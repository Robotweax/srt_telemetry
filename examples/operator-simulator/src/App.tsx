import { useEffect, useReducer, useState } from "react";
import {
  Activity,
  ArrowRight,
  ChevronRight,
  CircleHelp,
  LayoutGrid,
  LockKeyhole,
  Radio,
  RotateCcw,
  ShieldCheck,
  SkipForward,
  Workflow,
} from "lucide-react";
import { Button } from "./components/ui/button";
import {
  initial,
  simulatedApi,
  blocked,
  busy,
  scenarios,
  type Action,
  type Source,
  type Scenario,
} from "./model";
import { en as t } from "./copy";
function Badge({
  children,
  tone = "muted",
}: {
  children: React.ReactNode;
  tone?: string;
}) {
  return <span className={`badge ${tone}`}>{children}</span>;
}
function Monitor({
  source,
  label,
  program = false,
}: {
  source: Source;
  label: string;
  program?: boolean;
}) {
  return (
    <div className="monitor-wrap">
      <div className="monitor-label">
        <span>{label}</span>
        <b>{source.name}</b>
      </div>
      <div
        className={`monitor ${source.preview === "Failed" ? "failed" : ""}`}
        role="img"
        aria-label={`${label}: ${source.name}, ${source.preview === "Failed" ? "preview unavailable" : "synthetic test pattern"}`}
      >
        <div className="safe-area" />
        <div className="test-circle">
          {source.preview === "Failed" ? (
            <CircleHelp size={32} />
          ) : (
            <span>{source.id.toUpperCase()}</span>
          )}
        </div>
        <span className="monitor-corner">
          {source.preview === "Failed"
            ? "PREVIEW UNAVAILABLE"
            : "SYNTHETIC · 1080p25"}
        </span>
        <span className="monitor-bottom">
          {source.preview === "Failed"
            ? "Media status is independent"
            : "TEST PATTERN · NOT LIVE VIDEO"}
        </span>
        <span className={`tally ${program ? "pgm" : "pvw"}`}>
          {program ? "PGM" : label === "Preview" ? "PVW" : "SRC"}
        </span>
      </div>
    </div>
  );
}
function Evidence({ source }: { source: Source }) {
  return (
    <dl className="evidence">
      <div>
        <dt>Connection</dt>
        <dd>
          <Badge tone={source.connection === "Connected" ? "ok" : "warn"}>
            {source.connection}
          </Badge>
        </dd>
      </div>
      <div>
        <dt>Media arrival</dt>
        <dd>
          <Badge tone={source.media === "Arriving" ? "ok" : "bad"}>
            {source.media}
          </Badge>
        </dd>
      </div>
      <div>
        <dt>Preview pipeline</dt>
        <dd>
          <Badge tone={source.preview === "Healthy" ? "ok" : "warn"}>
            {source.preview}
          </Badge>
        </dd>
      </div>
      <div>
        <dt>Telemetry</dt>
        <dd>
          <Badge tone={source.fresh ? "ok" : "warn"}>
            {source.fresh ? "Fresh · fixture tick" : "Stale · not graded"}
          </Badge>
        </dd>
      </div>
    </dl>
  );
}
export default function App() {
  const [s, dispatch] = useReducer(
    (state: ReturnType<typeof initial>, action: Action) =>
      simulatedApi(state, "POST", "/sim/action", action),
    undefined,
    () => initial(),
  );
  const [route, setRoute] = useState(window.location.hash || "#/");
  useEffect(() => {
    const onHash = () => {
      setRoute(window.location.hash || "#/");
      dispatch({ type: "navigate" });
    };
    window.addEventListener("hashchange", onHash);
    return () => window.removeEventListener("hashchange", onHash);
  }, []);
  const program = s.sources.find((x) => x.id === s.program)!;
  const preview = s.sources.find((x) => x.id === s.preview)!;
  const detail = s.sources.find((x) => route === `#/stream/${x.id}`);
  const isProgram = route === "#/program";
  const reason = blocked(s);
  const phase = s.command?.phase;
  const recoverable =
    (!!s.command && ["rejected", "unknown"].includes(s.command.phase)) ||
    s.scenario !== "normal";
  function link(path: string) {
    dispatch({ type: "navigate" });
    window.location.hash = path;
  }
  return (
    <div className="shell">
      <aside className="sidebar">
        <a className="brand" href="#/" aria-label="Robotweax Operations">
          <img src="/robotweax-r.svg" alt="" />
          <div>
            ROBO<span>TWEAX</span>
            <small>OPERATOR SIMULATOR</small>
          </div>
        </a>
        <div className="workspace">
          WORKSPACE<strong>{t.project}</strong>
          <small>Project REF-01</small>
        </div>
        <nav aria-label="Main navigation">
          <a href="#/" aria-current={!isProgram ? "page" : undefined}>
            <LayoutGrid size={16} />
            {t.operations}
          </a>
          <a href="#/program" aria-current={isProgram ? "page" : undefined}>
            <Workflow size={16} />
            {t.program}
          </a>
        </nav>
        <div className="side-note">
          <ShieldCheck size={18} />
          <strong>{t.boundary}</strong>
          <p>{t.boundaryNote}</p>
          <a
            href="https://github.com/Robotweax/broadcast-platform-product/blob/main/docs/ux.md"
            target="_blank"
            rel="noreferrer"
          >
            Product design guidance ↗
          </a>
        </div>
      </aside>
      <div className="body">
        <header className="topbar">
          <span>
            <Radio size={15} />
            {t.project}
            <ChevronRight size={14} />
            <b>
              {isProgram ? t.switching : detail ? t.sourceDetail : t.operations}
            </b>
          </span>
          <Badge tone="sim">{t.simulation}</Badge>
        </header>
        <section className="simulation" aria-label="Simulation controls">
          <div>
            <label htmlFor="scenario">SIMULATOR / {t.scenario}</label>
            <select
              id="scenario"
              aria-label="Scenario"
              value={s.scenario}
              onChange={(e) =>
                dispatch({
                  type: "reset",
                  scenario: e.target.value as Scenario,
                })
              }
            >
              {Object.entries(scenarios).map(([value, label]) => (
                <option value={value} key={value}>
                  {label}
                </option>
              ))}
            </select>
          </div>
          <Button
            variant="outline"
            size="sm"
            onClick={() => dispatch({ type: "reset", scenario: s.scenario })}
          >
            <RotateCcw size={14} />
            {t.reset}
          </Button>
          <Button
            variant="outline"
            size="sm"
            disabled={!s.queue.length}
            onClick={() => dispatch({ type: "next" })}
          >
            <SkipForward size={14} />
            {t.step} ({s.queue.length})
          </Button>
          <Button
            variant="outline"
            size="sm"
            disabled={!recoverable || !!s.queue.length}
            onClick={() => dispatch({ type: "recover" })}
          >
            {t.recover}
          </Button>
          <span
            className="tick"
            title="Manual simulator stepping; no real time or latency"
          >
            Tick {s.step.toString().padStart(2, "0")}
          </span>
          <small className="replay-note">
            Manual replay: Next event simulates backend evidence. It is not a
            production operator action.
          </small>
        </section>
        <main>
          <div className="heading">
            <div>
              <p className="eyebrow">
                {isProgram
                  ? "PROGRAM / CONTROL ROOM A"
                  : detail
                    ? "SOURCE / " + detail.id.toUpperCase()
                    : "LIVE OPERATIONS"}
              </p>
              <h1>
                {isProgram
                  ? "Program 01 · Preview / Take"
                  : detail
                    ? detail.name
                    : t.overview}
              </h1>
              <p>
                {isProgram ? t.instruction : detail ? detail.role : t.subtitle}
              </p>
            </div>
            {!isProgram && (
              <Button onClick={() => link("/program")} variant="outline">
                {t.viewProgram}
                <ArrowRight size={14} />
              </Button>
            )}
          </div>
          {!isProgram && !detail && route !== "#/" ? (
            <section className="panel">
              <h2>{t.notFound}</h2>
              <a href="#/">{t.back}</a>
            </section>
          ) : null}
          {!isProgram && !detail && route === "#/" && (
            <>
              <div className="summary-strip">
                <span>
                  <b>
                    {s.sources.filter((x) => x.media === "Arriving").length}/3
                  </b>{" "}
                  media arriving
                </span>
                <span>
                  <b>{s.sources.filter((x) => !x.fresh).length}</b> telemetry
                  ungraded
                </span>
                <span>
                  Program <b>{program.name}</b>
                </span>
                <span>
                  Destination{" "}
                  <b>
                    {s.observed
                      ? s.sources.find((x) => x.id === s.observed)?.name
                      : "Unknown"}
                  </b>
                </span>
              </div>
              <section className="panel">
                <div className="section-title">
                  <h2>{t.sources}</h2>
                  <span>3 sources · synthetic fixture</span>
                </div>
                <div className="source-grid">
                  {s.sources.map((source) => (
                    <article key={source.id}>
                      <Monitor
                        source={source}
                        label={
                          s.program === source.id
                            ? "CURRENT PROGRAM"
                            : "CONTRIBUTION"
                        }
                        program={s.program === source.id}
                      />
                      <div className="source-meta">
                        <b>{source.name}</b>
                        <span>{source.role}</span>
                      </div>
                      <Evidence source={source} />
                      <Button
                        variant="outline"
                        className="full"
                        onClick={() => link(`/stream/${source.id}`)}
                      >
                        {t.detail}
                        <ChevronRight size={14} />
                      </Button>
                    </article>
                  ))}
                </div>
              </section>
              <section className="route-strip">
                <Workflow size={20} />
                <div>
                  <b>Program 01</b>
                  <span>
                    {program.name} → Control room A · downstream{" "}
                    {s.observed ? "observed (synthetic)" : "unknown"}
                  </span>
                </div>
                <Badge tone={busy(s) ? "warn" : "muted"}>
                  {phase ?? "No pending command"}
                </Badge>
                <a href="#/program">Review switching →</a>
              </section>
            </>
          )}
          {detail && (
            <>
              <a className="back" href="#/">
                ← {t.back}
              </a>
              <div className="detail-grid">
                <section className="panel">
                  <Monitor source={detail} label="SOURCE PREVIEW" />
                  <div className="section-title">
                    <h2>{t.evidence}</h2>
                    <span>
                      Session {detail.id}-g{detail.generation}
                    </span>
                  </div>
                  <Evidence source={detail} />
                </section>
                <section className="panel inspector">
                  <h2>Signal path</h2>
                  <div className="path-node">
                    {detail.name}
                    <small>Contribution source</small>
                  </div>
                  <div className="path-line">↓</div>
                  <div className="path-node">
                    Media arrival
                    <small>{detail.media} · independent of preview</small>
                  </div>
                  <div className="path-line">↓</div>
                  <div className="path-node">
                    Program 01
                    <small>
                      {s.program === detail.id
                        ? "Selected by confirmed commit"
                        : "Not selected for Program"}
                    </small>
                  </div>
                  <hr />
                  <h2>Current observation</h2>
                  <p>
                    {!detail.fresh
                      ? "Telemetry is stale. Current transport health and loss are unknown."
                      : detail.preview === "Failed"
                        ? "Preview pipeline unavailable. This alone does not indicate media failure."
                        : detail.media === "No media"
                          ? "No media is arriving. A connected socket is insufficient."
                          : "Synthetic fixture reports media arriving and preview available."}
                  </p>
                  <p className="muted">
                    No root-cause inference. Restore this scenario using the
                    simulation controls.
                  </p>
                  <div className="metric-row">
                    <span>Packet loss</span>
                    <b>
                      {detail.fresh && detail.media === "Arriving"
                        ? "0.2% · sample"
                        : "Unknown"}
                    </b>
                  </div>
                  <div className="metric-row">
                    <span>Glass-to-glass</span>
                    <b>Not measured</b>
                  </div>
                  <Button
                    className="full"
                    variant="outline"
                    onClick={() => {
                      dispatch({ type: "select", id: detail.id });
                      link("/program");
                    }}
                  >
                    Review in switching
                  </Button>
                </section>
              </div>
            </>
          )}
          {isProgram && (
            <div className="switch-layout">
              <section>
                <div className="monitor-grid">
                  <Monitor source={program} label={t.current} program />
                  <Monitor source={preview} label={t.preview} />
                </div>
                <div className="monitor-status">
                  <span>
                    Commit confirmed · revision {s.revision}
                    <small>
                      Downstream:{" "}
                      {s.observed
                        ? s.sources.find((x) => x.id === s.observed)?.name +
                          " observed (synthetic)"
                        : "Unknown · awaiting observation"}
                    </small>
                  </span>
                  <span>
                    Selection only · no automatic Take
                    <small>
                      {preview.media} / preview {preview.preview.toLowerCase()}
                    </small>
                  </span>
                </div>
                <section className="panel bank">
                  <div className="section-title">
                    <h2>Source bank</h2>
                    <span>Selection changes Preview only</span>
                  </div>
                  <div className="bank-buttons">
                    {s.sources.map((source) => (
                      <button
                        key={source.id}
                        disabled={busy(s)}
                        className={s.preview === source.id ? "selected" : ""}
                        aria-pressed={s.preview === source.id}
                        onClick={() =>
                          dispatch({ type: "select", id: source.id })
                        }
                      >
                        <b>{source.name}</b>
                        <small>{source.media}</small>
                        <span>
                          {s.program === source.id
                            ? "PROGRAM"
                            : s.preview === source.id
                              ? "PREVIEW"
                              : "SOURCE"}
                        </span>
                      </button>
                    ))}
                  </div>
                </section>
                <section className="take-zone">
                  <div>
                    <Badge tone={s.armed ? "sim" : "muted"}>
                      <LockKeyhole size={12} />
                      {s.armed ? "ARMED" : "LOCKED"}
                    </Badge>
                    <h2>
                      {program.name} → {preview.name}
                    </h2>
                    <p>Program 01 · Control room A · revision {s.revision}</p>
                    <p role="status">
                      {reason ?? (s.armed ? t.armed : t.unsent)}
                    </p>
                  </div>
                  <div className="take-buttons">
                    <Button
                      disabled={!!reason || s.armed}
                      onClick={() => dispatch({ type: "arm" })}
                    >
                      {t.arm}
                    </Button>
                    <Button
                      variant="take"
                      disabled={!s.armed || !!reason}
                      onClick={() => dispatch({ type: "take" })}
                    >
                      {t.take}
                    </Button>
                    <Button
                      variant="outline"
                      disabled={!s.armed}
                      onClick={() => dispatch({ type: "lock" })}
                    >
                      {t.lock}
                    </Button>
                  </div>
                </section>
              </section>
              <aside className="panel inspector">
                <h2>Execution & evidence</h2>
                <Badge
                  tone={
                    phase === "rejected" || phase === "unknown"
                      ? "bad"
                      : "muted"
                  }
                >
                  {phase ?? "No command submitted"}
                </Badge>
                <ol className="stages">
                  {(
                    ["requested", "accepted", "applied", "observed"] as const
                  ).map((stage, i) => {
                    const rank = [
                      "requested",
                      "accepted",
                      "applied",
                      "observed",
                    ].indexOf(phase ?? "");
                    return (
                      <li key={stage} className={rank >= i ? "reached" : ""}>
                        <span>{i + 1}</span>
                        <div>
                          <b>{stage}</b>
                          <small>
                            {stage === "requested"
                              ? "Operator intent"
                              : stage === "accepted"
                                ? "Command acknowledged"
                                : stage === "applied"
                                  ? "Switcher commit confirmed"
                                  : "Destination observation"}
                          </small>
                        </div>
                      </li>
                    );
                  })}
                </ol>
                <p>
                  {phase === "rejected"
                    ? "Revision conflict. Known Program is preserved. Recover, review, then arm again."
                    : phase === "unknown"
                      ? "Effect unknown. Reconcile before another Take."
                      : t.pending}
                </p>
                <hr />
                <Evidence source={preview} />
                <p className="muted">
                  {s.command?.id ?? "No command ID"}
                  <br />
                  All stages are synthetic. Continuity, decoder entry and audio
                  quality are not measured.
                </p>
              </aside>
            </div>
          )}
          <section className="panel journal">
            <div className="section-title">
              <h2>
                <Activity size={15} />
                {t.events}
              </h2>
              <span>Newest first · this replay only</span>
            </div>
            <ol>
              {s.log.slice(0, 8).map((event, i) => (
                <li key={`${s.log.length - i}-${event}`}>
                  <span>SIM {String(s.log.length - i).padStart(2, "0")}</span>
                  {event}
                </li>
              ))}
            </ol>
          </section>
          <footer>
            {t.simulationNote}
            <span>{t.catalog}</span>
          </footer>
        </main>
      </div>
    </div>
  );
}
