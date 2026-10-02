/* SRT Telemetry built-in UI.
 *
 * Plain JavaScript, no dependencies, embedded in the HTTP library. Reads
 * /api/v1/snapshot, /api/v1/sources/{id}/history and /api/v1/events, then
 * follows the WebSocket feed (/api/v1/live), polling when the feed is
 * unavailable. Every value from the server is inserted as text, never as
 * markup: labels and stream IDs can come from remote peers. */
'use strict';

(() => {
  // ------------------------------------------------------------ vocabulary

  const VERDICT_LABEL = { ok: 'OK', watch: 'Watch', at_risk: 'At risk', failing: 'Failing', unknown: 'Unknown' };
  /* Fleet order: worst first; Unknown sits above OK because nobody can vouch
   * for it. */
  const VERDICT_RANK = { failing: 0, at_risk: 1, watch: 2, unknown: 3, ok: 4 };
  const VERDICT_ORDER = ['failing', 'at_risk', 'watch', 'unknown', 'ok'];

  /* What each reason means for an operator. */
  const REASON_TEXT = {
    none: 'Every indicator is within its thresholds.',
    no_data: 'Nothing has arrived or been sent for a few seconds: the source may be paused, or the path is down.',
    missing_metric: 'A metric this verdict needs is not reported by the SRT library.',
    missing_setting: 'A setting this verdict needs (latency or bandwidth limits) is not reported.',
    source_error: 'Reading the SRT statistics failed.',
    connecting: 'The connection is being set up.',
    connection_lost: 'The connection is lost.',
    role_unknown: 'The endpoint is neither a known sender nor a known receiver.',
    recovery_headroom_low: 'Lost packets are repaired with little time to spare: the receive buffer runs low against the latency.',
    latency_low_for_rtt: 'The latency is too short for this round-trip time to repair losses reliably.',
    unrecovered_loss: 'Packets are lost for good: the viewer sees errors.',
    undecryptable: 'Packets cannot be decrypted: the passphrases do not match.',
    late_repairs: 'Retransmissions arrive too late to be played.',
    recovery_inefficient: 'Many lost packets are not repaired.',
    overhead_budget_high: 'Retransmissions are using up the bandwidth allowed for recovery.',
    path_capacity_exceeded: 'The stream sends more than the path can carry.',
    sender_backlog: 'Data is piling up in the send buffer; packets will soon be dropped as too late.',
    sender_drops: 'The sender drops packets as too late to send.',
    peer_read_stall: 'The receiver is not taking data: its buffer is full.',
    application_read_stall: 'The receiving application does not read fast enough.',
    acknowledgements_stalled: 'Data waits without acknowledgements or loss reports: the path or the peer stopped answering.',
    path_congested: 'The path is congested: the round-trip time rose sharply and loss persists. More latency will not help.',
  };

  const REASON_SHORT = {
    none: 'all clear',
    no_data: 'no data',
    missing_metric: 'metric missing',
    missing_setting: 'setting missing',
    source_error: 'statistics read failed',
    connecting: 'connecting',
    connection_lost: 'connection lost',
    role_unknown: 'role unknown',
    recovery_headroom_low: 'little recovery headroom',
    latency_low_for_rtt: 'latency short for the RTT',
    unrecovered_loss: 'unrecovered loss',
    undecryptable: 'undecryptable packets',
    late_repairs: 'repairs arrive late',
    recovery_inefficient: 'repairs failing',
    overhead_budget_high: 'recovery bandwidth used up',
    path_capacity_exceeded: 'above path capacity',
    sender_backlog: 'send buffer backlog',
    sender_drops: 'sender drops',
    peer_read_stall: 'receiver not reading',
    application_read_stall: 'application not reading',
    acknowledgements_stalled: 'no acknowledgements',
    path_congested: 'path congested',
  };

  const INDICATORS = {
    recovery_headroom: {
      name: 'Recovery headroom',
      desc: 'Receive buffer span ÷ latency: the time left to repair a gap. About 100 % when nothing is missing.',
      fmt: (v) => fmtPercent(v),
    },
    latency_sufficiency: {
      name: 'Latency vs RTT',
      desc: 'Latency ÷ round-trip time. SRT needs several round trips to repair a loss.',
      fmt: (v) => `${fmtNumber(v, 2)} × RTT`,
    },
    unrecovered_ratio: {
      name: 'Unrecovered packets',
      desc: 'Share of packets never delivered to the application, over 10 s.',
      fmt: (v) => fmtPercent(v),
    },
    recovery_efficiency: {
      name: 'Repair success',
      desc: 'Share of lost packets that were repaired in time, over 10 s.',
      fmt: (v) => fmtPercent(v),
    },
    overhead_budget: {
      name: 'Recovery bandwidth used',
      desc: 'Retransmissions ÷ the overhead SRT may add (OHEADBW or MAXBW), over 10 s.',
      fmt: (v) => fmtPercent(v),
    },
    path_headroom: {
      name: 'Load vs capacity',
      desc: 'Sending rate ÷ estimated link capacity; graded only when the path shows congestion.',
      fmt: (v) => fmtPercent(v),
    },
    sender_backlog: {
      name: 'Send buffer vs drop limit',
      desc: 'Oldest data in the send buffer ÷ the too-late drop threshold.',
      fmt: (v) => fmtPercent(v),
    },
    peer_read_stall: {
      name: 'Peer read stall',
      desc: (role) => (role === 'receiver' ? 'Data waiting for the application ÷ latency: above 1 the application reads late.' : 'How full the receiver keeps its buffer: near 100 % it has stopped taking data.'),
      fmt: (v, role) => (role === 'receiver' ? `${fmtNumber(v, 2)} × latency` : fmtPercent(v)),
    },
  };

  const EVENT_KIND = {
    source_added: 'Source added',
    source_removed: 'Source removed',
    verdict_changed: 'Verdict changed',
  };

  // ------------------------------------------------------------ formatting

  function fmtNumber(v, digits) {
    if (v === null || v === undefined || !Number.isFinite(v)) return '—';
    return v.toLocaleString(undefined, { maximumFractionDigits: digits, minimumFractionDigits: 0 });
  }

  function sig(v) {
    const a = Math.abs(v);
    const text = a >= 100 ? v.toFixed(0) : a >= 10 ? v.toFixed(1) : v.toFixed(2);
    return text.includes('.') ? text.replace(/\.?0+$/, '') : text;
  }

  function fmtBitrate(v) {
    if (v === null || v === undefined || !Number.isFinite(v)) return '—';
    const a = Math.abs(v);
    if (a >= 1e9) return `${sig(v / 1e9)} Gbit/s`;
    if (a >= 1e6) return `${sig(v / 1e6)} Mbit/s`;
    if (a >= 1e3) return `${sig(v / 1e3)} kbit/s`;
    return `${v.toFixed(0)} bit/s`;
  }

  function fmtDuration(v) {
    if (v === null || v === undefined || !Number.isFinite(v)) return '—';
    const a = Math.abs(v);
    if (v === 0) return '0 ms';
    if (a >= 10) return `${v.toFixed(1)} s`;
    if (a >= 1) return `${v.toFixed(2)} s`;
    if (a >= 0.01) return `${(v * 1000).toFixed(0)} ms`;
    return `${(v * 1000).toFixed(1)} ms`;
  }

  function fmtPacketRate(v) {
    if (v === null || v === undefined || !Number.isFinite(v)) return '—';
    if (v === 0) return '0 pkt/s';
    return `${Math.abs(v) >= 10 ? v.toFixed(0) : v.toFixed(1)} pkt/s`;
  }

  function fmtPercent(r) {
    if (r === null || r === undefined || !Number.isFinite(r)) return '—';
    const p = r * 100;
    if (p === 0) return '0 %';
    const a = Math.abs(p);
    if (a >= 10) return `${p.toFixed(0)} %`;
    if (a >= 1) return `${p.toFixed(1)} %`;
    if (a >= 0.1) return `${p.toFixed(2)} %`;
    return `${p.toFixed(3)} %`;
  }

  function fmtBytes(v) {
    if (v === null || v === undefined || !Number.isFinite(v)) return '—';
    const a = Math.abs(v);
    if (a >= 1e9) return `${sig(v / 1e9)} GB`;
    if (a >= 1e6) return `${sig(v / 1e6)} MB`;
    if (a >= 1e3) return `${sig(v / 1e3)} kB`;
    return `${v.toFixed(0)} B`;
  }

  function fmtCount(v) {
    if (v === null || v === undefined || !Number.isFinite(v)) return '—';
    return Math.round(v).toLocaleString();
  }

  function fmtAge(seconds) {
    if (!Number.isFinite(seconds) || seconds < 0) return '';
    if (seconds < 60) return `${Math.floor(seconds)} s`;
    const minutes = Math.floor(seconds / 60);
    if (minutes < 60) return `${minutes} min`;
    const hours = Math.floor(minutes / 60);
    if (hours < 48) return `${hours} h ${minutes % 60} min`;
    return `${Math.floor(hours / 24)} d`;
  }

  function fmtClock(ms, withSeconds = true) {
    const d = new Date(ms);
    const pad = (n) => String(n).padStart(2, '0');
    return withSeconds
      ? `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
      : `${pad(d.getHours())}:${pad(d.getMinutes())}`;
  }

  function timeZoneLabel() {
    const minutes = -new Date().getTimezoneOffset();
    const sign = minutes >= 0 ? '+' : '-';
    const a = Math.abs(minutes);
    return `UTC${sign}${String(Math.floor(a / 60)).padStart(2, '0')}:${String(a % 60).padStart(2, '0')}`;
  }

  /* Formats a value by its UCUM unit, as the snapshot reports it. */
  function fmtUnit(unit, v) {
    switch (unit) {
      case 'bit/s':
        return fmtBitrate(v);
      case 's':
        return fmtDuration(v);
      case '{packet}/s':
        return fmtPacketRate(v);
      case '{packet}':
        return `${fmtNumber(v, 0)} pkt`;
      case 'By':
        return fmtBytes(v);
      case '1':
        return fmtPercent(v);
      default:
        return fmtNumber(v, 3);
    }
  }

  // ------------------------------------------------------------ DOM helpers

  /* Elements are built from text only. */
  function h(tag, attrs, ...children) {
    const el = document.createElement(tag);
    applyAttrs(el, attrs);
    append(el, children);
    return el;
  }

  const SVG = 'http://www.w3.org/2000/svg';

  function s(tag, attrs, ...children) {
    const el = document.createElementNS(SVG, tag);
    applyAttrs(el, attrs);
    append(el, children);
    return el;
  }

  function applyAttrs(el, attrs) {
    if (!attrs) return;
    for (const [k, v] of Object.entries(attrs)) {
      if (v === null || v === undefined || v === false) continue;
      if (k === 'text') el.textContent = v;
      else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
      else el.setAttribute(k, v === true ? '' : String(v));
    }
  }

  /* replaceChildren without null or false children. */
  function put(el, ...children) {
    el.replaceChildren();
    append(el, children);
  }

  function append(el, children) {
    for (const c of children.flat(Infinity)) {
      if (c === null || c === undefined || c === false) continue;
      el.append(c instanceof Node ? c : document.createTextNode(String(c)));
    }
  }

  function verdictIcon(verdict, size = 14) {
    const v = VERDICT_LABEL[verdict] ? verdict : 'unknown';
    const svg = s('svg', { width: size, height: size, viewBox: '0 0 16 16', 'aria-hidden': 'true' });
    /* Shape carries the verdict as well as colour. */
    if (v === 'ok') {
      svg.append(s('circle', { class: 'fill', cx: 8, cy: 8, r: 7 }),
        s('path', { d: 'M4.6 8.2l2.3 2.3 4.6-4.8', fill: 'none', stroke: '#fff', 'stroke-width': 1.8, 'stroke-linecap': 'round', 'stroke-linejoin': 'round' }));
    } else if (v === 'watch') {
      svg.append(s('circle', { class: 'fill', cx: 8, cy: 8, r: 7 }),
        s('path', { d: 'M8 4.2v4.6', stroke: '#1a1a19', 'stroke-width': 1.9, 'stroke-linecap': 'round' }),
        s('circle', { cx: 8, cy: 11.4, r: 1.1, fill: '#1a1a19' }));
    } else if (v === 'at_risk') {
      svg.append(s('path', { class: 'fill', d: 'M8 1.2l7.2 13H.8z' }),
        s('path', { d: 'M8 6v4', stroke: '#1a1a19', 'stroke-width': 1.8, 'stroke-linecap': 'round' }),
        s('circle', { cx: 8, cy: 12.2, r: 1, fill: '#1a1a19' }));
    } else if (v === 'failing') {
      svg.append(s('path', { class: 'fill', d: 'M5.1 1h5.8L15 5.1v5.8L10.9 15H5.1L1 10.9V5.1z' }),
        s('path', { d: 'M5.5 5.5l5 5m0-5l-5 5', stroke: '#fff', 'stroke-width': 1.8, 'stroke-linecap': 'round' }));
    } else {
      svg.append(s('circle', { class: 'fill', cx: 8, cy: 8, r: 7 }),
        s('path', { d: 'M6.2 6.2a1.9 1.9 0 1 1 2.6 1.8c-.5.2-.8.6-.8 1.1v.4', fill: 'none', stroke: '#fff', 'stroke-width': 1.6, 'stroke-linecap': 'round' }),
        s('circle', { cx: 8, cy: 11.8, r: 0.95, fill: '#fff' }));
    }
    return svg;
  }

  function verdictBadge(verdict, size) {
    const v = VERDICT_LABEL[verdict] ? verdict : 'unknown';
    return h('span', { class: `verdict v-${v}` }, verdictIcon(v, size), VERDICT_LABEL[v]);
  }

  // ------------------------------------------------------------ advice

  /* Turns the health verdict into the operator's answer: what to do, and
   * why. Watch never asks for a change; its knob becomes a hint. */
  function advice(health, role) {
    if (!health) return { action: 'Health grading is not enabled', why: '', hint: '' };
    const v = health.verdict;
    const why = REASON_TEXT[health.reason] || health.reason || '';
    const value = health.suggested_value;
    const has = value !== null && value !== undefined && Number.isFinite(value);
    const verb = (fallback) => (health.direction === 'increase' ? 'Raise' : health.direction === 'decrease' ? 'Lower' : fallback);
    let change = null;
    switch (health.knob) {
      case 'latency': {
        /* SRTO_LATENCY sets the receiver's latency; a sender asks for it
         * with SRTO_PEERLATENCY. The connection uses the larger value. */
        const option = role === 'sender' ? 'SRTO_PEERLATENCY' : 'SRTO_LATENCY';
        change = has ? `${verb('Raise')} the latency to ${fmtDuration(value)} (${option})` : `${verb('Raise')} the latency (${option})`;
        break;
      }
      case 'bitrate':
        change = has ? `${verb('Lower')} the bitrate to ${fmtBitrate(value)}` : `${verb('Lower')} the bitrate`;
        break;
      case 'overhead_bandwidth':
        change = has ? `${verb('Raise')} SRTO_OHEADBW to ${fmtNumber(value, 0)} %` : `${verb('Raise')} SRTO_OHEADBW`;
        break;
      case 'max_bandwidth':
        change = has ? `${verb('Raise')} SRTO_MAXBW to ${fmtNumber(value, 0)} bytes/s (${fmtBitrate(value * 8)})` : `${verb('Raise')} SRTO_MAXBW`;
        break;
      case 'passphrase':
        change = 'Check that both ends use the same passphrase';
        break;
      case 'receiving_application':
        change = 'Check the receiving application: it is not reading';
        break;
      default:
        break;
    }
    if (v === 'watch') {
      return { action: 'Nothing to change yet', why, hint: change ? `If it persists: ${lowerFirst(change)}` : '' };
    }
    if (v === 'ok') return { action: 'No action needed', why, hint: '' };
    if (change) return { action: change, why, hint: '' };
    let action;
    if (v === 'unknown') action = health.reason === 'connecting' ? 'Wait for the connection' : 'No grade possible';
    else if (health.reason === 'connection_lost') action = 'Check the network path and the peer';
    else action = 'No setting fixes this: check the network path';
    return { action, why, hint: '' };
  }

  function lowerFirst(text) {
    return text && /^[A-Z][a-z]/.test(text) ? text.charAt(0).toLowerCase() + text.slice(1) : text;
  }

  // ------------------------------------------------------------ state

  const state = {
    token: '',
    window: 1,
    sources: new Map(),
    generatedMs: 0,
    lastDataAt: 0,
    series: new Map(),
    events: [],
    eventSequences: new Set(),
    lastSequence: 0,
    overwritten: 0,
    journal: true,
    historyLoaded: new Map(), /* id -> time of the last load attempt */
    viewedId: null,
    route: { view: 'fleet' },
    filter: { verdict: null, query: '', role: 'all' },
    rangeSeconds: 15 * 60,
    hover: null,
    link: 'connecting',
    socket: null,
    retry: 1000,
    reconnectTimer: null,
    pollTimer: null,
    lastMessageAt: 0,
    liveIntervalMs: 1000,
    locked: false,
    started: false,
  };

  /* One hour of 1 s points for the source on screen; two minutes (the
   * sparkline) for the others, so a large fleet stays small. */
  const MAX_POINTS = 3700;
  const FLEET_POINTS = 130;
  const MAX_EVENTS = 5000;

  function storage(kind) {
    try {
      return kind === 'session' ? window.sessionStorage : window.localStorage;
    } catch {
      return null;
    }
  }

  function readStored(kind, key) {
    try {
      return storage(kind)?.getItem(key) ?? '';
    } catch {
      return '';
    }
  }

  function writeStored(kind, key, value) {
    try {
      if (value) storage(kind)?.setItem(key, value);
      else storage(kind)?.removeItem(key);
    } catch {
      /* private mode: keep it in memory only */
    }
  }

  // ------------------------------------------------------------ data model

  /* Labels and stream IDs can come from remote peers: drop bidirectional
   * and invisible formatting characters, so a name cannot pose as another. */
  function clean(text) {
    return String(text).replace(/[\u0000-\u001f\u007f​-‏‪-‮⁠-⁩﻿]/g, '');
  }

  function sourceName(src) {
    const l = src.labels || {};
    for (const key of ['stream', 'name', 'channel', 'title', 'stream_id', 'id']) {
      if (typeof l[key] === 'string' && clean(l[key])) return { name: clean(l[key]), key };
    }
    return { name: `Source ${src.id}`, key: null };
  }

  function sourceChips(src, skipKey) {
    return Object.entries(src.labels || {})
      .filter(([k]) => k !== skipKey)
      .map(([k, v]) => {
        const text = `${clean(k)}=${clean(v)}`;
        return h('span', { class: 'chip', title: text }, text);
      });
  }

  function roleScope(role, key) {
    if (key.startsWith('srt.connection.')) return true;
    if (role === 'sender') return key.startsWith('srt.sender.');
    if (role === 'receiver') return key.startsWith('srt.receiver.');
    return true;
  }

  function seriesOf(id) {
    let entry = state.series.get(id);
    if (!entry) {
      entry = { points: [] };
      state.series.set(id, entry);
    }
    return entry;
  }

  /* Points are kept in time order, one per window start. A live point
   * replaces a history point of the same window. */
  function insertPoint(id, point) {
    const pts = seriesOf(id).points;
    const n = pts.length;
    if (n === 0 || pts[n - 1].t < point.t) {
      pts.push(point);
    } else {
      let lo = 0;
      let hi = n;
      while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (pts[mid].t < point.t) lo = mid + 1;
        else hi = mid;
      }
      if (lo < n && pts[lo].t === point.t) {
        if (point.live || !pts[lo].live) pts[lo] = point;
      } else {
        pts.splice(lo, 0, point);
      }
    }
    const limit = id === state.viewedId ? MAX_POINTS : FLEET_POINTS;
    if (pts.length > limit) pts.splice(0, pts.length - limit);
  }

  /* Only the statistics the charts and sparklines use are kept, as numbers. */
  const KEPT = new Map();
  function keep(key, stats) {
    const list = KEPT.get(key) || [];
    for (const st of stats) if (!list.includes(st)) list.push(st);
    KEPT.set(key, list);
  }

  function compact(metrics) {
    const v = {};
    for (const [key, stats] of KEPT) {
      const value = metrics[key];
      if (!value) continue;
      for (const st of stats) {
        const x = value[st];
        if (typeof x === 'number' && Number.isFinite(x)) v[`${key}|${st}`] = x;
      }
    }
    return v;
  }

  function pointFromWindow(src) {
    const w = src.window;
    if (!w || !w.metrics || !Number.isFinite(w.start_unix_ms)) return null;
    return {
      t: w.start_unix_ms,
      w: Math.max(1, w.end_unix_ms - w.start_unix_ms),
      verdict: src.health ? src.health.verdict : null,
      reason: src.health ? src.health.reason : null,
      v: compact(w.metrics),
      live: true,
    };
  }

  /* History records name packet counts per window; the charts use rates. */
  function pointFromRecord(role, record) {
    const m = {};
    const seconds = Math.max(0.001, record.window_ms / 1000);
    for (const [key, value] of Object.entries(record.metrics || {})) {
      if (!roleScope(role, key)) continue;
      if (key.endsWith('.packets') && value.total !== undefined) {
        m[`${key.slice(0, -'.packets'.length)}.packet_rate`] = { rate: value.total / seconds };
      } else {
        m[key] = value;
      }
    }
    return {
      t: record.start_unix_ms,
      w: Math.max(1, record.window_ms),
      verdict: record.health ? record.health.verdict : null,
      reason: record.health ? record.health.reason : null,
      v: compact(m),
      live: false,
    };
  }

  function stat(point, key, stats) {
    for (const st of stats) {
      const x = point.v[`${key}|${st}`];
      if (x !== undefined) return x;
    }
    return null;
  }

  function windowStat(src, key, ...stats) {
    const value = src.window?.metrics?.[key];
    if (!value) return null;
    for (const st of stats) {
      const v = value[st];
      if (typeof v === 'number' && Number.isFinite(v)) return v;
    }
    return null;
  }

  function applySnapshot(snapshot) {
    if (!snapshot || !Array.isArray(snapshot.sources)) return;
    state.generatedMs = snapshot.generated_unix_ms || Date.now();
    state.lastDataAt = Date.now();
    const seen = new Set();
    for (const src of snapshot.sources) {
      seen.add(src.id);
      state.sources.set(src.id, src);
      const point = pointFromWindow(src);
      if (point) insertPoint(src.id, point);
    }
    for (const id of [...state.sources.keys()]) {
      if (!seen.has(id)) {
        state.sources.delete(id);
        state.series.delete(id);
        state.historyLoaded.delete(id);
      }
    }
    render();
  }

  /* Events are merged by sequence number, so a gap fetch and the feed can
   * arrive in any order. */
  function addEvents(events) {
    let added = false;
    for (const e of events || []) {
      if (!e || !Number.isFinite(e.sequence) || state.eventSequences.has(e.sequence)) continue;
      state.eventSequences.add(e.sequence);
      state.events.push(e);
      if (e.sequence > state.lastSequence) state.lastSequence = e.sequence;
      added = true;
    }
    if (added) {
      state.events.sort((a, b) => a.sequence - b.sequence);
      if (state.events.length > MAX_EVENTS) {
        for (const e of state.events.splice(0, state.events.length - MAX_EVENTS)) state.eventSequences.delete(e.sequence);
      }
    }
    return added;
  }

  // ------------------------------------------------------------ network

  class AuthError extends Error {}
  class HttpError extends Error {
    constructor(status) {
      super(`HTTP ${status}`);
      this.status = status;
    }
  }

  async function api(path) {
    const headers = {};
    if (state.token) headers.Authorization = `Bearer ${state.token}`;
    const response = await fetch(path, { headers, cache: 'no-store', credentials: 'omit' });
    if (response.status === 401) throw new AuthError('unauthorized');
    if (!response.ok) throw new HttpError(response.status);
    return response.json();
  }

  async function loadSnapshot() {
    const query = state.window ? `?window=${state.window}` : '';
    try {
      applySnapshot(await api(`/api/v1/snapshot${query}`));
    } catch (error) {
      if (error instanceof HttpError && error.status === 400 && state.window) {
        state.window = 0; /* 1 s windows not configured: use the default */
        return loadSnapshot();
      }
      throw error;
    }
  }

  async function loadEvents(after = state.lastSequence) {
    if (!state.journal) return;
    let cursor = after;
    for (let page = 0; page < 20; ++page) {
      let body;
      try {
        body = await api(`/api/v1/events?after=${cursor}`);
      } catch (error) {
        if (error instanceof HttpError && error.status === 404) {
          state.journal = false;
          return;
        }
        throw error;
      }
      if (Number.isFinite(body.overwritten)) state.overwritten = Math.max(state.overwritten, body.overwritten);
      addEvents(body.events);
      const list = body.events || [];
      if (!body.more || list.length === 0) break;
      cursor = list[list.length - 1].sequence;
    }
  }

  /* Loads (or fills in) the history of the source on screen: the last hour
   * on first view, and from the last point after a reconnect. */
  async function loadHistory(id, sinceMs) {
    const last = state.historyLoaded.get(id);
    if (sinceMs === undefined && last !== undefined && (last.ok || Date.now() - last.at < 10000)) return;
    const src = state.sources.get(id);
    if (!src) return;
    state.historyLoaded.set(id, { at: Date.now(), ok: false });
    let since = sinceMs ?? Math.max(0, (state.generatedMs || Date.now()) - 3600 * 1000);
    for (let page = 0; page < 4; ++page) {
      let body;
      try {
        body = await api(`/api/v1/sources/${encodeURIComponent(id)}/history?since_ms=${since}`);
      } catch (error) {
        if (error instanceof HttpError && error.status === 404) {
          state.historyLoaded.set(id, { at: Date.now(), ok: true }); /* history off, or the source is gone */
        }
        return;
      }
      for (const record of body.records || []) insertPoint(id, pointFromRecord(src.role, record));
      if (!body.more || !Number.isFinite(body.next_since_ms)) break;
      since = body.next_since_ms;
    }
    state.historyLoaded.set(id, { at: Date.now(), ok: true });
    render();
  }

  function setLink(link, text) {
    state.link = link;
    const el = document.getElementById('link-state');
    el.dataset.state = link;
    document.getElementById('link-text').textContent = text;
  }

  function closeFeed() {
    if (state.reconnectTimer) clearTimeout(state.reconnectTimer);
    state.reconnectTimer = null;
    if (state.socket) {
      const old = state.socket;
      state.socket = null;
      old.close();
    }
  }

  function connectFeed() {
    state.reconnectTimer = null;
    if (state.locked) return;
    if (!('WebSocket' in window)) {
      startPolling();
      return;
    }
    const params = new URLSearchParams();
    if (state.window) params.set('window', String(state.window));
    /* Browsers cannot set headers on a WebSocket; the server accepts the
     * token in the query for the feed only. */
    if (state.token) params.set('token', state.token);
    const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const query = params.toString();
    let socket;
    try {
      socket = new WebSocket(`${scheme}//${location.host}/api/v1/live${query ? `?${query}` : ''}`);
    } catch {
      startPolling();
      scheduleReconnect();
      return;
    }
    state.socket = socket;
    socket.addEventListener('open', () => {
      if (state.socket !== socket) return;
      state.retry = 1000;
      state.lastMessageAt = Date.now();
      stopPolling();
      setLink('live', 'Live');
      /* The feed sends events recorded after it opened, and windows from
       * now on: fetch what was missed while it was down. */
      const after = state.lastSequence;
      loadEvents(after).then(render).catch(() => {});
      if (state.viewedId !== null) {
        const pts = seriesOf(state.viewedId).points;
        if (pts.length) loadHistory(state.viewedId, pts[pts.length - 1].t + 1);
      }
    });
    socket.addEventListener('message', (message) => {
      if (state.socket !== socket) return;
      state.lastMessageAt = Date.now();
      let body;
      try {
        body = JSON.parse(message.data);
      } catch {
        return;
      }
      if (body.type === 'snapshot') applySnapshot(body.data);
      else if (body.type === 'events' && addEvents(body.events)) render();
    });
    socket.addEventListener('close', () => {
      if (state.socket !== socket) return;
      state.socket = null;
      if (state.locked) return;
      /* A refused feed may mean the token changed: polling finds out. */
      startPolling();
      scheduleReconnect();
    });
  }

  function scheduleReconnect() {
    if (state.reconnectTimer || state.locked) return;
    const delay = state.retry;
    state.retry = Math.min(state.retry * 2, 30000);
    state.reconnectTimer = setTimeout(connectFeed, delay);
  }

  /* A half-open connection (sleeping laptop, dropped NAT entry) sends
   * nothing: reconnect after a few silent intervals. */
  function watchdog() {
    if (state.socket && state.socket.readyState === WebSocket.OPEN && state.lastMessageAt &&
        Date.now() - state.lastMessageAt > Math.max(10000, 3 * state.liveIntervalMs)) {
      const stale = state.socket;
      state.socket = null;
      stale.close();
      startPolling();
      scheduleReconnect();
    }
  }

  function startPolling() {
    if (state.pollTimer || state.locked) return;
    setLink('polling', 'Polling');
    const tick = async () => {
      try {
        await loadSnapshot();
        await loadEvents();
        if (state.link !== 'live') setLink('polling', 'Polling');
      } catch (error) {
        if (error instanceof AuthError) {
          lock(true);
          return;
        }
        if (state.link !== 'live') setLink('offline', 'Offline');
      }
    };
    tick();
    state.pollTimer = setInterval(tick, 2000);
  }

  function stopPolling() {
    if (state.pollTimer) clearInterval(state.pollTimer);
    state.pollTimer = null;
  }

  // ------------------------------------------------------------ token gate

  function readTokenFromHash() {
    /* "#token=..." keeps the token out of server and proxy logs: the
     * fragment is never sent. It is moved to session storage at once and
     * removed from the address bar. */
    const hash = location.hash;
    if (hash.startsWith('#token=')) {
      let token = '';
      try {
        token = decodeURIComponent(hash.slice(7));
      } catch {
        token = '';
      }
      writeStored('session', 'srt-telemetry-token', token);
      history.replaceState(null, '', `${location.pathname}${location.search}#/`);
      return token;
    }
    return readStored('session', 'srt-telemetry-token');
  }

  /* Stops all traffic and asks for a token, once. */
  function lock(failed) {
    if (state.locked) return;
    state.locked = true;
    state.started = false;
    stopPolling();
    closeFeed();
    if (view && view.destroy) view.destroy();
    view = null;
    showGate(failed);
  }

  function showGate(failed) {
    const app = document.getElementById('app');
    setLink('offline', 'Locked');
    document.getElementById('banner').className = 'banner hidden';
    const input = h('input', { type: 'password', id: 'token-input', autocomplete: 'off', spellcheck: 'false', 'aria-label': 'Access token' });
    const submit = () => {
      state.token = input.value.trim();
      writeStored('session', 'srt-telemetry-token', state.token);
      start();
    };
    input.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') submit();
    });
    put(app,
      h('div', { class: 'card gate' },
        h('h1', { text: 'Access token required' }),
        h('p', {}, failed && state.token ? 'The token was not accepted. ' : '',
          'This telemetry server is protected by a token. Enter it to open the dashboard; it is kept for this browser tab only.'),
        input,
        h('button', { class: 'primary', type: 'button', onclick: submit, text: 'Open dashboard' })));
    input.focus();
  }

  // ------------------------------------------------------------ routing

  function parseRoute() {
    const match = /^#\/source\/(\d+)/.exec(location.hash);
    return match ? { view: 'source', id: Number(match[1]) } : { view: 'fleet' };
  }

  let view = null;

  function render() {
    if (!state.started) return;
    const route = parseRoute();
    if (!view || view.kind !== route.view || (route.view === 'source' && view.id !== route.id)) {
      if (view && view.destroy) view.destroy();
      view = route.view === 'source' ? sourceView(route.id) : fleetView();
      document.getElementById('app').replaceChildren(view.root);
    }
    view.update();
    renderBanner();
  }

  let bannerText = '';

  /* Stale data: the text names the time of the last data, so it does not
   * change (and is not re-announced) every second. */
  function renderBanner() {
    const banner = document.getElementById('banner');
    const age = state.lastDataAt ? (Date.now() - state.lastDataAt) / 1000 : 0;
    const text = state.started && state.lastDataAt && age > 5
      ? `No new data since ${fmtClock(state.lastDataAt)}. The values shown are from ${fmtClock(state.generatedMs)}.`
      : '';
    if (text === bannerText) return;
    bannerText = text;
    banner.textContent = text;
    banner.className = text ? 'banner error' : 'banner hidden';
  }

  // ------------------------------------------------------------ fleet view

  function primaryBitrateKey(role) {
    return role === 'receiver' ? 'srt.receiver.unique_received.bitrate' : 'srt.sender.unique_sent.bitrate';
  }

  /* Loss before repair, comparable across roles. */
  function lossShare(src) {
    if (src.role === 'receiver') {
      const lost = windowStat(src, 'srt.receiver.lost.packet_rate', 'rate');
      const got = windowStat(src, 'srt.receiver.unique_received.packet_rate', 'rate');
      if (lost === null || got === null) return null;
      return lost + got > 0 ? lost / (lost + got) : 0;
    }
    if (src.role !== 'sender') return null;
    const lost = windowStat(src, 'srt.sender.loss_reported.packet_rate', 'rate');
    const sent = windowStat(src, 'srt.sender.unique_sent.packet_rate', 'rate');
    if (lost === null || sent === null) return null;
    return sent > 0 ? Math.min(1, lost / sent) : 0;
  }

  function dropRate(src) {
    if (src.role !== 'sender' && src.role !== 'receiver') return null;
    return windowStat(src, src.role === 'receiver' ? 'srt.receiver.dropped.packet_rate' : 'srt.sender.dropped.packet_rate', 'rate');
  }

  function latencyOf(src) {
    if (src.role === 'receiver') return windowStat(src, 'srt.receiver.latency', 'last', 'mean');
    if (src.role === 'sender') return windowStat(src, 'srt.sender.peer_latency', 'last', 'mean');
    return windowStat(src, 'srt.receiver.latency', 'last', 'mean') ?? windowStat(src, 'srt.sender.peer_latency', 'last', 'mean');
  }

  keep('srt.sender.unique_sent.bitrate', ['rate', 'mean']);
  keep('srt.receiver.unique_received.bitrate', ['rate', 'mean']);

  function sparkline(id, role, width = 110, height = 28) {
    const svg = s('svg', { class: 'spark', width, height, viewBox: `0 0 ${width} ${height}`, 'aria-hidden': 'true' });
    const pts = seriesOf(id).points;
    const end = state.generatedMs || Date.now();
    const start = end - 120 * 1000;
    const key = primaryBitrateKey(role);
    const values = [];
    for (let i = Math.max(0, pts.length - FLEET_POINTS); i < pts.length; ++i) {
      const p = pts[i];
      if (p.t < start) continue;
      const v = stat(p, key, ['rate', 'mean']);
      if (v !== null) values.push(p, v);
    }
    if (values.length < 4) return svg;
    let max = 1;
    for (let i = 1; i < values.length; i += 2) max = Math.max(max, values[i]);
    let d = '';
    let prev = null;
    for (let i = 0; i < values.length; i += 2) {
      const p = values[i];
      const x = ((p.t + p.w / 2 - start) / (end - start)) * width;
      const y = height - 2 - (values[i + 1] / max) * (height - 4);
      /* Same gap rule as the charts: a missing window breaks the line. */
      d += `${prev === null || p.t - (prev.t + prev.w) > prev.w / 2 ? 'M' : 'L'}${x.toFixed(1)} ${y.toFixed(1)}`;
      prev = p;
    }
    svg.append(s('path', { d, class: 'line s1' }));
    return svg;
  }

  function fleetView() {
    const tallies = {};
    const summary = h('div', { class: 'summary' });
    for (const v of VERDICT_ORDER) {
      const count = h('span', { class: 'count num', text: '0' });
      const button = h('button', { class: `tally v-${v}`, type: 'button', 'aria-pressed': 'false' },
        count, verdictBadge(v, 13));
      button.addEventListener('click', () => {
        state.filter.verdict = state.filter.verdict === v ? null : v;
        render();
      });
      tallies[v] = { button, count };
      summary.append(button);
    }
    const title = h('h1', { text: 'Endpoints' });
    const search = h('input', { class: 'search', type: 'search', placeholder: 'Filter by name or label', 'aria-label': 'Filter endpoints' });
    search.value = state.filter.query;
    search.addEventListener('input', () => {
      state.filter.query = search.value;
      render();
    });
    const roles = h('div', { class: 'segmented', role: 'group', 'aria-label': 'Role' });
    const roleButtons = {};
    for (const [role, label] of [['all', 'All'], ['sender', 'Senders'], ['receiver', 'Receivers']]) {
      const b = h('button', { type: 'button', text: label });
      b.addEventListener('click', () => {
        state.filter.role = role;
        render();
      });
      roleButtons[role] = b;
      roles.append(b);
    }
    const tbody = h('tbody');
    const table = h('table', { class: 'fleet' },
      h('thead', {}, h('tr', {},
        h('th', { text: 'Status' }),
        h('th', { text: 'Endpoint' }),
        h('th', { text: 'Role' }),
        h('th', { text: 'What to do' }),
        h('th', { class: 'r', text: 'Bitrate' }),
        h('th', { class: 'r', text: 'RTT' }),
        h('th', { class: 'r', text: 'Latency' }),
        h('th', { class: 'r', text: 'Loss' }),
        h('th', { class: 'r', text: 'Dropped' }),
        h('th', { text: 'Last 2 min' }))),
      tbody);
    const empty = h('div', { class: 'empty hidden' });
    const eventsBody = h('tbody');
    const eventsCard = h('div', { class: 'card events' }, h('table', {}, eventsBody));
    const eventsSection = h('section', { class: 'section' },
      h('div', { class: 'section-head' }, h('h2', { text: 'Recent events' })), eventsCard);
    const root = h('div', {},
      summary,
      h('div', { class: 'headline' }, title, h('div', { class: 'toolbar' }, search, roles)),
      h('div', { class: 'card' }, table, empty),
      eventsSection);

    const rows = new Map();
    let order = '';

    /* Rebuilds a cell only when what it shows changed. */
    function setCell(cell, signature, build) {
      if (cell.dataset.sig === signature) return;
      cell.dataset.sig = signature;
      put(cell, build());
    }

    function rowFor(src) {
      let row = rows.get(src.id);
      if (!row) {
        const cells = {
          status: h('td', { class: 'status' }),
          name: h('td'),
          role: h('td'),
          advice: h('td', { class: 'advice' }),
          bitrate: h('td', { class: 'r num', 'data-label': 'Bitrate' }),
          rtt: h('td', { class: 'r num', 'data-label': 'RTT' }),
          latency: h('td', { class: 'r num', 'data-label': 'Latency' }),
          loss: h('td', { class: 'r num', 'data-label': 'Loss' }),
          dropped: h('td', { class: 'r num', 'data-label': 'Dropped' }),
          spark: h('td', { class: 'spark-cell' }),
        };
        const tr = h('tr', { tabindex: '0' }, Object.values(cells));
        const open = () => {
          location.hash = `#/source/${src.id}`;
        };
        tr.addEventListener('click', open);
        tr.addEventListener('keydown', (e) => {
          if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            open();
          }
        });
        row = { tr, cells, sparkAt: 0 };
        rows.set(src.id, row);
      }
      return row;
    }

    function update() {
      const all = [...state.sources.values()];
      const counts = { ok: 0, watch: 0, at_risk: 0, failing: 0, unknown: 0 };
      for (const src of all) {
        const v = src.health?.verdict;
        counts[VERDICT_LABEL[v] ? v : 'unknown'] += 1;
      }
      for (const v of VERDICT_ORDER) {
        tallies[v].count.textContent = String(counts[v]);
        tallies[v].count.classList.toggle('zero', counts[v] === 0);
        tallies[v].button.setAttribute('aria-pressed', state.filter.verdict === v ? 'true' : 'false');
      }
      for (const [role, b] of Object.entries(roleButtons)) b.setAttribute('aria-pressed', state.filter.role === role ? 'true' : 'false');
      /* Never a wrong OK: "no action needed" only when every endpoint is OK
       * or Watch; ungraded endpoints are counted, not assumed fine. */
      const problems = counts.failing + counts.at_risk;
      const plural = (n, word) => `${n} ${word}${n === 1 ? '' : 's'}`;
      let headline = 'Endpoints';
      if (all.length > 0) {
        if (problems > 0) headline = `${problems} of ${plural(all.length, 'endpoint')} need${problems === 1 ? 's' : ''} attention`;
        else if (counts.unknown > 0) headline = `${counts.unknown} of ${plural(all.length, 'endpoint')} cannot be graded`;
        else headline = `${plural(all.length, 'endpoint')}: no action needed`;
      }
      title.textContent = headline;

      const q = state.filter.query.trim().toLowerCase();
      const shown = all.filter((src) => {
        const v = VERDICT_LABEL[src.health?.verdict] ? src.health.verdict : 'unknown';
        if (state.filter.verdict && v !== state.filter.verdict) return false;
        if (state.filter.role !== 'all' && src.role !== state.filter.role) return false;
        if (q) {
          const text = [String(src.id), String(src.role), ...Object.entries(src.labels || {}).map(([k, val]) => `${k}=${val}`)].join(' ').toLowerCase();
          if (!text.includes(q)) return false;
        }
        return true;
      });
      shown.sort((a, b) => {
        const ra = VERDICT_RANK[a.health?.verdict] ?? 3;
        const rb = VERDICT_RANK[b.health?.verdict] ?? 3;
        if (ra !== rb) return ra - rb;
        const na = sourceName(a).name;
        const nb = sourceName(b).name;
        if (na !== nb) return na.localeCompare(nb);
        const ka = String(a.role);
        const kb = String(b.role);
        return ka === kb ? a.id - b.id : ka < kb ? -1 : 1;
      });
      const visible = new Set(shown.map((src) => src.id));
      for (const [id, row] of rows) {
        if (!visible.has(id)) {
          row.tr.remove();
          if (!state.sources.has(id)) rows.delete(id);
        }
      }
      const now = Date.now();
      for (const src of shown) {
        const { tr, cells } = rowFor(src);
        const row = rows.get(src.id);
        const health = src.health;
        const v = VERDICT_LABEL[health?.verdict] ? health.verdict : 'unknown';
        const age = health && Number.isFinite(health.verdict_age_seconds) ? `for ${fmtAge(health.verdict_age_seconds)}` : '';
        cells.status.className = `status v-${v}`;
        setCell(cells.status, `${v}|${age}`, () => [verdictBadge(v), age ? h('div', { class: 'faint num', text: age }) : null]);
        const { name, key } = sourceName(src);
        const role = String(src.role);
        setCell(cells.name, JSON.stringify([name, src.labels]), () => [h('div', { class: 'endpoint-name', text: name }),
          h('div', { class: 'labels' }, sourceChips(src, key))]);
        const connection = src.connection && src.connection !== 'connected' && src.connection !== 'unreported' ? String(src.connection) : '';
        setCell(cells.role, `${role}|${connection}`, () => [h('span', { class: 'role', text: role }),
          connection ? h('div', { class: 'faint', text: connection }) : null]);
        const a = advice(health, src.role);
        const why = v !== 'ok' && health?.reason && health.reason !== 'none' ? REASON_SHORT[health.reason] || String(health.reason) : '';
        setCell(cells.advice, `${a.action}|${a.hint}|${why}`, () => [h('div', { class: 'action', text: a.action }),
          a.hint ? h('div', { class: 'why', text: a.hint }) : null,
          why ? h('div', { class: 'why', text: why }) : null]);
        tr.setAttribute('aria-label', `${name}, ${role}: ${VERDICT_LABEL[v]}. ${a.action}. Open details.`);
        const texts = [
          [cells.bitrate, fmtBitrate(windowStat(src, primaryBitrateKey(src.role), 'rate'))],
          [cells.rtt, fmtDuration(windowStat(src, 'srt.connection.rtt', 'mean', 'last'))],
          [cells.latency, fmtDuration(latencyOf(src))],
          [cells.loss, fmtPercent(lossShare(src))],
          [cells.dropped, fmtPacketRate(dropRate(src))],
        ];
        for (const [cell, text] of texts) if (cell.textContent !== text) cell.textContent = text;
        const pts = seriesOf(src.id).points;
        const lastT = pts.length ? pts[pts.length - 1].t : 0;
        /* Sparklines follow new windows, at most once a second per row. */
        if (lastT !== row.sparkT && now - row.sparkAt >= 1000) {
          row.sparkT = lastT;
          row.sparkAt = now;
          put(cells.spark, sparkline(src.id, src.role));
        }
      }
      /* Reorder only when the order changed, so focus and hover stay. */
      const nextOrder = shown.map((src) => src.id).join(',');
      if (nextOrder !== order || tbody.children.length !== shown.length) {
        order = nextOrder;
        const focused = document.activeElement;
        for (const src of shown) tbody.append(rows.get(src.id).tr);
        if (focused && focused !== document.activeElement && document.contains(focused)) focused.focus();
      }
      table.classList.toggle('hidden', shown.length === 0);
      empty.classList.toggle('hidden', shown.length !== 0);
      const emptyText = all.length === 0
        ? 'No sources yet. Add SRT sockets to the sampler and they appear here.'
        : 'No endpoint matches the filter.';
      if (empty.textContent !== emptyText) empty.textContent = emptyText;

      eventsSection.classList.toggle('hidden', !state.journal);
      renderEvents(eventsBody, state.events, true, 50);
    }

    return { kind: 'fleet', root, update };
  }

  function eventDetail(e) {
    if (e.kind === 'verdict_changed' && typeof e.detail === 'string') {
      const [verdict, reasonPart] = e.detail.split(' ');
      const reason = (reasonPart || '').replace(/:$/, '');
      const rest = e.detail.includes(':') ? e.detail.slice(e.detail.indexOf(':') + 1).trim() : '';
      /* "knob direction value": phrase it like the advice. */
      const [knob, direction, value] = rest.split(' ');
      const src = state.sources.get(e.source_id);
      const suggestion = knob && knob !== 'none'
        ? advice({ verdict: 'at_risk', reason, knob, direction, suggested_value: value === undefined ? null : Number(value) }, src?.role).action
        : '';
      return h('span', {}, verdictBadge(verdict, 12), ' ',
        reason && reason !== 'none' ? REASON_SHORT[reason] || clean(reason) : '',
        suggestion ? h('span', { class: 'faint', text: ` → ${lowerFirst(suggestion)}` }) : null);
    }
    return h('span', { class: 'mono', text: clean(e.detail ?? '') });
  }

  function renderEvents(tbody, events, withSource, limit) {
    const list = events.slice(-limit).reverse();
    const key = `${withSource}:${list.length}:${list.length ? list[0].sequence : 0}:${state.sources.size}:${state.overwritten}`;
    if (tbody.dataset.key === key) return;
    tbody.dataset.key = key;
    const note = state.overwritten > 0
      ? h('tr', {}, h('td', { class: 'faint', colspan: '5', text: `${state.overwritten} older events were overwritten in the server's journal before they could be read.` }))
      : null;
    if (list.length === 0) {
      put(tbody, h('tr', {}, h('td', { class: 'faint', text: 'No events yet.' })), note);
      return;
    }
    put(tbody, ...list.map((e) => {
      const src = state.sources.get(e.source_id);
      const name = src ? `${sourceName(src).name} · ${src.role}` : `source ${e.source_id}`;
      const kind = String(e.kind ?? '');
      const severity = String(e.severity ?? '');
      return h('tr', {},
        h('td', { class: 'num faint', text: fmtClock(e.observed_unix_ms) }),
        withSource ? h('td', {}, src ? h('a', { href: `#/source/${Number(e.source_id)}`, text: name }) : h('span', { class: 'faint', text: name })) : null,
        h('td', {}, h('span', { class: `sev sev-${severity.replace(/[^a-z]/g, '')}`, text: severity })),
        h('td', { class: 'kind', text: EVENT_KIND[kind] || kind.replace(/_/g, ' ') }),
        h('td', {}, eventDetail(e)));
    }), note);
  }

  // ------------------------------------------------------------ charts

  const tooltip = () => document.getElementById('tooltip');

  function niceTicks(max, count = 4) {
    if (!(max > 0)) return { max: 1, ticks: [0, 0.25, 0.5, 0.75, 1] };
    const raw = max / count;
    const mag = 10 ** Math.floor(Math.log10(raw));
    const step = [1, 2, 2.5, 5, 10].map((m) => m * mag).find((st) => st >= raw) || 10 * mag;
    const top = Math.ceil(max / step) * step;
    const ticks = [];
    for (let v = 0; v <= top + step / 2; v += step) ticks.push(v);
    return { max: top, ticks };
  }

  /* Time axis step: about four to eight labels across the span. */
  function timeStep(span) {
    const steps = [10e3, 15e3, 30e3, 60e3, 120e3, 300e3, 600e3, 900e3, 1800e3, 3600e3];
    return steps.find((st) => span / st <= 8) || 3600e3;
  }

  function formatterFor(unit) {
    switch (unit) {
      case 'bit/s':
        return fmtBitrate;
      case 's':
        return fmtDuration;
      case 'pkt/s':
        return fmtPacketRate;
      case 'ratio':
        return fmtPercent;
      default:
        return (v) => fmtNumber(v, 2);
    }
  }

  /* A time-series line chart: one y axis, at most three series and dashed
   * reference lines, gaps where windows are missing. */
  function lineChart(spec) {
    const legend = h('div', { class: 'legend' },
      spec.series.map((se, i) => h('span', { class: 'key' }, h('span', { class: `swatch sw-${i + 1}` }), se.label)),
      (spec.refs || []).map((r) => h('span', { class: 'key' }, h('span', { class: 'swatch dashed sw-ref' }), r.label)));
    const svg = s('svg', { class: 'plot', height: spec.height || 170, role: 'img', 'aria-label': spec.title });
    const card = h('div', { class: 'card chart' }, h('h2', { text: spec.title }), legend, svg);
    const fmt = formatterFor(spec.unit);
    let geometry = null;

    function draw(src, points, start, end) {
      const width = Math.max(240, svg.clientWidth || 600);
      const height = spec.height || 170;
      const left = 64;
      const right = 10;
      const top = 8;
      const bottom = 22;
      const plotW = width - left - right;
      const plotH = height - top - bottom;
      svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
      const visible = points.filter((p) => p.t + p.w >= start && p.t <= end);
      const refs = (spec.refs || []).map((r) => ({ ...r, v: r.value(src) })).filter((r) => Number.isFinite(r.v));
      let max = 0;
      for (const p of visible) {
        for (const se of spec.series) {
          const v = stat(p, se.key, se.stats);
          if (v !== null && v > max) max = v;
        }
      }
      for (const r of refs) max = Math.max(max, r.v);
      if (spec.minMax) max = Math.max(max, spec.minMax);
      const { max: yMax, ticks } = niceTicks(max * 1.05);
      const x = (t) => left + ((t - start) / (end - start)) * plotW;
      const y = (v) => top + plotH - (v / yMax) * plotH;
      const nodes = [];
      const axis = s('g', { class: 'axis' });
      for (const tick of ticks) {
        const yy = y(tick);
        nodes.push(s('line', { class: tick === 0 ? 'baseline' : 'gridline', x1: left, x2: width - right, y1: yy, y2: yy }));
        axis.append(s('text', { x: left - 8, y: yy + 4, 'text-anchor': 'end', text: fmt(tick) }));
      }
      const stepMs = timeStep(end - start);
      for (let t = Math.ceil(start / stepMs) * stepMs; t <= end; t += stepMs) {
        axis.append(s('text', { x: x(t), y: height - 6, 'text-anchor': 'middle', text: fmtClock(t, stepMs < 60e3) }));
      }
      nodes.push(axis);
      for (const r of refs) {
        nodes.push(s('line', { class: 'line ref', x1: left, x2: width - right, y1: y(r.v), y2: y(r.v) }));
      }
      spec.series.forEach((se, i) => {
        let d = '';
        let prev = null;
        for (const p of visible) {
          const v = stat(p, se.key, se.stats);
          if (v === null) {
            prev = null;
            continue;
          }
          /* Break the line across missing windows: an outage must show. */
          const gap = prev === null || p.t - (prev.t + prev.w) > prev.w / 2;
          d += `${gap ? 'M' : 'L'}${x(p.t + p.w / 2).toFixed(1)} ${y(v).toFixed(1)}`;
          prev = p;
        }
        if (d) nodes.push(s('path', { class: `line s${i + 1}`, d }));
      });
      if (!visible.length) {
        nodes.push(s('text', { class: 'no-data', x: left + plotW / 2, y: top + plotH / 2, 'text-anchor': 'middle', text: 'Waiting for data' }));
      }
      const hoverPoint = state.hover !== null ? nearest(visible, state.hover) : null;
      if (hoverPoint) {
        const hx = x(hoverPoint.t + hoverPoint.w / 2);
        nodes.push(s('line', { class: 'crosshair', x1: hx, x2: hx, y1: top, y2: top + plotH }));
        spec.series.forEach((se, i) => {
          const v = stat(hoverPoint, se.key, se.stats);
          if (v !== null) nodes.push(s('circle', { class: `dot s${i + 1}`, cx: hx, cy: y(v), r: 4 }));
        });
      }
      const overlay = s('rect', { x: left, y: top, width: plotW, height: plotH, fill: 'transparent' });
      nodes.push(overlay);
      put(svg, ...nodes);
      geometry = { left, plotW, start, end, visible, src };
      overlay.addEventListener('pointermove', onMove);
      overlay.addEventListener('pointerleave', onLeave);
    }

    function onMove(event) {
      if (!geometry) return;
      const rect = svg.getBoundingClientRect();
      const scale = rect.width / (svg.viewBox.baseVal.width || rect.width);
      const px = (event.clientX - rect.left) / scale;
      const t = geometry.start + ((px - geometry.left) / geometry.plotW) * (geometry.end - geometry.start);
      state.hover = t;
      hoverOwner = api_;
      lastPointer = { x: event.clientX, y: event.clientY };
      redrawCharts();
    }

    function onLeave() {
      state.hover = null;
      hoverOwner = null;
      tooltip().classList.add('hidden');
      redrawCharts();
    }

    function showTooltip() {
      if (!geometry || state.hover === null) return;
      const p = nearest(geometry.visible, state.hover);
      const tip = tooltip();
      if (!p) {
        tip.classList.add('hidden');
        return;
      }
      put(tip, h('div', { class: 't num', text: fmtClock(p.t) }),
        spec.series.map((se, i) => h('div', { class: 'row' },
          h('span', { class: 'key' }, h('span', { class: `swatch sw-${i + 1}` }), se.label),
          h('span', { class: 'num', text: fmt(stat(p, se.key, se.stats)) }))),
        (spec.refs || []).map((r) => {
          const v = r.value(geometry.src);
          return Number.isFinite(v) ? h('div', { class: 'row' }, h('span', { class: 'key' }, h('span', { class: 'swatch dashed sw-ref' }), r.label), h('span', { class: 'num', text: fmt(v) })) : null;
        }));
      positionTooltip(tip);
    }

    const api_ = { card, draw, showTooltip };
    return api_;
  }

  let hoverOwner = null;
  let lastPointer = { x: 0, y: 0 };
  let redrawCharts = () => {};

  function positionTooltip(tip) {
    tip.classList.remove('hidden');
    const w = tip.offsetWidth;
    const hgt = tip.offsetHeight;
    let left = lastPointer.x + 14;
    if (left + w > window.innerWidth - 8) left = lastPointer.x - w - 14;
    let topPos = lastPointer.y + 14;
    if (topPos + hgt > window.innerHeight - 8) topPos = lastPointer.y - hgt - 14;
    tip.style.left = `${Math.max(8, left)}px`;
    tip.style.top = `${Math.max(8, topPos)}px`;
  }

  function nearest(points, t) {
    let best = null;
    let bestD = Infinity;
    for (const p of points) {
      const d = Math.abs(p.t + p.w / 2 - t);
      if (d < bestD) {
        bestD = d;
        best = p;
      }
    }
    return best && bestD <= Math.max(2000, best.w) ? best : null;
  }

  /* Verdict per window as a strip: when it went wrong, and for how long. */
  function healthTimeline() {
    const svg = s('svg', { class: 'plot', height: 34, role: 'img', 'aria-label': 'Verdict over time' });
    const legend = h('div', { class: 'legend' },
      VERDICT_ORDER.slice().reverse().map((v) => h('span', { class: 'key' }, verdictBadge(v, 12))),
      h('span', { class: 'key faint', text: 'blank: no data' }));
    const card = h('div', { class: 'card timeline' }, h('h2', { text: 'Verdict over time' }), legend, svg);
    let geometry = null;

    function draw(src, points, start, end) {
      const width = Math.max(240, svg.clientWidth || 600);
      const left = 64;
      const right = 10;
      const plotW = width - left - right;
      svg.setAttribute('viewBox', `0 0 ${width} 34`);
      const x = (t) => left + ((t - start) / (end - start)) * plotW;
      const visible = points.filter((p) => p.t + p.w >= start && p.t <= end && p.verdict);
      const nodes = [s('rect', { class: 'gap', x: left, y: 4, width: plotW, height: 14, rx: 3 })];
      /* Merge runs of one verdict into one rectangle. */
      let run = null;
      const flush = () => {
        if (!run) return;
        const x0 = Math.max(left, x(run.t0));
        const x1 = Math.min(left + plotW, x(run.t1));
        if (x1 > x0) nodes.push(s('rect', { class: `v-${VERDICT_LABEL[run.v] ? run.v : 'unknown'}`, x: x0, y: 4, width: Math.max(1, x1 - x0), height: 14 }));
        run = null;
      };
      for (const p of visible) {
        if (run && run.v === p.verdict && p.t - run.t1 <= 1500) run.t1 = p.t + p.w;
        else {
          flush();
          run = { v: p.verdict, t0: p.t, t1: p.t + p.w };
        }
      }
      flush();
      const axis = s('g', { class: 'axis' });
      const stepMs = timeStep(end - start);
      for (let t = Math.ceil(start / stepMs) * stepMs; t <= end; t += stepMs) {
        axis.append(s('text', { x: x(t), y: 32, 'text-anchor': 'middle', text: fmtClock(t, stepMs < 60e3) }));
      }
      nodes.push(axis);
      const hoverPoint = state.hover !== null ? nearest(visible, state.hover) : null;
      if (hoverPoint) {
        const hx = x(hoverPoint.t + hoverPoint.w / 2);
        nodes.push(s('line', { class: 'crosshair', x1: hx, x2: hx, y1: 0, y2: 22 }));
      }
      const overlay = s('rect', { x: left, y: 0, width: plotW, height: 22, fill: 'transparent' });
      nodes.push(overlay);
      put(svg, ...nodes);
      geometry = { left, plotW, start, end, visible };
      overlay.addEventListener('pointermove', (event) => {
        const rect = svg.getBoundingClientRect();
        const scale = rect.width / (svg.viewBox.baseVal.width || rect.width);
        const px = (event.clientX - rect.left) / scale;
        state.hover = geometry.start + ((px - geometry.left) / geometry.plotW) * (geometry.end - geometry.start);
        hoverOwner = api_;
        lastPointer = { x: event.clientX, y: event.clientY };
        redrawCharts();
      });
      overlay.addEventListener('pointerleave', () => {
        state.hover = null;
        hoverOwner = null;
        tooltip().classList.add('hidden');
        redrawCharts();
      });
    }

    function showTooltip() {
      if (!geometry || state.hover === null) return;
      const p = nearest(geometry.visible, state.hover);
      const tip = tooltip();
      if (!p) {
        put(tip, h('div', { class: 't', text: 'No data' }));
        positionTooltip(tip);
        return;
      }
      put(tip, h('div', { class: 't num', text: fmtClock(p.t) }), verdictBadge(p.verdict, 12),
        p.reason && p.reason !== 'none' ? h('div', { class: 'muted', text: REASON_SHORT[p.reason] || p.reason }) : null);
      positionTooltip(tip);
    }

    const api_ = { card, draw, showTooltip };
    return api_;
  }

  function chartSpecs(role) {
    if (role !== 'sender' && role !== 'receiver') {
      /* Unknown role: only what both ends share. */
      return [{
        title: 'Round-trip time',
        unit: 's',
        series: [{ key: 'srt.connection.rtt', stats: ['mean', 'max', 'last'], label: 'RTT' }],
      }];
    }
    if (role === 'sender') {
      return [
        {
          title: 'Throughput',
          unit: 'bit/s',
          series: [
            { key: 'srt.sender.unique_sent.bitrate', stats: ['rate', 'mean'], label: 'Original' },
            { key: 'srt.sender.retransmitted.bitrate', stats: ['rate', 'mean'], label: 'Retransmitted' },
          ],
        },
        {
          title: 'Loss and recovery',
          unit: 'pkt/s',
          series: [
            { key: 'srt.sender.loss_reported.packet_rate', stats: ['rate'], label: 'Reported lost' },
            { key: 'srt.sender.dropped.packet_rate', stats: ['rate'], label: 'Dropped (too late)' },
            { key: 'srt.sender.retransmitted.packet_rate', stats: ['rate'], label: 'Retransmitted' },
          ],
        },
        {
          title: 'Timing',
          unit: 's',
          series: [
            { key: 'srt.connection.rtt', stats: ['mean', 'max', 'last'], label: 'RTT' },
            { key: 'srt.sender.buffer.duration', stats: ['max', 'mean'], label: 'Send buffer' },
          ],
          refs: [{ label: 'Latency', value: (src) => latencyOf(src) }],
        },
        {
          title: 'Margins',
          unit: 'ratio',
          minMax: 1,
          series: [
            { key: 'srt.sender.buffer.drop_threshold_utilization', stats: ['max', 'mean'], label: 'Drop limit used' },
            { key: 'srt.sender.flow_window.utilization', stats: ['max', 'mean'], label: 'Flow window used' },
          ],
        },
      ];
    }
    return [
      {
        title: 'Throughput',
        unit: 'bit/s',
        series: [
          { key: 'srt.receiver.unique_received.bitrate', stats: ['rate', 'mean'], label: 'Delivered (distinct packets)' },
        ],
      },
      {
        title: 'Loss and recovery',
        unit: 'pkt/s',
        series: [
          { key: 'srt.receiver.lost.packet_rate', stats: ['rate'], label: 'Lost (before repair)' },
          { key: 'srt.receiver.dropped.packet_rate', stats: ['rate'], label: 'Unrecovered' },
          { key: 'srt.receiver.retransmitted.packet_rate', stats: ['rate'], label: 'Repairs received' },
        ],
      },
      {
        title: 'Timing',
        unit: 's',
        series: [
          { key: 'srt.connection.rtt', stats: ['mean', 'max', 'last'], label: 'RTT' },
          { key: 'srt.receiver.buffer.duration', stats: ['min', 'mean'], label: 'Receive buffer' },
        ],
        refs: [{ label: 'Latency', value: (src) => latencyOf(src) }],
      },
      {
        title: 'Recovery headroom',
        unit: 'ratio',
        minMax: 1,
        series: [{ key: 'srt.receiver.recovery_headroom', stats: ['min', 'mean'], label: 'Headroom (buffer ÷ latency)' }],
      },
    ];
  }

  // ------------------------------------------------------------ source view

  function tile(label, value, sub) {
    return h('div', { class: 'card tile' },
      h('div', { class: 'k', text: label }),
      h('div', { class: 'v num', text: value }),
      sub ? h('div', { class: 's num', text: sub }) : null);
  }

  function tilesFor(src) {
    const rtt = windowStat(src, 'srt.connection.rtt', 'mean', 'last');
    const rttMax = windowStat(src, 'srt.connection.rtt', 'max');
    const latency = latencyOf(src);
    const ratio = rtt && latency ? `${fmtNumber(latency / rtt, 1)} × RTT` : '';
    const counters = src.counters || {};
    const list = [];
    if (src.role !== 'sender' && src.role !== 'receiver') {
      list.push(tile('RTT', fmtDuration(rtt), rttMax !== null ? `max ${fmtDuration(rttMax)}` : ''));
      list.push(tile('Latency', fmtDuration(latency), ratio));
    } else if (src.role === 'sender') {
      const unique = windowStat(src, 'srt.sender.unique_sent.bitrate', 'rate');
      const total = windowStat(src, 'srt.sender.sent.bitrate', 'rate');
      const retx = windowStat(src, 'srt.sender.retransmitted.bitrate', 'rate');
      list.push(tile('Sending', fmtBitrate(unique), total !== null ? `${fmtBitrate(total)} on the wire` : ''));
      list.push(tile('Retransmissions', total ? fmtPercent((retx || 0) / total) : '—',
        fmtPacketRate(windowStat(src, 'srt.sender.retransmitted.packet_rate', 'rate'))));
      list.push(tile('Loss reported', fmtPercent(lossShare(src)), fmtPacketRate(windowStat(src, 'srt.sender.loss_reported.packet_rate', 'rate'))));
      list.push(tile('Dropped (too late)', fmtPacketRate(dropRate(src)), `${fmtCount(counters['srt.sender.dropped.packets'])} since connect`));
      list.push(tile('RTT', fmtDuration(rtt), rttMax !== null ? `max ${fmtDuration(rttMax)}` : ''));
      list.push(tile('Latency (receiver)', fmtDuration(latency), ratio));
      list.push(tile('Send buffer', fmtDuration(windowStat(src, 'srt.sender.buffer.duration', 'max', 'mean')),
        `${fmtPercent(windowStat(src, 'srt.sender.buffer.drop_threshold_utilization', 'max'))} of the drop limit`));
      list.push(tile('Flow window used', fmtPercent(windowStat(src, 'srt.sender.flow_window.utilization', 'max', 'mean')),
        `${fmtCount(windowStat(src, 'srt.sender.flight_size', 'max'))} pkt in flight`));
      const capacity = windowStat(src, 'srt.connection.link_capacity_estimate', 'mean', 'last');
      if (capacity !== null) list.push(tile('Link estimate', fmtBitrate(capacity), 'packet-pair estimate'));
    } else {
      const unique = windowStat(src, 'srt.receiver.unique_received.bitrate', 'rate');
      const total = windowStat(src, 'srt.receiver.received.bitrate', 'rate');
      list.push(tile('Receiving', fmtBitrate(unique), total !== null ? `${fmtBitrate(total)} on the wire` : ''));
      list.push(tile('Loss before repair', fmtPercent(lossShare(src)), fmtPacketRate(windowStat(src, 'srt.receiver.lost.packet_rate', 'rate'))));
      list.push(tile('Unrecovered', fmtPacketRate(dropRate(src)), `${fmtCount(counters['srt.receiver.dropped.packets'])} since connect`));
      list.push(tile('Repairs received', fmtPacketRate(windowStat(src, 'srt.receiver.retransmitted.packet_rate', 'rate')),
        `${fmtCount(counters['srt.receiver.belated.packets'])} arrived too late`));
      list.push(tile('RTT', fmtDuration(rtt), rttMax !== null ? `max ${fmtDuration(rttMax)}` : ''));
      list.push(tile('Latency', fmtDuration(latency), ratio));
      list.push(tile('Receive buffer', fmtDuration(windowStat(src, 'srt.receiver.buffer.duration', 'min', 'mean')),
        `headroom ${fmtPercent(windowStat(src, 'srt.receiver.recovery_headroom', 'min', 'mean'))}`));
      const undecryptable = counters['srt.receiver.undecryptable.packets'];
      if (undecryptable) list.push(tile('Undecryptable', fmtCount(undecryptable), 'packets since connect'));
      const fec = windowStat(src, 'srt.receiver.fec_recovered.packet_rate', 'rate');
      if (fec) list.push(tile('FEC rebuilt', fmtPacketRate(fec), `${fmtPacketRate(windowStat(src, 'srt.receiver.fec_unrecovered.packet_rate', 'rate'))} not rebuilt`));
    }
    return list;
  }

  function sourceView(id) {
    const crumbs = h('div', { class: 'crumbs' }, h('a', { href: '#/', text: '← All endpoints' }));
    const head = h('div', { class: 'source-head' });
    const hero = h('div', { class: 'card hero' });
    const tiles = h('div', { class: 'tiles' });
    const indicators = h('div', { class: 'indicators' });
    const missing = h('p', { class: 'muted hidden' });
    const rangeGroup = h('div', { class: 'segmented', role: 'group', 'aria-label': 'Time range' });
    const rangeButtons = [];
    for (const [seconds, label] of [[300, '5 min'], [900, '15 min'], [3600, '1 h']]) {
      const b = h('button', { type: 'button', text: label });
      b.addEventListener('click', () => {
        state.rangeSeconds = seconds;
        render();
      });
      rangeButtons.push([seconds, b]);
      rangeGroup.append(b);
    }
    const timeline = healthTimeline();
    const chartsBox = h('div', { class: 'charts' });
    let charts = [];
    let chartsRole = null;
    const eventsBody = h('tbody');
    const eventsSection = h('section', { class: 'section' },
      h('div', { class: 'section-head' }, h('h2', { text: 'Events' })),
      h('div', { class: 'card events' }, h('table', {}, eventsBody)));
    const metricsBody = h('tbody');
    const countersBody = h('tbody');
    const infoBody = h('tbody');
    const details = h('details', { class: 'card' },
      h('summary', { text: 'All metrics, counters and source details' }),
      h('div', { class: 'grid-2' },
        h('div', {}, h('table', {},
          h('thead', {}, h('tr', {}, h('th', { text: 'Metric (current window)' }), h('th', { class: 'r', text: 'Rate / last' }), h('th', { class: 'r', text: 'Min' }), h('th', { class: 'r', text: 'Mean' }), h('th', { class: 'r', text: 'Max' }), h('th', { class: 'r', text: 'p95' }))),
          metricsBody)),
        h('div', {},
          h('table', { class: 'kv' }, h('thead', {}, h('tr', {}, h('th', { text: 'Source' }), h('th', { text: '' }))), infoBody),
          h('table', { class: 'kv' }, h('thead', {}, h('tr', {}, h('th', { text: 'Counter (since connect)' }), h('th', { class: 'r', text: 'Value' }))), countersBody))));
    details.addEventListener('toggle', () => render());
    const body = h('div', {},
      hero,
      h('section', { class: 'section' }, h('div', { class: 'section-head' }, h('h2', { text: 'Now' }), h('span', { class: 'faint', id: 'window-note' })), tiles),
      h('section', { class: 'section' }, h('div', { class: 'section-head' }, h('h2', { text: 'Health indicators' })), indicators, missing),
      h('section', { class: 'section' }, h('div', { class: 'section-head' }, h('h2', { text: 'History' }), h('span', { class: 'faint', text: `Times are local (${timeZoneLabel()})` }), rangeGroup), timeline.card, h('div', { class: 'section' }, chartsBox)),
      eventsSection,
      h('section', { class: 'section' }, details));
    const gone = h('div', { class: 'card empty hidden' }, 'This source is no longer reported. ', h('a', { href: '#/', text: 'Back to all endpoints' }));
    const root = h('div', {}, crumbs, head, gone, body);

    const resize = 'ResizeObserver' in window ? new ResizeObserver(() => redrawCharts()) : null;
    resize?.observe(chartsBox);

    function drawCharts() {
      const src = state.sources.get(id);
      if (!src) return;
      const points = seriesOf(id).points;
      const end = Math.max(state.generatedMs || 0, points.length ? points[points.length - 1].t + points[points.length - 1].w : 0) || Date.now();
      /* Until the data covers the range, fit it (at least a minute). */
      const first = points.length ? points[0].t : end;
      const start = Math.max(end - state.rangeSeconds * 1000, Math.min(first, end - 60 * 1000));
      timeline.draw(src, points, start, end);
      for (const c of charts) c.draw(src, points, start, end);
      if (hoverOwner && state.hover !== null) hoverOwner.showTooltip();
    }

    function update() {
      const src = state.sources.get(id);
      gone.classList.toggle('hidden', Boolean(src) || state.sources.size === 0);
      body.classList.toggle('hidden', !src);
      if (!src) {
        put(head, h('h1', { text: `Source ${id}` }));
        return;
      }
      loadHistory(id);
      const { name, key } = sourceName(src);
      document.title = `${name} · ${src.role} · SRT Telemetry`;
      put(head, h('h1', { text: name }), h('span', { class: 'role', text: src.role }),
        h('span', { class: 'labels' }, sourceChips(src, key)),
        h('span', { class: 'faint', text: `${src.connection || 'unreported'}${src.up ? '' : ' · statistics read failing'}` }));

      const health = src.health;
      const v = VERDICT_LABEL[health?.verdict] ? health.verdict : 'unknown';
      const a = advice(health, src.role);
      hero.className = `card hero v-${v}`;
      put(hero,
        h('div', { class: 'state' },
          h('div', { class: 'big' }, verdictBadge(v, 26)),
          h('div', { class: 'reason', text: a.why }),
          h('div', { class: 'meta num' },
            health && Number.isFinite(health.verdict_age_seconds) ? `Held for ${fmtAge(health.verdict_age_seconds)}` : '',
            health?.primary_indicator && health.primary_indicator !== 'none' && v !== 'ok'
              ? ` · from the indicator ${INDICATORS[health.primary_indicator]?.name || health.primary_indicator}` : '')),
        h('div', { class: 'act' },
          h('div', { class: 'label', text: v === 'ok' || v === 'watch' || v === 'unknown' ? 'Action' : 'Suggested change' }),
          h('div', { class: 'do', text: a.action }),
          a.hint ? h('div', { class: 'note', text: a.hint }) : null,
          health?.knob && health.knob !== 'none' && v !== 'watch'
            ? h('div', { class: 'note', text: src.role === 'sender' ? 'Advice from what this sender sees; the receiver may see more.' : 'Advice from what this receiver sees; the sender may see more.' })
            : null));

      put(tiles, ...tilesFor(src));
      const note = document.getElementById('window-note');
      if (note && src.window) note.textContent = `${fmtClock(src.window.start_unix_ms)}–${fmtClock(src.window.end_unix_ms)}${src.window.quality?.length ? ` · ${src.window.quality.join(', ')}` : ''}`;

      put(indicators, ...(health?.indicators || []).map((ind) => {
        const meta = INDICATORS[ind.name] || { name: clean(ind.name), desc: '', fmt: (x) => fmtNumber(x, 3) };
        const iv = VERDICT_LABEL[ind.state] ? ind.state : 'unknown';
        return h('div', { class: `card indicator v-${iv}` },
          h('div', { class: 'top-line' }, h('span', { class: 'name', text: meta.name }), verdictBadge(iv, 12)),
          h('div', { class: 'value num', text: ind.value === null ? '—' : meta.fmt(ind.value, src.role) }),
          h('div', { class: 'desc', text: `${typeof meta.desc === 'function' ? meta.desc(src.role) : meta.desc}${ind.reason && ind.reason !== 'none' && iv !== 'ok' ? ` Now: ${REASON_SHORT[ind.reason] || clean(ind.reason)}.` : ''}` }),
          ind.required ? h('div', { class: 'req', text: 'Required for OK' }) : null);
      }));
      const missingList = health?.missing_metrics || [];
      missing.classList.toggle('hidden', missingList.length === 0);
      missing.textContent = missingList.length ? `Not reported by this SRT library: ${missingList.join(', ')}.` : '';

      for (const [seconds, b] of rangeButtons) b.setAttribute('aria-pressed', state.rangeSeconds === seconds ? 'true' : 'false');
      if (chartsRole !== src.role) {
        chartsRole = src.role;
        charts = chartSpecs(src.role).map(lineChart);
        put(chartsBox, ...charts.map((c) => c.card));
      }
      drawCharts();

      eventsSection.classList.toggle('hidden', !state.journal);
      renderEvents(eventsBody, state.events.filter((e) => e.source_id === id), false, 200);

      if (details.open) {
        put(metricsBody, ...Object.entries(src.window?.metrics || {}).map(([k, m]) =>
          h('tr', {},
            h('td', { class: 'mono', text: k }),
            h('td', { class: 'r num', text: fmtUnit(m.unit, m.rate ?? m.last) }),
            h('td', { class: 'r num', text: fmtUnit(m.unit, m.min) }),
            h('td', { class: 'r num', text: fmtUnit(m.unit, m.mean) }),
            h('td', { class: 'r num', text: fmtUnit(m.unit, m.max) }),
            h('td', { class: 'r num', text: m.p95 === undefined ? '' : fmtUnit(m.unit, m.p95) }))));
        put(countersBody, ...Object.entries(src.counters || {}).map(([k, value]) =>
          h('tr', {}, h('td', { class: 'mono', text: k }), h('td', { class: 'r num', text: fmtCount(value) }))));
        put(infoBody,
          h('tr', {}, h('td', { text: 'Source ID' }), h('td', { class: 'num', text: String(src.id) })),
          h('tr', {}, h('td', { text: 'Role' }), h('td', { text: src.role })),
          h('tr', {}, h('td', { text: 'Connection' }), h('td', { text: src.connection || 'unreported' })),
          h('tr', {}, h('td', { text: 'Statistics' }), h('td', { text: src.up ? 'reading' : 'read failing' })),
          h('tr', {}, h('td', { text: 'Window' }), h('td', { class: 'num', text: src.window ? `${src.window.samples} samples${src.window.quality?.length ? `, ${src.window.quality.join(', ')}` : ''}` : 'none yet' })),
          ...Object.entries(src.labels || {}).map(([k, val]) => h('tr', {}, h('td', { text: k }), h('td', { text: val }))));
      }
    }

    redrawCharts = drawCharts;
    state.viewedId = id;
    return {
      kind: 'source',
      id,
      root,
      update,
      destroy() {
        resize?.disconnect();
        /* Back to the fleet: keep two minutes, reload history next time. */
        state.viewedId = null;
        state.historyLoaded.delete(id);
        const pts = state.series.get(id)?.points;
        if (pts && pts.length > FLEET_POINTS) pts.splice(0, pts.length - FLEET_POINTS);
        redrawCharts = () => {};
        state.hover = null;
        hoverOwner = null;
        tooltip().classList.add('hidden');
        document.title = 'SRT Telemetry';
      },
    };
  }

  // ------------------------------------------------------------ start

  function applyTheme(theme) {
    if (theme === 'dark' || theme === 'light') document.documentElement.dataset.theme = theme;
    else delete document.documentElement.dataset.theme;
  }

  function setupTheme() {
    applyTheme(readStored('local', 'srt-telemetry-theme'));
    document.getElementById('theme').addEventListener('click', () => {
      const current = document.documentElement.dataset.theme ||
        (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
      const next = current === 'dark' ? 'light' : 'dark';
      writeStored('local', 'srt-telemetry-theme', next);
      applyTheme(next);
    });
  }

  let startTimer = null;

  async function start() {
    if (startTimer) clearTimeout(startTimer);
    startTimer = null;
    state.started = false;
    state.locked = false;
    state.retry = 1000;
    closeFeed();
    stopPolling();
    setLink('connecting', 'Connecting');
    try {
      await loadSnapshot();
    } catch (error) {
      if (error instanceof AuthError) {
        lock(Boolean(state.token));
        return;
      }
      setLink('offline', 'Offline');
      document.getElementById('app').replaceChildren(h('div', { class: 'card empty' },
        'The telemetry server did not answer. Retrying…'));
      startTimer = setTimeout(start, 3000);
      return;
    }
    state.started = true;
    view = null;
    render();
    try {
      await loadEvents();
    } catch {
      /* the feed and polling fetch them later */
    }
    render();
    connectFeed();
  }

  function main() {
    for (const role of ['sender', 'receiver', 'unknown']) {
      for (const spec of chartSpecs(role)) for (const se of spec.series) keep(se.key, se.stats);
    }
    setupTheme();
    state.token = readTokenFromHash();
    window.addEventListener('hashchange', () => {
      if (location.hash.startsWith('#token=')) {
        state.token = readTokenFromHash();
        start();
        return;
      }
      window.scrollTo(0, 0);
      render();
    });
    setInterval(renderBanner, 1000);
    setInterval(watchdog, 1000);
    start();
  }

  main();
})();
