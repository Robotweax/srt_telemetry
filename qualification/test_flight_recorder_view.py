#!/usr/bin/env python3
"""Offline case view qualification: conservative evidence and safe rendering."""

import base64
import copy
import hashlib
import importlib.util
import json
import unittest
from html.parser import HTMLParser
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("flight_recorder_view", ROOT / "tools/flight_recorder_view.py")
VIEW = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VIEW)


class Page(HTMLParser):
    def __init__(self, source):
        super().__init__(convert_charrefs=True)
        self.tags = []
        self.text = []
        self.scripts = []
        self.in_script = False
        self.feed(source)

    def handle_starttag(self, tag, attrs):
        self.tags.append((tag, dict(attrs)))
        if tag == "script":
            self.in_script = True
            self.scripts.append("")

    def handle_endtag(self, tag):
        if tag == "script":
            self.in_script = False

    def handle_data(self, data):
        self.text.append(data)
        if self.in_script:
            self.scripts[-1] += data


def make_case():
    """Use actual qualified recording/analysis values, not invented signal data."""
    source = ROOT / "testdata/golden-incidents/incident-006-jsonl-constant-loss-robotweax-haivision"
    case = {
        "manifest": {
            "formatVersion": "1.0", "caseId": "renderer-qualification",
            "evidenceClass": "existing-qualified-recording",
            "sessions": [{"sessionId": "session-1", "endpoints": []}],
            "triggers": [{"kind": "manual", "sampleIndex": 3}],
            "events": [], "gaps": [], "provenance": {"source": str(source.relative_to(ROOT))},
        },
        "recordings": {}, "timeline": [],
        "analysis": json.loads((source / "analysis.json").read_text()),
        "assessment": {"status": "supported", "reasons": [], "timePolicy": "qualified-logical-sample-windows"},
    }
    for role in ("sender", "receiver"):
        path = role + ".jsonl"
        records = [json.loads(line) for line in (source / path).read_text().splitlines()]
        case["recordings"][path] = {"header": records[0], "samples": records[1:]}
        case["manifest"]["sessions"][0]["endpoints"].append({
            "endpointId": role + "-1", "role": role, "connectionEpoch": 1, "recording": path,
            "clock": {"kind": "monotonic", "domainId": role + "-clock", "uncertaintyNano": None},
        })
        for sample in records[1:]:
            case["timeline"].append({
                "sessionId": "session-1", "endpointId": role + "-1", "role": role,
                "connectionEpoch": 1, "sampleIndex": sample["sampleIndex"], "windowIndex": sample["sampleIndex"],
                "domainId": role + "-clock", "timeNano": sample["raw"]["sample_monotonic_nano"],
                "uncertaintyNano": None, "quality": [], "raw": sample["raw"], "delta": sample["delta"],
            })
    return case


