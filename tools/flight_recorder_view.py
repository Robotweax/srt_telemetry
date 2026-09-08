#!/usr/bin/env python3
"""Render a validated Flight Recorder case as an offline, dependency-free HTML file.

This module presents the existing incident analyzer's results. It neither infers
new diagnoses nor synchronizes clocks. Package validation belongs to the caller.
All case data is escaped as text; no case data is evaluated as HTML or JavaScript.
"""

import hashlib
import html
import json
import math
from collections import OrderedDict


HYPOTHESIS_LABELS = {
    "transmission-path-loss-with-recovery": "Paketverlust auf dem Übertragungsweg mit Wiederherstellung",
    "transmission-path-rtt-step-with-recovery": "Erhöhte Umlaufzeit auf dem Übertragungsweg mit Erholung",
    "sender-side-pressure": "Rückstau am Sender",
    "receiver-side-pressure": "Rückstau am Empfänger",
    "receiver-recovery-margin-exhausted": "Wiederherstellungs-/Playoutreserve am Empfänger ausgeschöpft",
}
NEXT_MEASUREMENT = {
    "transmission-path-loss-with-recovery": "Zeitlich zugeordnete Paketbeobachtungen an beiden Endpunkten ergänzen, um Verlust und Wiederholung einzugrenzen; ein Paketbefund allein belegt keine Anwendungszustellung.",
    "transmission-path-rtt-step-with-recovery": "Die Uhren mit dokumentierter Unsicherheit zuordnen und Paketlaufzeiten an beiden Endpunkten erfassen; RTT allein trennt die Übertragungsrichtungen nicht.",
    "sender-side-pressure": "Angebotene Anwendungsrate und Sendeterminierung zusammen mit der Ausgangskapazität im selben Zeitfenster messen.",
    "receiver-side-pressure": "Lesezeitpunkte und Pausen der Empfängeranwendung zusammen mit der Empfangspufferbelegung aufzeichnen.",
    "receiver-recovery-margin-exhausted": "Empfänger-Drops, konfigurierte Latenz und Anwendungszustellung zeitlich zuordnen; eine größere Latenz erst in einem kontrollierten Vergleich prüfen.",
}

# Display metadata uses the public telemetry field bits; it is not a second
# telemetry adapter. Missing values and explicitly unavailable fields stay null.
METRICS = (
    ("RTT · ms", "raw", "rtt_seconds", 15, 1000),
    ("Verlust Sender · Δ pkt", "delta", "sender_loss_packet_delta", 4, 1),
    ("Verlust Empfänger · Δ pkt", "delta", "receiver_loss_packet_delta", 5, 1),
    ("Wiederholt · Δ pkt", "delta", "retransmitted_packet_delta", 6, 1),
    ("Drop Sender · Δ pkt", "delta", "sender_drop_packet_delta", 7, 1),
    ("Drop Empfänger · Δ pkt", "delta", "receiver_drop_packet_delta", 8, 1),
    ("Sendepuffer · B", "raw", "send_buffer_bytes", 21, 1),
    ("Empfangspuffer · B", "raw", "receive_buffer_bytes", 23, 1),
    ("Flight · pkt", "raw", "flight_size_packets", 16, 1),
    ("Konfigurierte TSBPD-Latenz · ms", "raw", "receiver_tsbpd_delay_seconds", 26, 1000),
)
QUALITY_FLAGS = {
    1: "first-sample",
    2: "monotonic-clock-regression",
    4: "provider-changed",
    8: "counter-reset",
}


def _text(value):
    if value is None:
        return "—"
    if isinstance(value, (dict, list)):
        return json.dumps(value, ensure_ascii=False, sort_keys=True)
    return str(value)


def _escape(value):
    return html.escape(_text(value), quote=True)


def _list(items, empty="Keine Angaben vorhanden."):
    if not items:
        return '<p class="muted">' + _escape(empty) + "</p>"
    return "<ul>" + "".join("<li>" + _escape(item) + "</li>" for item in items) + "</ul>"


def _json_details(value, label="Vollständige Quelldaten"):
    return (
        '<details class="source"><summary>' + _escape(label) + "</summary><pre>"
        + _escape(json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2))
        + "</pre></details>"
    )


