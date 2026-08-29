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


def validate_loss_result(result: dict, expected_format: str) -> None:
    if result.get("schemaVersion") != "1alpha1" or result.get("status") != "pass":
        fail("analysis result identity is invalid")
    if result["source"]["format"] != expected_format:
        fail(f"analysis source format mismatch: {result['source']['format']}")
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
    hypotheses = result["hypotheses"]
    if len(hypotheses) != 4 or {item["id"] for item in hypotheses} != {
        "transmission-path-loss-with-recovery",
        "transmission-path-rtt-step-with-recovery",
        "sender-side-pressure",
        "receiver-side-pressure",
    }:
        fail("analysis does not contain the four required hypotheses")
    if result["selectedHypothesis"] != {
        "id": "transmission-path-loss-with-recovery",
        "assessment": "supported",
        "rationale": (
            "Aligned sender loss, receiver loss, and retransmission deltas identify "
            "recoverable path loss without sufficient endpoint-pressure evidence."
        ),
    }:
        fail(f"unexpected selected hypothesis: {result['selectedHypothesis']}")
    if not all(item["missingEvidence"] for item in hypotheses):
        fail("every hypothesis must expose its missing evidence")
    if not result["nonClaims"]:
        fail("analysis must retain explicit non-claims")


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
        result = analyzer.build_analysis(analyzer.load_manifest(manifest))
        validate_loss_result(result, "robotweax-srt-telemetry-csv-v1")
        manifest_results.append(result)
        manifest_document = json.loads(manifest.read_text(encoding="utf-8"))
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
            validate_loss_result(jsonl_result, "robotweax-srt-telemetry-jsonl-v1")
            retained = json.loads(
                (manifest.parent / recordings["analysis"]["file"]).read_text(
                    encoding="utf-8"
                )
            )
            if jsonl_result != retained:
                fail(f"retained JSONL analysis is not reproducible: {manifest}")
            for field in ["timeline", "totals", "hypotheses", "selectedHypothesis"]:
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
