#!/usr/bin/env python3

import argparse
import copy
import importlib.util
import json
import subprocess
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise RuntimeError(message)


def load_analyzer(path: Path):
    specification = importlib.util.spec_from_file_location(
        "robotweax_incident_analyzer", path
    )
    if specification is None or specification.loader is None:
        fail(f"cannot load analyzer: {path}")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


HYPOTHESIS_IDS = {
    "transmission-path-loss-with-recovery",
    "transmission-path-rtt-step-with-recovery",
    "sender-side-pressure",
    "receiver-side-pressure",
}


def validate_common_result(result: dict, expected_format: str) -> None:
    if result.get("schemaVersion") != "1alpha1" or result.get("status") != "pass":
        fail("analysis result identity is invalid")
    if result["source"]["format"] != expected_format:
        fail(f"analysis source format mismatch: {result['source']['format']}")
    hypotheses = result["hypotheses"]
    if len(hypotheses) != 4 or {item["id"] for item in hypotheses} != HYPOTHESIS_IDS:
        fail("analysis does not contain the four required hypotheses")
    if not all(item["missingEvidence"] for item in hypotheses):
        fail("every hypothesis must expose its missing evidence")
    if not result["nonClaims"]:
        fail("analysis must retain explicit non-claims")


def validate_loss_result(result: dict, expected_format: str) -> None:
    validate_common_result(result, expected_format)
    timeline = result["timeline"]
    expected_timeline = {
        "sampleCount": 9,
        "firstDeviationSample": 3,
        "lastDeviationSample": 6,
        "recoverySample": 7,
    }
    for field, expected in expected_timeline.items():
        if timeline[field] != expected:
            fail(f"timeline {field} expected {expected}, got {timeline[field]}")
    if result["selectedHypothesis"] != {
        "id": "transmission-path-loss-with-recovery",
        "assessment": "supported",
        "rationale": (
            "Aligned sender loss, receiver loss, and retransmission deltas identify "
            "recoverable path loss without sufficient endpoint-pressure evidence."
        ),
    }:
        fail(f"unexpected selected hypothesis: {result['selectedHypothesis']}")


def validate_rtt_result(result: dict, expected_format: str) -> None:
    validate_common_result(result, expected_format)
    expected_timeline = {
        "sampleCount": 13,
        "firstDeviationSample": 3,
        "lastDeviationSample": 7,
        "recoverySample": 8,
    }
    for field, expected in expected_timeline.items():
        actual = result["timeline"][field]
        if actual != expected:
            fail(f"RTT timeline {field} expected {expected}, got {actual}")
    if result["selectedHypothesis"] != {
        "id": "transmission-path-rtt-step-with-recovery",
        "assessment": "supported",
        "rationale": (
            "Paired sender and receiver RTT cross conservative warm-baseline "
            "thresholds and recover without loss or endpoint-pressure evidence."
        ),
    }:
        fail(f"unexpected RTT selected hypothesis: {result['selectedHypothesis']}")
    rtt_hypothesis = next(
        item
        for item in result["hypotheses"]
        if item["id"] == "transmission-path-rtt-step-with-recovery"
    )
    support_ids = {item["id"] for item in rtt_hypothesis["supportingEvidence"]}
    if rtt_hypothesis["assessment"] != "supported" or not {
        "paired-rtt-step",
        "rtt-recovery-boundary",
    }.issubset(support_ids):
        fail(f"retained RTT evidence is incomplete: {rtt_hypothesis}")
    totals = result["totals"]
    if totals["senderLossPackets"] != 0 or totals["receiverLossPackets"] != 0:
        fail(f"retained RTT incident contains public loss: {totals}")
    if (
        totals["uniqueSentPackets"] != 12_000
        or totals["uniqueReceivedPackets"] != 12_000
        or totals["physicalSentPackets"] < totals["uniqueSentPackets"]
    ):
        fail(f"retained RTT delivery totals are invalid: {totals}")
    baseline = result["baselines"]["rttSeconds"]
    for role in ["sender", "receiver"]:
        value = baseline[role]
        threshold = baseline[f"{role}StepThreshold"]
        if value is None or threshold is None or threshold <= value:
            fail(f"retained RTT {role} baseline is invalid: {baseline}")