def _number(value):
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        return '<span class="unavailable" title="Nicht verfügbar">—</span>'
    if isinstance(value, float) and not math.isfinite(value):
        return '<span class="unavailable" title="Nicht verfügbar">—</span>'
    return _escape(value if isinstance(value, int) else format(value, ".6g"))


def _metric_value(sample, metric):
    _, section, field, bit, factor = metric
    values = sample.get(section, {}) or {}
    available = values.get("available_fields" if section == "raw" else "available_delta_fields")
    if isinstance(available, int) and not available & (1 << bit):
        return None
    value = values.get(field)
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        return None
    return value * factor


def _quality(sample):
    quality = list(sample.get("quality", []) or [])
    flags = (sample.get("delta", {}) or {}).get("quality_flags", 0)
    if isinstance(flags, int):
        quality += [label for bit, label in QUALITY_FLAGS.items() if flags & bit]
    return list(dict.fromkeys(_text(item) for item in quality))


def _badge(value, kind=""):
    return '<span class="badge ' + kind + '">' + _escape(value) + "</span>"


def _location(item):
    parts = []
    for key, label in (("endpointId", "Endpunkt"), ("connectionEpoch", "Epoche"),
                       ("sampleIndex", "Sample"), ("windowIndex", "Fenster"),
                       ("domainId", "Zeitdomäne"), ("timeNano", "Zeit / ns")):
        if item.get(key) is not None:
            parts.append(label + ": " + _text(item[key]))
    return " · ".join(parts) or "Zeitliche Zuordnung nicht angegeben"


def _events(items, title, empty):
    cards = []
    for item in items:
        if not isinstance(item, dict):
            cards.append('<article class="event">' + _escape(item) + "</article>")
            continue
        label = item.get("label") or item.get("reason") or item.get("kind") or item.get("type") or title
        cards.append(
            '<article class="event"><strong>' + _escape(label) + '</strong><p class="muted">'
            + _escape(_location(item)) + "</p>" + _json_details(item, "Ereignisdetails") + "</article>"
        )
    return '<section class="panel"><h2>' + _escape(title) + "</h2>" + (
        "".join(cards) if cards else '<p class="muted">' + _escape(empty) + "</p>"
    ) + "</section>"


def _endpoints(manifest, recordings):
    output = []
    for session in manifest.get("sessions", []):
        rows = []
        for endpoint in session.get("endpoints", []):
            recording = recordings.get(endpoint.get("recording"), {}) or {}
            header = recording.get("header", {}) or {}
            clock = endpoint.get("clock", {}) or {}
            cells = [endpoint.get("endpointId"), endpoint.get("role"), endpoint.get("connectionEpoch"),
                     header.get("configuredProvider", endpoint.get("provider")), header.get("providerVersion"),
                     clock.get("kind"), clock.get("domainId"), clock.get("uncertaintyNano")]
            rows.append("<tr>" + "".join("<td>" + _escape(cell) + "</td>" for cell in cells) + "</tr>")
        output.append(
            '<h3>Sitzung <span class="mono">' + _escape(session.get("sessionId")) + "</span></h3>"
            '<div class="table-scroll" tabindex="0" role="region" aria-label="Endpunkte der Sitzung">'
            '<table><thead><tr><th>Endpunkt</th><th>Rolle</th><th>Epoche</th><th>Provider</th>'
            '<th>Version</th><th>Uhr</th><th>Zeitdomäne</th><th>Unsicherheit · ns</th></tr></thead><tbody>'
            + "".join(rows) + "</tbody></table></div>" + _json_details(session, "Sitzung, Konfiguration und Uhrzuordnung")
        )
    return '<section class="panel"><h2>Sitzungen &amp; Endpunkte</h2>' + (
        "".join(output) or '<p class="muted">Keine Endpunkte vorhanden.</p>'
    ) + "</section>"


