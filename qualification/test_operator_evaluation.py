#!/usr/bin/env python3

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str) -> None:
    raise RuntimeError(message)


def run(command, expect_success=True):
    completed = subprocess.run(command, capture_output=True, text=True)
    if expect_success and completed.returncode != 0:
        rendered_command = " ".join(str(item) for item in command)
        fail(
            f"command failed ({completed.returncode}): {rendered_command}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    if not expect_success and completed.returncode == 0:
        fail(
            f"command unexpectedly succeeded: {' '.join(str(item) for item in command)}"
        )
    return completed


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, document: dict) -> None:
    path.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def keys_recursive(value):
    if isinstance(value, dict):
        for key, child in value.items():
            yield key
            yield from keys_recursive(child)
    elif isinstance(value, list):
        for child in value:
            yield from keys_recursive(child)


def build_response(study: dict, packet_dir: Path, participant: str, arm: str) -> dict:
    tasks = []
    for assignment in study["arms"][arm]:
        scenario = next(
            item
            for item in study["scenarios"]
            if item["scenarioId"] == assignment["scenarioId"]
        )
        reference = read_json(packet_dir / scenario["reference"]["file"])
        report_condition = assignment["condition"] == "incident-report"
        if report_condition:
            first = reference["firstDeviationSample"]
            domain = reference["failureDomain"]
            hypotheses = reference["supportedHypotheses"]
            missing = reference["missingEvidence"]
            timing = {
                "timeToFirstDeviationMillis": 20_000,
                "timeToFailureDomainMillis": 30_000,
                "completionMillis": 50_000,
            }
            confidence = 5
        elif participant == "fixture-a":
            first = reference["firstDeviationSample"] + 1
            domain = "sender-endpoint"
            hypotheses = ["sender-side-pressure"]
            missing = []
            timing = {
                "timeToFirstDeviationMillis": 120_000,
                "timeToFailureDomainMillis": 180_000,
                "completionMillis": 220_000,
            }
            confidence = 2
        else:
            first = reference["firstDeviationSample"]
            domain = reference["failureDomain"]
            hypotheses = reference["supportedHypotheses"] + ["sender-side-pressure"]
            missing = ["exact-failing-hop"]
            timing = {
                "timeToFirstDeviationMillis": 90_000,
                "timeToFailureDomainMillis": 150_000,
                "completionMillis": 190_000,
            }
            confidence = 3
        tasks.append(
            {
                "scenarioId": assignment["scenarioId"],
                "condition": assignment["condition"],
                "timing": timing,
                "answer": {
                    "firstDeviationSample": first,
                    "failureDomain": domain,
                    "supportedHypotheses": hypotheses,
                    "missingEvidence": missing,
                    "confidence": confidence,
                    "notes": "synthetic harness fixture",
                },
            }
        )
    return {
        "schemaVersion": "1alpha1",
        "kind": "robotweax-srt-operator-evaluation-response",
        "studyId": study["studyId"],
        "participantId": participant,
        "evidenceClass": "synthetic-harness",
        "arm": arm,
        "collectionMode": "automated-test-fixture",
        "tasks": tasks,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Test the operator evaluation harness")
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--analyzer", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, action="append", required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    arguments = parser.parse_args()
    if len(arguments.manifest) < 2:
        fail("test requires at least two retained JSONL incident manifests")

    arguments.work_dir.mkdir(parents=True, exist_ok=True)
    temporary = Path(
        tempfile.mkdtemp(prefix="operator-evaluation-", dir=arguments.work_dir)
    )
    try:
        packet_dir = temporary / "packet"
        prepare = [
            sys.executable,
            str(arguments.tool.resolve()),
            "prepare",
            "--study-id",
            "loss-rtt-pressure-comparison-candidate",
            "--analyzer",
            str(arguments.analyzer.resolve()),
            "--output-dir",
            str(packet_dir),
        ]
        for manifest in arguments.manifest:
            prepare.extend(["--manifest", str(manifest.resolve())])
        run(prepare)
        run(prepare, expect_success=False)

        study_path = packet_dir / "study.json"
        study = read_json(study_path)
        if study.get("kind") != "robotweax-srt-operator-evaluation-study":
            fail("prepared study has the wrong identity")
        if set(study["arms"]) != {"arm-a", "arm-b"}:
            fail("prepared study does not expose both counterbalanced arms")
        scenario_ids = {scenario["scenarioId"] for scenario in study["scenarios"]}
        if len(scenario_ids) != len(arguments.manifest):
            fail("prepared study does not retain every requested scenario")
        for arm, assignments in study["arms"].items():
            if {assignment["scenarioId"] for assignment in assignments} != scenario_ids:
                fail(f"{arm} does not cover every scenario once")
        for scenario_id in scenario_ids:
            conditions = {
                assignment["condition"]
                for assignments in study["arms"].values()
                for assignment in assignments
                if assignment["scenarioId"] == scenario_id
            }
            if conditions != {"raw-statistics", "incident-report"}:
                fail(f"scenario is not counterbalanced: {scenario_id}")
        if study["gateLimits"]["minimumDistinctFailureClasses"] != 5:
            fail("study does not preserve the strategy's five-failure-class gate")

        forbidden_raw_keys = {
            "analysis",
            "deviating",
            "failureDomain",
            "firstDeviationSample",
            "hypotheses",
            "selectedHypothesis",
            "supportingEvidence",
        }
        for scenario in study["scenarios"]:
            raw_view = read_json(
                packet_dir / scenario["views"]["raw-statistics"]["file"]
            )
            if forbidden_raw_keys & set(keys_recursive(raw_view)):
                fail("raw-statistics condition leaks diagnostic inference")
            report_view = read_json(
                packet_dir / scenario["views"]["incident-report"]["file"]
            )
            if (
                report_view.get("report", {})
                .get("selectedHypothesis", {})
                .get("assessment")
                != "supported"
            ):
                fail("incident-report condition has no supported reference result")

        session_paths = []
        for participant, arm in [("fixture-a", "arm-a"), ("fixture-b", "arm-b")]:
            response_path = temporary / f"{participant}-response.json"
            session_path = temporary / f"{participant}-session.json"
            write_json(
                response_path, build_response(study, packet_dir, participant, arm)
            )
            run(
                [
                    sys.executable,
                    str(arguments.tool.resolve()),
                    "score",
                    "--study",
                    str(study_path),
                    "--response",
                    str(response_path),
                    "--output",
                    str(session_path),
                ]
            )
            session = read_json(session_path)
            if session.get("evidenceClass") != "synthetic-harness":
                fail("synthetic response lost its evidence classification")
            if {task["condition"] for task in session["tasks"]} != {
                "raw-statistics",
                "incident-report",
            }:
                fail("scored session does not contain both conditions")
            session_paths.append(session_path)

        summary_path = temporary / "summary.json"
        summarize = [
            sys.executable,
            str(arguments.tool.resolve()),
            "summarize",
            "--study",
            str(study_path),
            "--output",
            str(summary_path),
        ]
        for session_path in session_paths:
            summarize.extend(["--session", str(session_path)])
        run(summarize)
        summary = read_json(summary_path)
        if summary["claimAssessment"]["status"] != "harness-only":
            fail("synthetic sessions were mistaken for product evidence")
        if summary["claimAssessment"]["eligibleForHumanGateReview"]:
            fail("synthetic harness fixtures must not close the diagnostic gate")
        expected_failure_classes = sorted(
            {
                read_json(manifest.resolve())["failureClass"]
                for manifest in arguments.manifest
            }
        )
        if summary["distinctFailureClasses"] != expected_failure_classes:
            fail("summary failure-class accounting is incorrect")
        comparison = summary["comparison"]
        if comparison["pairedParticipantCount"] != 2:
            fail("summary does not retain both paired participants")
        if any(
            statistics["taskCount"] != len(arguments.manifest)
            for statistics in summary["statistics"].values()
        ):
            fail("summary condition task accounting is incorrect")
        if comparison["medianRawMinusReportTimeToFailureDomainMillis"] <= 0:
            fail("paired timing comparison has the wrong direction")
        if comparison["meanReportMinusRawFailureDomainCorrect"] < 0:
            fail("paired accuracy comparison has the wrong direction")
        if comparison["meanRawMinusReportUnsupportedHypothesisCount"] <= 0:
            fail("unsupported hypothesis comparison has the wrong direction")

        original_session_text = session_paths[0].read_text(encoding="utf-8")
        tampered_session = json.loads(original_session_text)
        tampered_session["tasks"][0]["answer"]["supportedHypotheses"] = []
        write_json(session_paths[0], tampered_session)
        tampered_session_summary = list(summarize)
        tampered_session_summary[
            tampered_session_summary.index(str(summary_path))
        ] = str(temporary / "tampered-session-summary.json")
        run(tampered_session_summary, expect_success=False)
        session_paths[0].write_text(original_session_text, encoding="utf-8")

        first_scenario = study["scenarios"][0]
        raw_path = packet_dir / first_scenario["views"]["raw-statistics"]["file"]
        raw_path.write_text(
            raw_path.read_text(encoding="utf-8") + "\n", encoding="utf-8"
        )
        tampered_packet_summary = list(summarize)
        tampered_packet_summary[tampered_packet_summary.index(str(summary_path))] = str(
            temporary / "tampered-packet-summary.json"
        )
        run(tampered_packet_summary, expect_success=False)
    finally:
        shutil.rmtree(temporary, ignore_errors=True)
    print("operator evaluation harness tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"operator evaluation harness test failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