def validate_receiver_pressure_result(result: dict, expected_format: str) -> None:
    validate_common_result(result, expected_format)
    timeline = result["timeline"]
    if timeline["sampleCount"] != 9 or timeline["firstDeviationSample"] != 3:
        fail(f"receiver-pressure timeline identity is invalid: {timeline}")
    last_deviation = timeline["lastDeviationSample"]
    if last_deviation not in {6, 7} or timeline["recoverySample"] != last_deviation + 1:
        fail(f"receiver-pressure recovery boundary is invalid: {timeline}")
    if result["selectedHypothesis"] != {
        "id": "receiver-side-pressure",
        "assessment": "supported",
        "rationale": (
            "Receiver buffer growth overlaps upstream sender pressure and recovers "
            "after the deviation without a supported path-loss explanation."
        ),
    }:
        fail(
            "unexpected receiver-pressure selected hypothesis: "
            f"{result['selectedHypothesis']}"
        )
    receiver_hypothesis = next(
        item for item in result["hypotheses"] if item["id"] == "receiver-side-pressure"
    )
    support_ids = {item["id"] for item in receiver_hypothesis["supportingEvidence"]}
    if receiver_hypothesis["assessment"] != "supported" or not {
        "receiver-buffer-growth",
        "upstream-backpressure",
        "receiver-buffer-recovery",
    }.issubset(support_ids):
        fail(
            f"retained receiver-pressure evidence is incomplete: {receiver_hypothesis}"
        )
    totals = result["totals"]
    if (
        totals["senderLossPackets"] != 0
        or totals["receiverLossPackets"] != 0
        or totals["uniqueSentPackets"] != 7_400
        or totals["uniqueReceivedPackets"] != 7_400
    ):
        fail(f"retained receiver-pressure delivery totals are invalid: {totals}")
    baseline = result["baselines"]["pressure"]
    value = baseline["receiverReceiveBufferBytes"]
    threshold = baseline["receiverReceiveBufferStepThreshold"]
    if value is None or threshold is None or threshold <= value:
        fail(f"retained receiver-pressure baseline is invalid: {baseline}")