def _timeline(case, qualified):
    coordinated = (case.get("assessment", {}) or {}).get("timePolicy") == "coordinator-sample-windows"
    lanes = OrderedDict()
    sessions = {
        (_text(endpoint.get("endpointId")), _text(endpoint.get("connectionEpoch"))): session.get("sessionId")
        for session in case.get("manifest", {}).get("sessions", [])
        for endpoint in session.get("endpoints", [])
    }
    for sample in case.get("timeline", []):
        endpoint_id, epoch = _text(sample.get("endpointId")), _text(sample.get("connectionEpoch"))
        session_id = sample.get("sessionId", sessions.get((endpoint_id, epoch)))
        key = (_text(session_id), endpoint_id, _text(sample.get("role")), epoch, _text(sample.get("domainId")))
        lanes.setdefault(key, []).append(sample)
    triggers = case.get("manifest", {}).get("triggers", [])
    output = []
    for lane_index, (key, samples) in enumerate(lanes.items()):
        session_id, endpoint_id, role, epoch, domain_id = key
        rows = []
        markers = []
        for row_index, sample in enumerate(samples):
            sample_index = sample.get("sampleIndex")
            quality = _quality(sample)
            matching = [trigger for trigger in triggers if isinstance(trigger, dict)
                        and trigger.get("sampleIndex") is not None
                        and trigger.get("sampleIndex") == sample_index
                        and (trigger.get("endpointId") == sample.get("endpointId")
                             or (qualified and trigger.get("endpointId") is None))
                        and (trigger.get("connectionEpoch") is None
                             or trigger.get("connectionEpoch") == sample.get("connectionEpoch"))]
            marked = bool(matching)
            row_id = "sample-" + str(lane_index) + "-" + str(row_index)
            marker_title = "Sample " + _text(sample_index)
            marker_title += " · Trigger" if marked else ""
            marker_title += " · " + ", ".join(quality) if quality else ""
            markers.append('<a href="#' + row_id + '" class="sample-marker' + (" trigger" if marked else "")
                           + (" boundary" if quality else "") + '" title="' + _escape(marker_title)
                           + '"><span class="sr-only">' + _escape(marker_title) + "</span></a>")
            flags = " ".join(_badge(flag, "warn") for flag in quality)
            if marked:
                flags = _badge("Trigger", "trigger") + " " + flags
            cells = ["<td>" + _escape(sample_index) + "</td>"]
            if qualified:
                cells.append("<td>" + _escape(sample.get("windowIndex", sample_index)) + "</td>")
            if coordinated:
                cells.extend("<td>" + _number(sample.get(field)) + "</td>"
                             for field in ("windowStartNano", "windowEndNano"))
            cells += ["<td>" + _number(sample.get("timeNano")) + "</td>",
                      "<td>" + _number(sample.get("uncertaintyNano")) + "</td>",
                      "<td>" + (flags or '<span class="muted">—</span>') + "</td>"]
            cells.extend("<td>" + _number(_metric_value(sample, metric)) + "</td>" for metric in METRICS)
            cells.append("<td>" + _json_details({"raw": sample.get("raw", {}), "delta": sample.get("delta", {})}, "Messwertdetails") + "</td>")
            rows.append('<tr id="' + row_id + '"' + (' class="marked"' if marked else "") + ">" + "".join(cells) + "</tr>")
        heading = endpoint_id + " · " + role + " · Epoche " + epoch
        output.append(
            '<article class="lane"><h3>' + _escape(heading) + '</h3><p class="muted">'
            + _escape("Sitzung " + session_id + " · Zeitdomäne " + domain_id + " · " + str(len(samples)) + " Samples")
            + '</p><div class="sample-strip" aria-label="Samples in Aufzeichnungsreihenfolge">' + "".join(markers) + "</div>"
            '<div class="table-scroll" tabindex="0" role="region" aria-label="Messwerte je Sample">'
            '<table class="metrics"><thead><tr><th>Sample</th>' + ("<th>Logisches Fenster</th>" if qualified else "")
            + ("<th>Erfassungsfenster Start · ns</th><th>Erfassungsfenster Ende · ns</th>" if coordinated else "")
            + '<th>Lokale Zeit · ns</th><th>Unsicherheit · ns</th><th>Qualität / Grenze</th>'
            + "".join('<th title="' + _escape(metric[2]) + '">' + _escape(metric[0]) + "</th>" for metric in METRICS)
            + "<th>Quelle</th></tr></thead><tbody>" + "".join(rows) + "</tbody></table></div></article>"
        )
    return (
        '<section class="panel"><h2>Timeline &amp; Messwerte</h2><p class="muted">'
        'Jede Markierung steht für ein Sample in Aufzeichnungsreihenfolge. Die Abstände bilden keine Zeitdauer ab. '
        'Orange markiert Trigger; ein Rand markiert Qualitäts- oder Intervallgrenzen. '
        '— bedeutet nicht verfügbar, keine ausgewiesene Unsicherheit oder kein anwendbarer Wert; niemals eine gemessene Null. '
        'Δ pkt sind Intervalländerungen der Paketzähler. Die konfigurierte TSBPD-Latenz ist keine gemessene verbleibende Reserve oder Medienqualität.'
        '</p>' + ("".join(output) or '<p class="muted">Keine Messwerte vorhanden.</p>') + "</section>"
    )


