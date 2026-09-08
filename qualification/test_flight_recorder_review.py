#!/usr/bin/env python3
"""Adversarial review regressions for case evidence, clocks and bounded import.

The positive control uses an existing qualified recording. Mutations below are
deliberate inconsistent fixtures, not new captures or diagnostic benefit proof.
"""

import copy
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import flight_recorder as recorder


class FlightRecorderReviewTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = ROOT / "testdata/golden-incidents/incident-006-jsonl-constant-loss-robotweax-haivision"
        cls.paths = [cls.source / "sender.jsonl", cls.source / "receiver.jsonl"]
        cls.base = recorder.make_case(
            cls.paths, "retained-capture", aligned=True, trigger_index=3,
            source_manifest=cls.source / "manifest.json", private=True,
        )

    def case(self):
        return copy.deepcopy(self.base)

    def roundtrip(self, case):
        with tempfile.TemporaryDirectory(prefix="flight-review-") as directory:
            path = Path(directory) / "case.frcase"
            recorder.write_case(case, path)
            return recorder.read_case(path)

    def assert_insufficient(self, case):
        result = recorder.assess_case(self.roundtrip(case))
        self.assertEqual(result["assessment"]["status"], "insufficient-evidence")
        self.assertIsNone(result["analysis"]["selectedHypothesis"]["id"])
        self.assertTrue(result["assessment"]["reasons"])
        return result

    def test_qualified_positive_control_survives_archive_roundtrip(self):
        result = recorder.assess_case(self.roundtrip(self.case()))
        self.assertEqual(result["assessment"]["status"], "supported")
        self.assertEqual(result["analysis"]["selectedHypothesis"]["id"], "transmission-path-loss-with-recovery")

    def test_arbitrary_metadata_cannot_authorize_logical_pairing(self):
        with tempfile.TemporaryDirectory(prefix="flight-review-") as directory:
            path = Path(directory) / "unrelated.json"
            path.write_text("{}\n", encoding="utf-8")
            with self.assertRaises(recorder.CaseError):
                recorder.make_case(self.paths, "retained-capture", aligned=True, capture_metadata=path)

    def test_logical_pairing_without_source_manifest_is_refused(self):
        case = self.case()
        case["manifest"]["provenance"]["sources"] = [
            source for source in case["manifest"]["provenance"]["sources"]
            if source["kind"] != "source-manifest"
        ]
        self.assert_insufficient(case)

    def test_alignment_and_endpoint_mapping_cannot_disagree(self):
        case = self.case()
        case["manifest"]["sessions"][0]["endpoints"][0]["clock"]["mapping"] = "none"
        self.assert_insufficient(case)

    def test_logical_alignment_and_endpoint_clock_kind_cannot_disagree(self):
        case = self.case()
        case["manifest"]["sessions"][0]["endpoints"][0]["clock"]["kind"] = "endpoint-local"
        self.assert_insufficient(case)

    def test_lifecycle_events_bound_global_analysis(self):
        for kind in ("reconnect", "configuration-change", "clock-boundary", "counter-reset", "sample-failure"):
            with self.subTest(kind=kind):
                case = self.case()
                case["manifest"]["events"].append({
                    "kind": kind,
                    "endpointId": case["manifest"]["sessions"][0]["endpoints"][0]["endpointId"],
                    "sampleIndex": 4,
                })
                self.assert_insufficient(case)

    def test_different_connection_epochs_are_not_combined(self):
        case = self.case()
        case["manifest"]["sessions"][0]["endpoints"][1]["connectionEpoch"] = 2
        self.assert_insufficient(case)

    def test_unflagged_raw_counter_reset_is_not_accepted(self):
        case = self.case()
        sample = case["recordings"]["recordings/endpoint-1.jsonl"]["samples"][4]
        self.assertEqual(sample["delta"]["quality_flags"], 0)
        sample["raw"]["sender_loss_packet_total"] = 0
        result = self.assert_insufficient(case)
        self.assertTrue(any("counter-reset-observed" in row["quality"] for row in result["timeline"]))

    def test_delta_conflicting_with_raw_counters_is_not_accepted(self):
        case = self.case()
        sample = case["recordings"]["recordings/endpoint-1.jsonl"]["samples"][4]
        sample["delta"]["sender_loss_packet_delta"] += 1
        result = self.assert_insufficient(case)
        self.assertTrue(any("counter-delta-mismatch" in row["quality"] for row in result["timeline"]))

    def test_counter_reset_bitmap_without_quality_flag_still_bounds_analysis(self):
        case = self.case()
        sample = case["recordings"]["recordings/endpoint-1.jsonl"]["samples"][4]
        sample["delta"]["counter_reset_fields"] = 1 << 4
        self.assertEqual(sample["delta"]["quality_flags"], 0)
        self.assert_insufficient(case)

    def test_large_json_integer_fails_as_controlled_case_error(self):
        with self.assertRaises(recorder.CaseError):
            recorder.validate_contract(10**400, {"type": "integer", "maximum": 2**64 - 1})
        recording = copy.deepcopy(self.base["recordings"]["recordings/endpoint-1.jsonl"])
        recording["samples"][0]["raw"]["observed_at_unix_nano"] = 10**400
        with self.assertRaises(recorder.CaseError):
            recorder.parse_recording(recorder.recording_bytes(recording))


if __name__ == "__main__":
    unittest.main()