def validate_sender_pressure_result(result: dict, expected_format: str) -> None:
    validate_common_result(result, expected_format)
    timeline = result["timeline"]
    expected_timeline = {
        "sampleCount": 9,
        "firstDeviationSample": 3,
        "lastDeviationSample": 6,
        "recoverySample": 7,
    }
    for field, expected in expected_timeline.items():
        if timeline[field] != expected:
            fail(
                f"sender-pressure timeline {field} expected {expected}, "
                f"got {timeline[field]}"
            )
    if result["selectedHypothesis"] != {
        "id": "sender-side-pressure",
        "assessment": "supported",
        "rationale": (
            "Sender buffer growth overlaps elevated flight size while the "
            "receiver buffer remains below its pressure threshold, then "
            "recovers without supported path-loss evidence."
        ),
    }:
        fail(
            "unexpected sender-pressure selected hypothesis: "
            f"{result['selectedHypothesis']}"
        )
    sender_hypothesis = next(
        item for item in result["hypotheses"] if item["id"] == "sender-side-pressure"
    )
    support_ids = {item["id"] for item in sender_hypothesis["supportingEvidence"]}
    if sender_hypothesis["assessment"] != "supported" or not {
        "sender-buffer-growth",
        "sender-flight-growth",
        "receiver-buffer-remains-below-pressure-threshold",
        "sender-pressure-recovery",
    }.issubset(support_ids):
        fail(f"retained sender-pressure evidence is incomplete: {sender_hypothesis}")
    totals = result["totals"]
    if (
        totals["senderLossPackets"] != 0
        or totals["receiverLossPackets"] != 0
        or totals["uniqueSentPackets"] != 2_400
        or totals["uniqueReceivedPackets"] != 2_400
    ):
        fail(f"retained sender-pressure delivery totals are invalid: {totals}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Test the local incident analyzer")
    parser.add_argument("--analyzer", type=Path, required=True)
    parser.add_argument("--jsonl", type=Path, nargs=2, required=True)
    parser.add_argument("--manifest", type=Path, action="append", required=True)
    parser.add_argument("--expected-analysis", type=Path, required=True)
    arguments = parser.parse_args()
    analyzer = load_analyzer(arguments.analyzer.resolve())

    manifest_results = []
    retained_recording_count = 0
    for manifest in arguments.manifest:
        manifest = manifest.resolve()
        manifest_document = json.loads(manifest.read_text(encoding="utf-8"))
        validators = {
            "constant-packet-loss": validate_loss_result,
            "burst-packet-loss": validate_loss_result,
            "round-trip-time-step": validate_rtt_result,
            "receiver-application-pause": validate_receiver_pressure_result,
            "bandwidth-reduction-with-queue-growth": validate_sender_pressure_result,
        }
        failure_class = manifest_document["failureClass"]
        if failure_class not in validators:
            fail(f"test has no result validator for {failure_class}: {manifest}")
        result_validator = validators[failure_class]
        result = analyzer.build_analysis(analyzer.load_manifest(manifest))
        result_validator(result, "robotweax-srt-telemetry-csv-v1")
        manifest_results.append(result)
        recordings = manifest_document.get("recordings")
        if recordings is not None:
            retained_recording_count += 1
            jsonl_result = analyzer.build_analysis(
                analyzer.load_jsonl(
                    [
                        manifest.parent / recordings["sender"]["file"],
                        manifest.parent / recordings["receiver"]["file"],
                    ]
                )
            )
            result_validator(jsonl_result, "robotweax-srt-telemetry-jsonl-v1")
            retained = json.loads(
                (manifest.parent / recordings["analysis"]["file"]).read_text(
                    encoding="utf-8"
                )
            )
            if jsonl_result != retained:
                fail(f"retained JSONL analysis is not reproducible: {manifest}")
            for field in [
                "timeline",
                "baselines",
                "totals",
                "hypotheses",
                "selectedHypothesis",
            ]:
                if jsonl_result[field] != result[field]:
                    fail(f"CSV and JSONL analysis disagree on {field}: {manifest}")
    expected = json.loads(arguments.expected_analysis.read_text(encoding="utf-8"))
    if manifest_results[0] != expected:
        fail("retained synthetic incident analysis is not reproducible")

    jsonl_source = analyzer.load_jsonl([path.resolve() for path in arguments.jsonl])
    jsonl_result = analyzer.build_analysis(jsonl_source)
    validate_loss_result(jsonl_result, "robotweax-srt-telemetry-jsonl-v1")

    command = [
        sys.executable,
        str(arguments.analyzer),
        "--jsonl",
        str(arguments.jsonl[0]),
        str(arguments.jsonl[1]),
        "--compact",
    ]
    completed = subprocess.run(command, check=True, capture_output=True, text=True)
    cli_result = json.loads(completed.stdout)
    validate_loss_result(cli_result, "robotweax-srt-telemetry-jsonl-v1")

    insufficient_source = copy.deepcopy(jsonl_source)
    for stream in insufficient_source["streams"].values():
        for sample in stream["samples"]:
            for field in [
                "sender_loss_packet_delta",
                "receiver_loss_packet_delta",
                "retransmitted_packet_delta",
            ]:
                if field in sample["delta"]:
                    sample["delta"][field] = None
    insufficient = analyzer.build_analysis(insufficient_source)
    if insufficient["selectedHypothesis"]["assessment"] != "insufficient-evidence":
        fail(
            "missing paired loss evidence must produce an insufficient-evidence result"
        )

    misaligned_source = copy.deepcopy(jsonl_source)
    receiver_samples = misaligned_source["streams"]["receiver"]["samples"]
    for sample in receiver_samples:
        sample["delta"]["receiver_loss_packet_delta"] = 0
    receiver_samples[7]["delta"]["receiver_loss_packet_delta"] = 20
    misaligned = analyzer.build_analysis(misaligned_source)
    if misaligned["selectedHypothesis"]["assessment"] != "insufficient-evidence":
        fail(
            "non-overlapping sender and receiver loss must not count as paired evidence"
        )

    rtt_source = copy.deepcopy(jsonl_source)
    for stream in rtt_source["streams"].values():
        for sample in stream["samples"]:
            index = sample["sampleIndex"]
            for field in [
                "sender_loss_packet_delta",
                "receiver_loss_packet_delta",
                "retransmitted_packet_delta",
                "sender_drop_packet_delta",
                "receiver_drop_packet_delta",
                "receiver_undecryptable_packet_delta",
            ]:
                if index > 0 and field in sample["delta"]:
                    sample["delta"][field] = 0
            sample["raw"]["rtt_seconds"] = 0.100 if 3 <= index <= 6 else 0.010
    rtt = analyzer.build_analysis(rtt_source)
    if rtt["timeline"]["firstDeviationSample"] != 3:
        fail(f"RTT step first deviation is incorrect: {rtt['timeline']}")
    if rtt["timeline"]["lastDeviationSample"] != 6:
        fail(f"RTT step last deviation is incorrect: {rtt['timeline']}")
    if rtt["timeline"]["recoverySample"] != 7:
        fail(f"RTT step recovery is incorrect: {rtt['timeline']}")
    if rtt["selectedHypothesis"] != {
        "id": "transmission-path-rtt-step-with-recovery",
        "assessment": "supported",
        "rationale": (
            "Paired sender and receiver RTT cross conservative warm-baseline "
            "thresholds and recover without loss or endpoint-pressure evidence."
        ),
    }:
        fail(f"unexpected RTT-step hypothesis: {rtt['selectedHypothesis']}")
    rtt_hypothesis = next(
        item
        for item in rtt["hypotheses"]
        if item["id"] == "transmission-path-rtt-step-with-recovery"
    )
    if (
        rtt_hypothesis["assessment"] != "supported"
        or not rtt_hypothesis["supportingEvidence"]
    ):
        fail(f"RTT-step evidence is incomplete: {rtt_hypothesis}")

    pressure_source = copy.deepcopy(jsonl_source)
    for role, stream in pressure_source["streams"].items():
        for sample in stream["samples"]:
            index = sample["sampleIndex"]
            for field in [
                "sender_loss_packet_delta",
                "receiver_loss_packet_delta",
                "retransmitted_packet_delta",
                "sender_drop_packet_delta",
                "receiver_drop_packet_delta",
                "receiver_undecryptable_packet_delta",
            ]:
                if index > 0 and field in sample["delta"]:
                    sample["delta"][field] = 0
            sample["raw"]["rtt_seconds"] = 0.010
            sample["raw"]["flight_size_packets"] = (
                100 if role == "sender" and 3 <= index <= 4 else 0
            )
            sample["raw"]["send_buffer_bytes"] = (
                1_000_000 if role == "sender" and 3 <= index <= 4 else 0
            )
            sample["raw"]["send_buffer_seconds"] = 0.0
            sample["raw"]["receive_buffer_bytes"] = (
                5_000_000 if role == "receiver" and 3 <= index <= 6 else 0
            )
            sample["raw"]["receive_buffer_seconds"] = (
                0.1 if role == "receiver" and 3 <= index <= 6 else 0.0
            )
    pressure = analyzer.build_analysis(pressure_source)
    expected_pressure_timeline = {
        "firstDeviationSample": 3,
        "lastDeviationSample": 6,
        "recoverySample": 7,
    }
    for field, expected in expected_pressure_timeline.items():
        if pressure["timeline"][field] != expected:
            fail(
                f"receiver pressure {field} expected {expected}: "
                f"{pressure['timeline']}"
            )
    if pressure["selectedHypothesis"] != {
        "id": "receiver-side-pressure",
        "assessment": "supported",
        "rationale": (
            "Receiver buffer growth overlaps upstream sender pressure and recovers "
            "after the deviation without a supported path-loss explanation."
        ),
    }:
        fail(
            f"unexpected receiver-pressure hypothesis: {pressure['selectedHypothesis']}"
        )
    receiver_hypothesis = next(
        item
        for item in pressure["hypotheses"]
        if item["id"] == "receiver-side-pressure"
    )
    if {item["id"] for item in receiver_hypothesis["supportingEvidence"]} != {
        "receiver-buffer-growth",
        "upstream-backpressure",
        "receiver-buffer-recovery",
    }:
        fail(f"receiver-pressure evidence is incomplete: {receiver_hypothesis}")

    sender_pressure_source = copy.deepcopy(jsonl_source)
    for role, stream in sender_pressure_source["streams"].items():
        for sample in stream["samples"]:
            index = sample["sampleIndex"]
            for field in [
                "sender_loss_packet_delta",
                "receiver_loss_packet_delta",
                "retransmitted_packet_delta",
                "sender_drop_packet_delta",
                "receiver_drop_packet_delta",
                "receiver_undecryptable_packet_delta",
            ]:
                if index > 0 and field in sample["delta"]:
                    sample["delta"][field] = 0
            sample["raw"]["rtt_seconds"] = 0.010
            sample["raw"]["flight_size_packets"] = (
                1_000 if role == "sender" and 3 <= index <= 6 else 0
            )
            sample["raw"]["send_buffer_bytes"] = (
                2_000_000 if role == "sender" and 3 <= index <= 6 else 0
            )
            sample["raw"]["send_buffer_seconds"] = 0.0
            sample["raw"]["receive_buffer_bytes"] = (
                10_000 if role == "receiver" and 3 <= index <= 6 else 0
            )
            sample["raw"]["receive_buffer_seconds"] = 0.0
    sender_pressure = analyzer.build_analysis(sender_pressure_source)
    expected_sender_timeline = {
        "firstDeviationSample": 3,
        "lastDeviationSample": 6,
        "recoverySample": 7,
    }
    for field, expected in expected_sender_timeline.items():
        if sender_pressure["timeline"][field] != expected:
            fail(
                f"sender pressure {field} expected {expected}: "
                f"{sender_pressure['timeline']}"
            )
    expected_sender_selection = {
        "id": "sender-side-pressure",
        "assessment": "supported",
        "rationale": (
            "Sender buffer growth overlaps elevated flight size while the "
            "receiver buffer remains below its pressure threshold, then "
            "recovers without supported path-loss evidence."
        ),
    }
    if sender_pressure["selectedHypothesis"] != expected_sender_selection:
        fail(
            f"unexpected sender-pressure hypothesis: "
            f"{sender_pressure['selectedHypothesis']}"
        )
    sender_hypothesis = next(
        item
        for item in sender_pressure["hypotheses"]
        if item["id"] == "sender-side-pressure"
    )
    if {item["id"] for item in sender_hypothesis["supportingEvidence"]} != {
        "sender-buffer-growth",
        "sender-flight-growth",
        "receiver-buffer-remains-below-pressure-threshold",
        "sender-pressure-recovery",
    }:
        fail(f"sender-pressure evidence is incomplete: {sender_hypothesis}")
    print(
        f"incident analyzer valid: {len(arguments.manifest)} golden incidents, "
        f"{retained_recording_count} retained recordings, and generated JSONL path"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"incident analyzer test failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