def _findings(case, supported):
    analysis = case.get("analysis", {}) or {}
    diagnosis = analysis.get("diagnosis", analysis) or {}
    selected = diagnosis.get("selectedHypothesis", {}) or {}
    hypotheses = analysis.get("hypotheses", diagnosis.get("hypotheses", [])) or []
    timeline = analysis.get("timeline", {}) or {}
    assessment = case.get("assessment", {}) or {}
    selected_id = selected.get("id") if supported else None
    label = HYPOTHESIS_LABELS.get(selected_id, selected_id) if supported else "Unzureichende Daten"
    output = '<section class="panel diagnosis-only result"><p class="eyebrow">Gestützte Einordnung</p><h2>' + _escape(label) + "</h2>"
    if supported:
        output += '<p>' + _escape(selected.get("rationale")) + '</p><div class="facts">'
        for key, label in (("firstDeviationSample", "Erstes abweichendes Fenster"),
                           ("lastDeviationSample", "Letztes abweichendes Fenster"), ("recoverySample", "Erholung ab Fenster")):
            output += '<div><span class="muted">' + label + "</span><strong>" + _escape(timeline.get(key)) + "</strong></div>"
        output += "</div><p class=\"muted\">Fenster sind die qualifizierte logische Zuordnung des Testablaufs. Innerhalb eines Fensters ist keine Reihenfolge zwischen den Endpunkten belegt.</p>"
    else:
        output += "<p>Die verfügbaren Beobachtungen tragen keine eindeutige gemeinsame Fehlerzuordnung. Die Messwerte und ihre Grenzen bleiben unten einsehbar.</p>"
    output += _list(assessment.get("reasons", []), "Keine zusätzlichen Einschränkungen der Fallprüfung angegeben.")
    output += "</section><section class=\"panel diagnosis-only\"><h2>Befunde &amp; alternative Erklärungen</h2>"
    if not hypotheses:
        output += '<p class="muted">Keine belastbar auswertbaren Hypothesen vorhanden.</p>'
    for hypothesis in hypotheses:
        hypothesis_id = hypothesis.get("id")
        is_selected = supported and hypothesis_id == selected_id
        title = HYPOTHESIS_LABELS.get(hypothesis_id, hypothesis_id)
        status = "Gestützt" if supported and hypothesis.get("assessment") == "supported" else "Nicht hinreichend belegt"
        output += '<details class="hypothesis"' + (" open" if is_selected else "") + '><summary>'
        output += _badge(status, "good" if is_selected else "") + " " + _escape(title) + "</summary>"
        output += '<p class="muted">Fehlerdomäne: ' + _escape(hypothesis.get("failureDomain")) + "</p>"
        output += "<p>" + _escape(hypothesis.get("summary")) + '</p><div class="evidence-grid"><div><h3>Stützende Beobachtungen</h3>'
        evidence = hypothesis.get("supportingEvidence", [])
        if not evidence:
            output += '<p class="muted">Keine angegeben.</p>'
        for item in evidence:
            if isinstance(item, dict):
                output += '<div class="evidence"><strong>' + _escape(item.get("id")) + "</strong><p>" + _escape(item.get("summary")) + '</p><p class="muted">Logische Samples: ' + _escape(item.get("samples")) + "</p></div>"
            else:
                output += "<p>" + _escape(item) + "</p>"
        output += "</div><div><h3>Widersprechende Beobachtungen</h3>" + _list(hypothesis.get("contradictoryEvidence", []), "Keine im Analyseergebnis angegeben; dies schließt Alternativen nicht aus.")
        output += "<h3>Fehlende Nachweise</h3>" + _list(hypothesis.get("missingEvidence", []), "Keine im Analyseergebnis angegeben.") + "</div></div></details>"
    output += "</section><section class=\"panel diagnosis-only\"><h2>Nächste Messung</h2><p>"
    output += _escape(NEXT_MEASUREMENT.get(selected_id, "Eine vollständige Aufzeichnung beider Endpunkte in derselben Verbindungsepoche mit dokumentierter Uhrzuordnung erfassen. Qualitätsgrenzen und Erfassungslücken zuerst klären; Anwendungsereignisse mitführen."))
    output += "</p><details><summary>Grenzen der Analyse</summary>" + _list(analysis.get("nonClaims", [])) + "</details></section>"
    return output