class FlightRecorderViewTests(unittest.TestCase):
    def test_existing_qualified_case_shows_evidence_and_boundaries(self):
        case = make_case()
        source = VIEW.render_case(case)
        page = Page(source)
        text = " ".join(page.text)
        self.assertIn("Paketverlust auf dem Übertragungsweg mit Wiederherstellung", text)
        self.assertIn("Stützende Beobachtungen", text)
        self.assertIn("Widersprechende Beobachtungen", text)
        self.assertIn("Fehlende Nachweise", text)
        self.assertIn("Nächste Messung", text)
        self.assertIn("first-sample", text)
        self.assertIn("keine Reihenfolge", text)
        self.assertEqual(len([attrs for tag, attrs in page.tags if tag == "tr" and attrs.get("id", "").startswith("sample-")]), len(case["timeline"]))
        self.assertEqual(len([attrs for tag, attrs in page.tags if tag == "a" and "trigger" in attrs.get("class", "")]), 2)

    def test_independent_clocks_never_support_global_diagnosis_or_reorder_samples(self):
        case = make_case()
        case["assessment"]["timePolicy"] = "independent-clock-domains"
        case["timeline"][0]["timeNano"] = 991337123
        case["timeline"][1]["timeNano"] = 991337122
        source = VIEW.render_case(case)
        self.assertIn("<h2>Unzureichende Daten</h2>", source)
        self.assertIn("Getrennte Zeitdomänen", source)
        self.assertNotIn("<th>Logisches Fenster</th>", source)
        self.assertNotIn("Erstes abweichendes Fenster", source)
        self.assertLess(source.index("991337123"), source.index("991337122"))

    def test_insufficient_assessment_overrides_existing_supported_hypothesis(self):
        case = make_case()
        case["assessment"].update(status="insufficient-evidence", reasons=["connection-epoch-boundary"])
        source = VIEW.render_case(case)
        self.assertIn("<h2>Unzureichende Daten</h2>", source)
        self.assertIn("connection-epoch-boundary", source)
        self.assertNotIn('class="badge good"', source)
        self.assertNotIn("Erstes abweichendes Fenster", source)

    def test_coordinator_windows_show_brackets_without_claiming_exact_order(self):
        case = make_case()
        case["assessment"]["timePolicy"] = "coordinator-sample-windows"
        case["timeline"][0].update(windowStartNano=123456, windowEndNano=123999, uncertaintyNano=543)
        source = VIEW.render_case(case)
        self.assertIn("Koordinierte Erfassungsfenster", source)
        self.assertIn("Erfassungsfenster Start · ns", source)
        self.assertIn("Erfassungsfenster Ende · ns", source)
        self.assertIn("<td>123456</td>", source)
        self.assertIn("<td>123999</td>", source)
        self.assertIn("<td>543</td>", source)
        self.assertIn("keine exakte Gleichzeitigkeit", source)
        self.assertIn("keine Reihenfolge", source)
        self.assertNotIn("<h2>Unzureichende Daten</h2>", source)
        self.assertIn("Konfigurierte TSBPD-Latenz · ms", source)
        self.assertNotIn("Playoutreserve · ms", source)

    def test_null_unavailable_and_nonfinite_metrics_are_not_zero(self):
        sample = {"raw": {"rtt_seconds": 0, "available_fields": 0}, "delta": {"sender_loss_packet_delta": 1, "available_delta_fields": 0}}
        self.assertIsNone(VIEW._metric_value(sample, VIEW.METRICS[0]))
        self.assertIsNone(VIEW._metric_value(sample, VIEW.METRICS[1]))
        sample["raw"]["available_fields"] = 1 << 15
        self.assertEqual(VIEW._metric_value(sample, VIEW.METRICS[0]), 0)
        sample["raw"]["rtt_seconds"] = None
        self.assertIsNone(VIEW._metric_value(sample, VIEW.METRICS[0]))
        for value in (None, float("nan"), float("inf"), True):
            self.assertIn("—", VIEW._number(value))
        self.assertEqual(VIEW._number(0), "0")

    def test_all_untrusted_case_strings_are_text_and_csp_allows_only_fixed_script(self):
        case = make_case()
        attack = '</script><img src="https://example.invalid/leak" onerror="alert(1)"><script>alert(2)</script>&'
        case["manifest"]["caseId"] = attack
        case["manifest"]["evidenceClass"] = attack
        case["manifest"]["sessions"][0]["sessionId"] = attack
        case["manifest"]["sessions"][0]["endpoints"][0]["clock"]["domainId"] = attack
        case["manifest"]["events"] = [{"kind": attack, "label": attack, "endpointId": attack}]
        case["manifest"]["triggers"][0]["label"] = attack
        case["manifest"]["gaps"] = [{"reason": attack}]
        case["manifest"]["provenance"] = {"source": attack}
        case["recordings"]["sender.jsonl"]["header"]["providerVersion"] = attack
        case["timeline"][0]["quality"] = [attack]
        case["timeline"][0]["raw"]["detail"] = attack
        case["analysis"]["selectedHypothesis"]["rationale"] = attack
        case["analysis"]["hypotheses"][0]["supportingEvidence"] = [{"id": attack, "summary": attack, "samples": [attack]}]
        case["analysis"]["hypotheses"][0]["missingEvidence"] = [attack]
        case["analysis"]["hypotheses"][0]["contradictoryEvidence"] = [attack]
        case["assessment"]["reasons"] = [attack]
        source = VIEW.render_case(case)
        page = Page(source)
        self.assertNotIn(attack, source)
        self.assertIn("&lt;img", source)
        self.assertEqual(len(page.scripts), 1)
        self.assertEqual(page.scripts[0], VIEW.SCRIPT)
        self.assertFalse([tag for tag, attrs in page.tags if tag in ("img", "iframe", "link", "form", "object")])
        for tag, attrs in page.tags:
            self.assertFalse([name for name in attrs if name.startswith("on")])
            self.assertNotIn("src", attrs)
        policy = next(attrs["content"] for tag, attrs in page.tags if tag == "meta" and attrs.get("http-equiv") == "Content-Security-Policy")
        digest = base64.b64encode(hashlib.sha256(VIEW.SCRIPT.encode()).digest()).decode()
        self.assertIn("script-src 'sha256-" + digest + "'", policy)
        self.assertIn("connect-src 'none'", policy)

    def test_raw_mode_reuses_one_measurement_table_per_lane_and_explains_blinding(self):
        case = make_case()
        page = Page(VIEW.render_case(case))
        self.assertEqual(len([attrs for tag, attrs in page.tags if tag == "table" and attrs.get("class") == "metrics"]), 2)
        buttons = [attrs for tag, attrs in page.tags if tag == "button"]
        self.assertEqual([attrs["data-mode"] for attrs in buttons], ["case", "raw"])
        self.assertIn("keine Verblindung", " ".join(page.text))

    def test_empty_case_is_valid_insufficient_view(self):
        case = {"manifest": {"caseId": "empty", "formatVersion": "1.0"},
                "analysis": {"diagnosis": {"status": "insufficient-evidence", "selectedHypothesis": None}},
                "assessment": {"status": "insufficient-evidence", "timePolicy": "independent-clock-domains", "reasons": ["sender missing"]}}
        source = VIEW.render_case(case)
        self.assertTrue(source.startswith("<!doctype html>"))
        self.assertTrue(source.endswith("</html>"))
        self.assertIn("Unzureichende Daten", source)
        self.assertIn("Keine Messwerte vorhanden.", source)
        self.assertIn("sender missing", source)

    def test_reconnects_and_clock_domains_keep_separate_lanes(self):
        case = make_case()
        sample = copy.deepcopy(case["timeline"][0])
        sample["connectionEpoch"] = 2
        sample["domainId"] = "new-clock"
        sample["quality"] = ["reconnect", "wall-clock-jump"]
        sample["delta"]["quality_flags"] = 8
        case["timeline"].append(sample)
        case["assessment"].update(status="insufficient-evidence", timePolicy="independent-clock-domains")
        source = VIEW.render_case(case)
        page = Page(source)
        self.assertEqual(len([attrs for tag, attrs in page.tags if tag == "article" and attrs.get("class") == "lane"]), 3)
        self.assertIn("counter-reset", source)
        self.assertIn("wall-clock-jump", source)
        self.assertIn("Epoche 2", source)
        self.assertIn("new-clock", source)


if __name__ == "__main__":
    unittest.main()
