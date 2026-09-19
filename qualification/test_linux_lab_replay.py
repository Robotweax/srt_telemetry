#!/usr/bin/env python3
"""Replay measured Linux incidents; distinguish capture gates from diagnosis."""

import hashlib
import importlib.util
import json
from pathlib import Path
import unittest

from run_bandwidth_reduction_capture import sender_pressure_evidence
from run_receiver_pause_capture import pressure_evidence

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "testdata/linux-lab-2026-09-19"
spec = importlib.util.spec_from_file_location("analyzer", ROOT / "tools/analyze_incident.py")
analyzer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyzer)
report_spec = importlib.util.spec_from_file_location(
    "operator_evaluation", ROOT / "tools/operator_evaluation.py")
operator_evaluation = importlib.util.module_from_spec(report_spec)
report_spec.loader.exec_module(operator_evaluation)


class LinuxLabReplay(unittest.TestCase):
    def recordings(self, run):
        source = analyzer.load_jsonl([
            FIXTURES / run / "sender.jsonl", FIXTURES / run / "receiver.jsonl"
        ])
        samples = [dict(sample, role=role)
                   for role, stream in source["streams"].items()
                   for sample in stream["samples"]]
        return samples, analyzer.build_analysis(source)

    def test_recording_integrity(self):
        hashes = json.loads((FIXTURES / "SHA256.json").read_text())
        self.assertEqual(len(hashes), 12)
        self.assertEqual(set(hashes), {
            str(path.relative_to(FIXTURES)) for path in FIXTURES.glob("*/*.jsonl")
        })
        for name, digest in hashes.items():
            with self.subTest(recording=name):
                self.assertEqual(hashlib.sha256((FIXTURES / name).read_bytes()).hexdigest(), digest)

    def test_receiver_growth_without_upstream_confirmation(self):
        for repetition in range(1, 4):
            with self.subTest(repetition=repetition):
                samples, result = self.recordings(
                    f"receiver_pause--haivision-to-robotweax--rep{repetition}")
                with self.assertRaisesRegex(RuntimeError, "receiver pressure evidence is incomplete"):
                    pressure_evidence(samples, pause_messages=7000, steady_messages=100)
                self.assertIsNone(result["selectedHypothesis"]["id"])
                self.assertEqual(result["selectedHypothesis"]["assessment"], "insufficient-evidence")
                hypothesis = next(h for h in result["hypotheses"] if h["id"] == "receiver-side-pressure")
                evidence = {e["id"]: e["samples"] for e in hypothesis["supportingEvidence"]}
                self.assertEqual(evidence["receiver-buffer-growth"], [3, 4, 5, 6])
                self.assertIn("receiver-buffer-recovery", evidence)
                self.assertNotIn("upstream-backpressure", evidence)
                self.assertFalse(any("combined sender-drop counter" in text
                                     for text in result["nonClaims"]))

    def test_sender_drops_reject_pure_capture_but_support_pressure(self):
        for repetition, drops in enumerate([415, 501, 398], 1):
            with self.subTest(repetition=repetition):
                samples, result = self.recordings(
                    f"bandwidth_reduction--robotweax-to-haivision--rep{repetition}")
                with self.assertRaisesRegex(RuntimeError,
                        f"sender/7: sender_drop_packet_delta={drops}"):
                    sender_pressure_evidence(samples, active_messages=2000, steady_messages=100)
                self.assertEqual(result["selectedHypothesis"]["id"], "sender-side-pressure")
                self.assertEqual(result["selectedHypothesis"]["assessment"], "supported")
                hypothesis = next(h for h in result["hypotheses"] if h["id"] == "sender-side-pressure")
                evidence = {e["id"]: e["samples"] for e in hypothesis["supportingEvidence"]}
                self.assertEqual(evidence["sender-drop"], [7])
                self.assertEqual(evidence["sender-buffer-growth"], [3, 4, 5, 6])
                observations = result["timeline"]["observations"]
                self.assertEqual(observations[7]["signals"]["senderDropPackets"], drops)
                self.assertEqual(result["timeline"]["recoverySample"], 8)
                # Drops alone must not be interpreted as missing application delivery.
                self.assertEqual(result["totals"]["uniqueSentPackets"], 2400)
                self.assertEqual(result["totals"]["uniqueReceivedPackets"], 2400)
                drop_evidence = next(e for e in hypothesis["supportingEvidence"]
                                     if e["id"] == "sender-drop")
                self.assertIn("local send/retransmission-buffer removals", drop_evidence["summary"])
                self.assertIn("message TTL or TLPKTDROP", drop_evidence["summary"])
                self.assertIn("may still arrive", drop_evidence["summary"])
                self.assertTrue(any("do not by themselves prove network loss or application payload loss"
                                    in text for text in result["nonClaims"]))
                self.assertTrue(any("does not distinguish message TTL from TLPKTDROP"
                                    in text for text in result["nonClaims"]))
                # The report must not infer a per-run deadline or verified payload
                # integrity from public counters alone.
                self.assertNotIn("payloadIntegrityVerified", result)
                report = operator_evaluation.build_report_view("lab-replay", {
                    "manifest": {"incidentId": "sender-drop-replay"}, "analysis": result,
                })
                rendered = operator_evaluation.render_report_view(report)
                self.assertIn("may still arrive at the receiver", rendered)
                self.assertIn("do not by themselves prove network loss or application payload loss", rendered)


if __name__ == "__main__":
    unittest.main()