STYLE = """
:root{color-scheme:light;--ink:#182d36;--muted:#566b75;--line:#dce4e6;--paper:#fff;--teal:#126c70;--orange:#be5c1d}
*{box-sizing:border-box}body{margin:0;background:#f0f4f4;color:var(--ink);font:15px/1.55 system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}main{max-width:1440px;margin:auto;padding:36px 28px 64px}header{border-top:5px solid var(--teal);padding-top:22px;margin-bottom:26px}h1{font-size:clamp(28px,4vw,44px);line-height:1.15;letter-spacing:-1.5px;margin:8px 0 12px}h2{font-size:21px;line-height:1.3;margin:0 0 16px}h3{font-size:15px;margin:20px 0 8px}p{margin:9px 0 14px}li{margin:5px 0}a{color:var(--teal)}.eyebrow{font-size:11px;letter-spacing:2px;font-weight:750;text-transform:uppercase;color:var(--teal)}.muted{color:var(--muted);font-size:13px}.mono,pre{font-family:ui-monospace,SFMono-Regular,Consolas,monospace}.case-id{overflow-wrap:anywhere}.panel{background:var(--paper);border:1px solid var(--line);border-radius:12px;padding:24px;margin-bottom:18px;min-width:0}.result{border-left:5px solid var(--teal)}.notice{background:#e7f0f0;padding:16px 19px;border-radius:8px}.facts{display:flex;gap:32px;flex-wrap:wrap;margin:20px 0}.facts>div{display:grid;gap:3px}.facts strong{font-size:24px}.badge{display:inline-block;max-width:100%;border:1px solid #cad8dc;border-radius:5px;background:#f4f7f7;padding:2px 7px;font-size:11px;line-height:1.5;overflow-wrap:anywhere}.badge.good{background:#e1f2ea;border-color:#96c6b0;color:#185940}.badge.warn{color:#805007;background:#fff4d9;border-color:#e0c386}.badge.trigger{background:#fff0e4;border-color:#d4915a;color:#903e0e}.toolbar{display:none;gap:6px;margin:20px 0}button{font:inherit;padding:9px 16px;border:1px solid #bacbd0;background:white;color:var(--ink);border-radius:7px;cursor:pointer}button[aria-pressed=true]{background:var(--ink);border-color:var(--ink);color:white}button:focus-visible,a:focus-visible,summary:focus-visible,[tabindex]:focus-visible{outline:3px solid #298c9c;outline-offset:3px}.lane{border-top:1px solid var(--line);padding:3px 0 22px}.sample-strip{display:flex;gap:4px;margin:14px 0;flex-wrap:wrap;max-height:120px;overflow:auto}.sample-marker{display:block;background:#a9c8cc;width:17px;height:20px;border-radius:3px;border:2px solid transparent}.sample-marker.trigger{background:#e8873f}.sample-marker.boundary{border-color:#795c1a}.sample-marker:hover{background:var(--teal)}.table-scroll{overflow:auto;max-height:560px;border:1px solid var(--line);border-radius:7px}table{border-collapse:separate;border-spacing:0;width:100%;font-size:12px}th,td{text-align:left;padding:10px 12px;border-bottom:1px solid #e6eded;vertical-align:top}th{position:sticky;top:0;background:#eef4f4;color:#405b65;font-weight:650;z-index:1}td{font-variant-numeric:tabular-nums;overflow-wrap:anywhere}tbody tr:last-child td{border-bottom:0}.metrics th{min-width:105px}.metrics th:first-child{min-width:70px}.metrics td{white-space:nowrap}.metrics td .source{max-width:450px}.metrics td pre{max-width:450px;white-space:pre}.marked td{background:#fff5eb}tr:target td{background:#e3f2f2}.unavailable{color:#81949c}.source{margin:14px 0 0;font-size:12px}.source summary{color:var(--muted)}.metrics .source{margin:0}.evidence-grid{display:grid;grid-template-columns:1fr 1fr;gap:26px}.evidence{border-left:2px solid #acd1cf;padding-left:12px;margin:14px 0}.evidence p{margin:4px 0}.hypothesis{border-top:1px solid var(--line);padding:16px 0}.hypothesis:last-child{padding-bottom:0}summary{cursor:pointer}pre{overflow:auto;max-height:350px;background:#f5f8f8;border:1px solid var(--line);border-radius:6px;padding:14px;font-size:11px;white-space:pre-wrap;overflow-wrap:anywhere}.event{border-left:3px solid #d28b4f;padding:1px 16px;margin:16px 0}.event .source{margin:4px 0}.two-col{display:grid;grid-template-columns:1fr 1fr;gap:18px}.two-col .panel{margin-bottom:18px}.raw-only{display:none}body[data-mode=raw] .diagnosis-only{display:none}body[data-mode=raw] .raw-only{display:block}.sr-only{position:absolute;width:1px;height:1px;padding:0;margin:-1px;overflow:hidden;clip:rect(0,0,0,0);white-space:nowrap;border:0}footer{font-size:12px;color:var(--muted);padding:8px 2px}@media(max-width:700px){main{padding:20px 12px 40px}.panel{padding:18px}.two-col,.evidence-grid{grid-template-columns:1fr}.facts{gap:20px}.toolbar{flex-wrap:wrap}}@media print{body{background:white}main{padding:0}.toolbar,.sample-strip{display:none!important}.panel{break-inside:avoid}.table-scroll{max-height:none;overflow:visible}.metrics{font-size:8px}.metrics th{min-width:0}th,td{padding:4px}details{break-inside:avoid}}
"""

