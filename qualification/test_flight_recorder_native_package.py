#!/usr/bin/env python3
"""Exercise captured native telemetry through case packaging and local import.

These are static replays of the retained 2026-09-08 run 007, not fresh runtime
measurements. Deliberately corrupted metadata is kept in temporary directories.
"""

import copy
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "qualification/flight-recorder-2026-09-08/live-capture-007"
sys.path.insert(0, str(ROOT / "tools"))
import flight_recorder as recorder


class NativeCasePackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.metadata = {}
        cls.originals = {}
        cls.cases = {}
        for scenario in ("loss-recovery", "receiver-pause"):
            directory = FIXTURES / scenario
            metadata_path = directory / "capture-manifest.json"
            cls.metadata[scenario] = json.loads(metadata_path.read_text(encoding="utf-8"))
            cls.originals[scenario] = {}
            for role in ("sender", "receiver"):
                cls.originals[scenario][role] = recorder.parse_recording((directory / (role + ".jsonl")).read_bytes())
            cls.cases[scenario] = recorder.make_case(
                [directory / "sender.jsonl", directory / "receiver.jsonl"],
                "new-experiment", capture_metadata=metadata_path,
            )

    def case(self, scenario="loss-recovery"):
        return copy.deepcopy(self.cases[scenario])

    def roundtrip(self, case):
        with tempfile.TemporaryDirectory(prefix="flight-native-package-") as directory:
            path = Path(directory) / "case.frcase"
            recorder.write_case(case, path)
            return recorder.read_case(path)

    def assert_insufficient(self, case):
        result = recorder.assess_case(self.roundtrip(case))
        self.assertEqual(result["assessment"]["status"], "insufficient-evidence")
        self.assertIsNone(result["analysis"]["selectedHypothesis"]["id"])
        self.assertTrue(result["assessment"]["reasons"])
        return result

    def assert_corrupt_metadata_rejected(self, metadata, expected_message):
        directory = FIXTURES / "loss-recovery"
        with tempfile.TemporaryDirectory(prefix="flight-corrupt-metadata-") as temporary:
            path = Path(temporary) / "capture-manifest.json"
            path.write_text(json.dumps(metadata), encoding="utf-8")
            with self.assertRaisesRegex(recorder.CaseError, expected_message):
                recorder.make_case(
                    [directory / "sender.jsonl", directory / "receiver.jsonl"],
                    "new-experiment", capture_metadata=path,
                )

    def retained_bracket(self, metadata, role="sender"):
        stamp = self.originals["loss-recovery"][role]["samples"][0]["raw"]["sample_monotonic_nano"]
        return next(bracket for bracket in metadata["clock"]["brackets"]
                    if bracket["endpointMonotonicNano"][role] == stamp)

    def test_real_native_loss_package_retains_supported_recovery_diagnosis(self):
        result = recorder.assess_case(self.roundtrip(self.case("loss-recovery")))
        self.assertEqual(result["assessment"]["status"], "supported")
        self.assertEqual(result["assessment"]["timePolicy"], "coordinator-sample-windows")
        self.assertEqual(result["analysis"]["selectedHypothesis"]["id"], "transmission-path-loss-with-recovery")
        self.assertEqual(len(result["timeline"]), 48)

    def test_real_native_receiver_pause_keeps_observations_without_forcing_diagnosis(self):
        result = recorder.assess_case(self.roundtrip(self.case("receiver-pause")))
        self.assertEqual(result["assessment"]["status"], "insufficient-evidence")
        self.assertEqual(result["assessment"]["reasons"], [])
        self.assertIsNone(result["analysis"]["selectedHypothesis"]["id"])
        hypothesis = next(item for item in result["analysis"]["hypotheses"] if item["id"] == "receiver-side-pressure")
        self.assertEqual(hypothesis["assessment"], "insufficient-evidence")
        self.assertEqual({item["id"] for item in hypothesis["supportingEvidence"]}, {"receiver-buffer-growth", "receiver-buffer-recovery"})
        self.assertTrue(hypothesis["missingEvidence"])
        self.assertEqual(len(result["timeline"]), 52)

    def test_effective_configuration_and_exact_build_provenance_survive(self):
        for scenario in self.cases:
            with self.subTest(scenario=scenario):
                result = self.roundtrip(self.case(scenario))
                capture = self.metadata[scenario]
                for endpoint in result["manifest"]["sessions"][0]["endpoints"]:
                    configuration = endpoint["configuration"]
                    self.assertEqual(configuration, {
                        "profile": "live-message-ipv4-loopback-clear",
                        "payloadBytes": 1316, "samplingNominalHz": 20,
                        "preRecords": 8, "postMilliseconds": 1000,
                        "capacityRecordsPerEndpoint": 64, "messageApi": True,
                        "senderTsbpdDelaySeconds": 0.12, "receiverTsbpdDelaySeconds": 0.12,
                    })
                    self.assertEqual(endpoint["build"], {
                        "librarySha256": "56fadae15ecad9570c12b4c89d5b5c346c41e0ada548844cc8361dd34c20e8c1",
                        "peerSha256": "2a85826c241aeb8e4e9530c49a592bd8ec90f304331e4bd84644ffccd9cfdddb",
                        "publicApiVersionHex": 66823,
                    })
                    original = self.originals[scenario][endpoint["role"]]
                    self.assertEqual(endpoint["buildProfileSha256"], hashlib.sha256(original["header"]["buildProfile"].encode()).hexdigest())
                    header = result["recordings"][endpoint["recording"]]["header"]
                    self.assertEqual(header["configuredProvider"], "robotweax-srt")
                    self.assertEqual(header["providerRevision"], "ac9c14d7815bfd6f76f8b60308c5465095625eb7")
                    self.assertEqual(header["providerVersion"], capture["provider"]["version"])

    def test_overwritten_prehistory_and_capture_window_survive_zero_export_counter(self):
        for scenario in self.cases:
            with self.subTest(scenario=scenario):
                result = self.roundtrip(self.case(scenario))
                for endpoint in result["manifest"]["sessions"][0]["endpoints"]:
                    # The frozen export has no overwrites itself; ring history
                    # must come from the native trigger-window state.
                    recording = result["recordings"][endpoint["recording"]]
                    self.assertEqual(recording["header"]["overwrittenCount"], 0)
                    window = endpoint["captureWindow"]
                    self.assertEqual(window["preOverwrittenCount"], 4)
                    self.assertEqual(window["preRecordCount"], 8)
                    self.assertEqual(window["allocatedRecords"], 72)
                    self.assertEqual(window["allocatedRecordBytes"], 33984)
                    self.assertEqual(window["recordCount"], len(recording["samples"]))
                    self.assertEqual(window["finishReason"], "deadline")
                    self.assertGreater(window["retainedPreNano"], 0)
                    self.assertGreater(window["retainedPostNano"], 0)
                    self.assertLessEqual(window["retainedPostNano"], 1_000_000_000)
                    gaps = [gap for gap in result["manifest"]["gaps"]
                            if gap.get("endpointId") == endpoint["endpointId"] and gap["kind"] == "prehistory-overwritten"]
                    self.assertEqual([gap["count"] for gap in gaps], [4])

    def test_support_export_removes_original_identities_but_preserves_measurements(self):
        for scenario in self.cases:
            with self.subTest(scenario=scenario):
                result = self.roundtrip(self.case(scenario))
                rendered_data = json.dumps(result, sort_keys=True)
                original_ids = [self.metadata[scenario]["flowId"]]
                original_ids.extend(recording["header"]["streamId"] for recording in self.originals[scenario].values())
                original_ids.extend(self.metadata[scenario]["clock"]["endpointDomains"].values())
                for identity in original_ids:
                    self.assertNotIn(identity, rendered_data)
                self.assertEqual(result["manifest"]["privacy"], {"mode": "support", "mediaPayloads": False, "credentials": False})
                for endpoint in result["manifest"]["sessions"][0]["endpoints"]:
                    recording = result["recordings"][endpoint["recording"]]
                    original = self.originals[scenario][endpoint["role"]]
                    self.assertEqual([sample["raw"] for sample in recording["samples"]], [sample["raw"] for sample in original["samples"]])
                    self.assertEqual([sample["delta"] for sample in recording["samples"]], [sample["delta"] for sample in original["samples"]])

    def test_native_clocks_and_measured_offset_intervals_are_not_rewritten(self):
        for scenario in self.cases:
            with self.subTest(scenario=scenario):
                result = recorder.assess_case(self.roundtrip(self.case(scenario)))
                original = self.originals[scenario]
                self.assertTrue(any(sender["raw"]["sample_monotonic_nano"] != receiver["raw"]["sample_monotonic_nano"]
                                    for sender, receiver in zip(original["sender"]["samples"], original["receiver"]["samples"])))
                for row in result["timeline"]:
                    stamp = original[row["role"]]["samples"][row["sampleIndex"]]["raw"]["sample_monotonic_nano"]
                    bracket = next(item for item in self.metadata[scenario]["clock"]["brackets"]
                                   if item["endpointMonotonicNano"][row["role"]] == stamp)
                    self.assertEqual(row["timeNano"], stamp)
                    self.assertEqual(row["windowStartNano"], bracket["beginMonotonicNano"])
                    self.assertEqual(row["windowEndNano"], bracket["endMonotonicNano"])
                    self.assertEqual([row["windowStartNano"] - stamp, row["windowEndNano"] - stamp], bracket["endpointToCoordinatorOffsetNano"][row["role"]])
                    self.assertEqual(row["uncertaintyNano"], row["windowEndNano"] - row["windowStartNano"])
                    self.assertGreater(row["uncertaintyNano"], 0)
                    # This run has clearly different numerical clock origins.
                    self.assertGreater(abs(row["windowStartNano"] - stamp), 1_000_000_000)

    def test_corrupted_source_recording_digest_is_rejected(self):
        metadata = copy.deepcopy(self.metadata["loss-recovery"])
        metadata["files"]["sender.jsonl"]["sha256"] = "0" * 64
        self.assert_corrupt_metadata_rejected(metadata, "recording digest mismatch")

    def test_corrupted_retained_sample_clock_binding_is_rejected(self):
        metadata = copy.deepcopy(self.metadata["loss-recovery"])
        self.retained_bracket(metadata)["endpointMonotonicNano"]["sender"] += 1
        self.assert_corrupt_metadata_rejected(metadata, "no capture bracket")

    def test_corrupted_measured_offset_interval_is_rejected(self):
        metadata = copy.deepcopy(self.metadata["loss-recovery"])
        self.retained_bracket(metadata)["endpointToCoordinatorOffsetNano"]["sender"][0] += 1
        self.assert_corrupt_metadata_rejected(metadata, "invalid clock offset interval")

    def test_imported_case_with_missing_sample_bracket_abstains(self):
        case = self.roundtrip(self.case())
        case["manifest"]["alignment"]["windows"].pop(0)
        self.assert_insufficient(case)

    def test_imported_case_with_overlapping_sample_brackets_abstains(self):
        case = self.roundtrip(self.case())
        windows = case["manifest"]["alignment"]["windows"]
        endpoint = windows[0]["endpointId"]
        lane = sorted((window for window in windows if window["endpointId"] == endpoint), key=lambda item: item["sampleIndex"])
        lane[1]["startNano"] = lane[0]["endNano"]
        result = self.assert_insufficient(case)
        self.assertTrue(any("Überlappende" in reason for reason in result["assessment"]["reasons"]))

    def test_imported_case_with_individually_valid_but_unpaired_brackets_abstains(self):
        case = self.roundtrip(self.case())
        window = case["manifest"]["alignment"]["windows"][0]
        window["startNano"] += 1
        window["endNano"] += 1
        result = self.assert_insufficient(case)
        self.assertTrue(any("do not share a coordinator sample window" in reason for reason in result["assessment"]["reasons"]))


if __name__ == "__main__":
    unittest.main()
