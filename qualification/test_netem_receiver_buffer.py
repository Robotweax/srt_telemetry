#!/usr/bin/env python3
"""Measured netem replays plus explicitly synthetic missing-data controls."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "testdata/netem-receiver-buffer-2026-09-19"


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


analyzer = load("analyze_incident")
operator = load("operator_evaluation")


class ReceiverBufferReplay(unittest.TestCase):
    def source(self, name):
        return analyzer.load_jsonl([DATA / name / "sender.jsonl", DATA / name / "receiver.jsonl"])

    def test_measured_repetitions_and_baselines(self):
        provenance = json.loads((DATA / "provenance.json").read_text())
        self.assertEqual(len(provenance["outcomes"]), 9)
        self.assertEqual(set(provenance["sha256"]), {
            str(path.relative_to(DATA)) for path in DATA.glob("*/*.jsonl")})
        for name, digest in provenance["sha256"].items():
            self.assertEqual(hashlib.sha256((DATA / name).read_bytes()).hexdigest(), digest)
        for name, expected in provenance["outcomes"].items():
            with self.subTest(run=name):
                report = analyzer.build_analysis(self.source(name))
                self.assertEqual(report["selectedHypothesis"]["id"], expected["selectedHypothesis"])
                self.assertEqual(report["selectedHypothesis"]["assessment"], expected["assessment"])
                view = operator.build_report_view("netem-replay", {
                    "manifest": {"incidentId": name}, "analysis": report})
                rendered = operator.render_report_view(view)
                if name.startswith("baseline"):
                    self.assertNotIn("Observed receiver buffer growth:", rendered)
                    continue
                self.assertIn("Observed receiver buffer growth:", rendered)
                self.assertIn("9212000", rendered)
                self.assertIn("1048576", rendered)
                self.assertIn("Buffer returned below the threshold", rendered)
                self.assertIn("does not establish slow application reads", rendered)
                self.assertLess(rendered.index("Observed receiver buffer growth:"), rendered.index("Hypotheses:"))
                if expected["assessment"] == "supported":
                    self.assertIn("Overlapping sender buffer or flight growth: samples", rendered)
                else:
                    self.assertIn("No overlapping sender buffer or flight growth crossed", rendered)
                self.assertEqual(report["totals"]["uniqueReceivedPackets"], 7400)

    def test_synthetic_missing_receiver_gauges(self):
        source = self.source("receiver-pause--haivision-to-robotweax--rep1")
        for sample in source["streams"]["receiver"]["samples"]:
            sample["raw"]["receive_buffer_bytes"] = None
        self.assertEqual(operator.render_receiver_buffer_observation(analyzer.build_analysis(source)), [])

    def test_synthetic_missing_sender_confirmation(self):
        original = self.source("receiver-pause--haivision-to-robotweax--rep1")
        for mode in ("gauges", "baseline"):
            with self.subTest(mode=mode):
                source = copy.deepcopy(original)
                for sample in source["streams"]["sender"]["samples"]:
                    if mode == "gauges" or sample["sampleIndex"] in (1, 2):
                        sample["raw"]["send_buffer_bytes"] = None
                        sample["raw"]["flight_size_packets"] = None
                lines = "\n".join(operator.render_receiver_buffer_observation(analyzer.build_analysis(source)))
                self.assertIn("Upstream confirmation is incomplete", lines)
                self.assertNotIn("No overlapping", lines)

    def test_synthetic_unobserved_recovery(self):
        source = self.source("receiver-pause--haivision-to-robotweax--rep1")
        for sample in source["streams"]["receiver"]["samples"]:
            if sample["sampleIndex"] >= 7:
                sample["raw"]["receive_buffer_bytes"] = 9212000
        lines = "\n".join(operator.render_receiver_buffer_observation(analyzer.build_analysis(source)))
        self.assertIn("recovery below the threshold was not observed", lines)


if __name__ == "__main__":
    unittest.main()