SCRIPT = """
(function () {
  'use strict';
  var toolbar = document.getElementById('view-modes');
  var buttons = toolbar.querySelectorAll('button');
  toolbar.style.display = 'flex';
  buttons.forEach(function (button) {
    button.addEventListener('click', function () {
      var mode = button.getAttribute('data-mode');
      document.body.setAttribute('data-mode', mode);
      buttons.forEach(function (item) {
        item.setAttribute('aria-pressed', String(item === button));
      });
      document.getElementById('mode-status').textContent = mode === 'raw'
        ? 'Statistikansicht: dieselben Messwerte, ohne Befunde und Hypothesen.'
        : 'Fallansicht: Messwerte und nachvollziehbare Befunde.';
    });
  });
}());
"""


def render_case(case: dict) -> str:
    """Return a complete local HTML view of an already validated case dictionary."""
    manifest = case.get("manifest", {}) or {}
    assessment = case.get("assessment", {}) or {}
    analysis = case.get("analysis", {}) or {}
    diagnosis = analysis.get("diagnosis", analysis) or {}
    selected = diagnosis.get("selectedHypothesis", {}) or {}
    coordinated = assessment.get("timePolicy") == "coordinator-sample-windows"
    qualified = coordinated or assessment.get("timePolicy") == "qualified-logical-sample-windows"
    supported = qualified and assessment.get("status") == "supported" and selected.get("assessment") == "supported"
    endpoint_count = sum(len(session.get("endpoints", [])) for session in manifest.get("sessions", []))
    # Hash fixed code, so case strings cannot create executable scripts even if
    # an escaping regression occurs. No network requests or form submissions.
    import base64
    digest = base64.b64encode(hashlib.sha256(SCRIPT.encode("utf-8")).digest()).decode("ascii")
    policy = "default-src 'none'; style-src 'unsafe-inline'; script-src 'sha256-" + digest + "'; connect-src 'none'; img-src 'none'; base-uri 'none'; form-action 'none'"
    body = '<!doctype html><html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">'
    body += '<meta http-equiv="Content-Security-Policy" content="' + _escape(policy) + '"><title>Flight Recorder · ' + _escape(manifest.get("caseId")) + "</title><style>" + STYLE + "</style></head><body data-mode=\"case\"><main>"
    body += '<header><p class="eyebrow">Robotweax · Lokale Fallansicht</p><h1>Flight Recorder</h1><p class="case-id mono">' + _escape(manifest.get("caseId")) + "</p>"
    body += _badge("Fallformat " + _text(manifest.get("formatVersion"))) + " " + _badge(manifest.get("evidenceClass"))
    body += '<div class="facts"><div><span class="muted">Sitzungen</span><strong>' + str(len(manifest.get("sessions", []))) + '</strong></div><div><span class="muted">Endpunkte / Epochen</span><strong>' + str(endpoint_count) + '</strong></div><div><span class="muted">Samples</span><strong>' + str(len(case.get("timeline", []))) + '</strong></div><div><span class="muted">Trigger</span><strong>' + str(len(manifest.get("triggers", []))) + "</strong></div></div></header>"
    body += '<div id="view-modes" class="toolbar" role="group" aria-label="Ansichtsmodus"><button type="button" data-mode="case" aria-pressed="true">Fallansicht</button><button type="button" data-mode="raw" aria-pressed="false">Reine Statistik</button></div><p id="mode-status" class="sr-only" aria-live="polite"></p>'
    body += '<section class="panel raw-only"><h2>Reine Statistik</h2><p>Dieselben Endpunkte, Trigger, Qualitätsangaben und Messwerte; Befunde und Hypothesen sind in diesem Modus ausgeblendet.</p><p class="muted">Dieser umschaltbare Modus ist keine Verblindung. Für den Nutzenvergleich sind die separat erzeugten, verblindeten Evaluationsartefakte zu verwenden.</p></section>'
    time_text = (
        "Koordinierte Erfassungsfenster: Der Koordinator begrenzt die gemeinsame Erfassung mit Start und Ende in seiner Uhrdomäne. Die angegebene Unsicherheit beschreibt die Fensterbreite, keine exakte Gleichzeitigkeit. Lokale monotone Zeitwerte der Endpunkte bleiben getrennt; innerhalb eines Fensters wird keine Reihenfolge behauptet."
        if coordinated else
        "Qualifizierte logische Samplefenster: Sender und Empfänger werden durch den dokumentierten Testablauf zugeordnet. Das ist keine Synchronisation unabhängiger monotoner Uhren. Innerhalb eines Fensters wird keine Reihenfolge behauptet; die reale Zeitunsicherheit ist nicht bestimmt."
        if qualified else
        "Getrennte Zeitdomänen: Samples werden je Endpunkt und Verbindungsepoche in Aufzeichnungsreihenfolge dargestellt. Unabhängige monotone Uhren sind nicht direkt vergleichbar. Ohne belastbare Uhrzuordnung und Unsicherheit ist keine gemeinsame Ereignisreihenfolge belegt."
    )
    body += '<section class="panel"><h2>Zeit &amp; Aussagegrenzen</h2><p class="notice">' + time_text + '</p><p class="muted">Transportmetriken belegen weder sichtbaren Bildstillstand noch erfolgreiche Anwendungszustellung. Diese Ansicht wertet keine Mediennutzdaten aus. Paket- und Medienbeobachtungen sind optionale Erweiterungen.</p></section>'
    body += _findings(case, supported)
    body += _endpoints(manifest, case.get("recordings", {}))
    body += '<div class="two-col">' + _events(manifest.get("triggers", []), "Vorfallmarkierungen", "Keine Trigger erfasst.") + _events(manifest.get("gaps", []), "Erfassungslücken", "Keine Erfassungslücken deklariert; dies belegt keine lückenlose Beobachtung.") + "</div>"
    body += _timeline(case, qualified)
    body += _events(manifest.get("events", []), "Konfiguration & Ereignisse", "Keine zusätzlichen Konfigurations- oder Anwendungsereignisse erfasst.")
    body += '<section class="panel"><h2>Herkunft &amp; Integrität</h2><p class="muted">Die Fallansicht wird nach lokaler Paketprüfung erzeugt. Herkunftsangaben und Prüfsummen dokumentieren die enthaltenen Quellen; sie sind keine unabhängige Authentisierung des Erzeugers.</p>'
    body += _json_details(manifest.get("provenance", {}), "Herkunftsnachweis") + _json_details(manifest, "Vollständiges Fallmanifest")
    for path, recording in case.get("recordings", {}).items():
        body += _json_details(recording.get("header", {}), "Aufzeichnung: " + _text(path))
    body += '</section><footer>Robotweax Flight Recorder · Erster lokaler Entwicklungsstand · Offline nutzbar, keine externen Ressourcen.</footer></main><script>' + SCRIPT + "</script></body></html>"
    return body
