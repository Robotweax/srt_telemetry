#!/usr/bin/env python3

import argparse
import hashlib
import json
import math
import re
import sys
from datetime import datetime
from pathlib import Path
from typing import Optional


SAMPLE_COUNT = 26
SAMPLE_INTERVAL_NANO = 50_000_000
SAMPLE_RATE_HZ = 20
TOTAL_MESSAGES = 240
TOTAL_LOSS = 8
PAYLOAD_BYTES = 1_316
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")

EXPECTED_SCENARIOS = {
    ("robotweax-to-haivision", "distributed-8x1"): {
        "sender": "robotweax-srt",
        "receiver": "haivision-srt",
        "activeSamples": list(range(2, 10)),
        "dropsPerActiveSample": 1,
    },
    ("robotweax-to-haivision", "concentrated-1x8"): {
        "sender": "robotweax-srt",
        "receiver": "haivision-srt",
        "activeSamples": [5],
        "dropsPerActiveSample": 8,
    },
    ("haivision-to-robotweax", "distributed-8x1"): {
        "sender": "haivision-srt",
        "receiver": "robotweax-srt",
        "activeSamples": list(range(2, 10)),
        "dropsPerActiveSample": 1,
    },
    ("haivision-to-robotweax", "concentrated-1x8"): {
        "sender": "haivision-srt",
        "receiver": "robotweax-srt",
        "activeSamples": [5],
        "dropsPerActiveSample": 8,
    },
}


def fail(message: str) -> None:
    raise RuntimeError(message)


def require_exact_keys(value: object, keys: set, name: str) -> dict:
    if not isinstance(value, dict):
        fail(f"{name} is not an object")
    actual = set(value)
    if actual != keys:
        fail(
            f"{name} keys differ: missing {sorted(keys - actual)}, "
            f"unexpected {sorted(actual - keys)}"
        )
    return value


def require_non_negative_integer(value: object, name: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        fail(f"{name} must be a non-negative integer: {value!r}")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_identity(result: dict) -> None:
    require_exact_keys(
        result,
        {
            "schemaVersion",
            "kind",
            "status",
            "generatedAt",
            "operatorQuestion",
            "telemetry",
            "environment",
            "workload",
            "providers",
            "scenarios",
            "costProjection",
            "decision",
        },
        "result",
    )
    if result["schemaVersion"] != "1alpha1":
        fail(f"unsupported schema version: {result['schemaVersion']}")
    if result["kind"] != "public-srt-loss-shape-observability":
        fail(f"unexpected result kind: {result['kind']}")
    if result["status"] != "pass":
        fail(f"result did not pass: {result['status']}")
    try:
        datetime.fromisoformat(result["generatedAt"].replace("Z", "+00:00"))
    except (AttributeError, ValueError) as error:
        raise RuntimeError(f"invalid generatedAt: {result['generatedAt']!r}") from error
    if "50 ms" not in result["operatorQuestion"]:
        fail("operator question does not declare the 50 ms observation window")

    telemetry = require_exact_keys(
        result["telemetry"], {"revision", "adapterVersion"}, "telemetry"
    )
    if not isinstance(telemetry["revision"], str) or len(telemetry["revision"]) < 7:
        fail("telemetry revision is missing")
    if telemetry["adapterVersion"] != "0.1.0-alpha.0":
        fail(f"unexpected adapter version: {telemetry['adapterVersion']}")


def validate_environment_and_workload(result: dict) -> None:
    environment = require_exact_keys(
        result["environment"],
        {
            "platform",
            "machine",
            "pythonVersion",
            "transport",
            "mode",
            "sampleRateHz",
            "sampleIntervalNano",
            "wallClockPaced",
            "sampleTimestampSource",
        },
        "environment",
    )
    expected_environment = {
        "transport": "ipv4-loopback-udp-proxy",
        "mode": "live-message",
        "sampleRateHz": SAMPLE_RATE_HZ,
        "sampleIntervalNano": SAMPLE_INTERVAL_NANO,
        "wallClockPaced": True,
        "sampleTimestampSource": "host-monotonic-clock",
    }
    for name, expected in expected_environment.items():
        if environment[name] != expected:
            fail(f"environment {name} expected {expected!r}, got {environment[name]!r}")
    for name in ["platform", "machine", "pythonVersion"]:
        if not isinstance(environment[name], str) or not environment[name]:
            fail(f"environment {name} is empty")

    workload = require_exact_keys(
        result["workload"],
        {
            "samples",
            "continuousTransfer",
            "sendPacingMicroseconds",
            "payloadBytesPerMessage",
            "payloadSha256",
            "totalMessagesPerScenario",
        },
        "workload",
    )
    expected_workload = {
        "samples": SAMPLE_COUNT,
        "continuousTransfer": True,
        "sendPacingMicroseconds": 2_000,
        "payloadBytesPerMessage": PAYLOAD_BYTES,
        "totalMessagesPerScenario": TOTAL_MESSAGES,
    }
    for name, expected in expected_workload.items():
        if workload[name] != expected:
            fail(f"workload {name} expected {expected!r}, got {workload[name]!r}")
    expected_payload = hashlib.sha256(
        bytes([ord("B")]) * PAYLOAD_BYTES * TOTAL_MESSAGES
    ).hexdigest()
    if workload["payloadSha256"] != expected_payload:
        fail("payload digest does not match the declared continuous transfer")


def validate_providers(result: dict) -> None:
    providers = result["providers"]
    if not isinstance(providers, list) or len(providers) != 2:
        fail("result must contain exactly two provider records")
    observed = set()
    for index, provider in enumerate(providers):
        provider = require_exact_keys(
            provider,
            {"provider", "version", "revision", "buildProfile", "artifacts"},
            f"providers[{index}]",
        )
        name = provider["provider"]
        if name not in {"robotweax-srt", "haivision-srt"} or name in observed:
            fail(f"invalid or duplicate provider: {name}")
        observed.add(name)
        for field in ["version", "revision", "buildProfile"]:
            if not isinstance(provider[field], str) or not provider[field]:
                fail(f"empty provider field {name}.{field}")
        artifacts = require_exact_keys(
            provider["artifacts"],
            {"peerExecutableSha256", "providerLibrarySha256"},
            f"{name}.artifacts",
        )
        for field, digest in artifacts.items():
            if not isinstance(digest, str) or not SHA256_PATTERN.fullmatch(digest):
                fail(f"invalid artifact digest {name}.{field}")


def validate_sampling(sampling: object, name: str) -> None:
    sampling = require_exact_keys(
        sampling,
        {
            "targetIntervalNano",
            "deadlineMisses",
            "minimumIntervalNano",
            "meanIntervalNano",
            "maximumIntervalNano",
            "schedule",
        },
        name,
    )
    if sampling["targetIntervalNano"] != SAMPLE_INTERVAL_NANO:
        fail(f"{name} target interval is not 50 ms")
    if sampling["deadlineMisses"] != 0:
        fail(f"{name} contains sampling deadline misses")
    minimum = require_non_negative_integer(
        sampling["minimumIntervalNano"], f"{name}.minimumIntervalNano"
    )
    maximum = require_non_negative_integer(
        sampling["maximumIntervalNano"], f"{name}.maximumIntervalNano"
    )
    mean = sampling["meanIntervalNano"]
    if not isinstance(mean, (int, float)) or isinstance(mean, bool):
        fail(f"{name}.meanIntervalNano is not numeric")
    if not minimum <= mean <= maximum or maximum >= 80_000_000:
        fail(f"{name} has invalid or excessive sampling intervals")
    schedule = sampling["schedule"]
    if not isinstance(schedule, list) or len(schedule) != SAMPLE_COUNT:
        fail(f"{name} must contain {SAMPLE_COUNT} schedule entries")
    for index, entry in enumerate(schedule, start=1):
        entry = require_exact_keys(
            entry,
            {
                "sampleIndex",
                "targetOffsetNano",
                "startLatenessNano",
                "completionLatenessNano",
            },
            f"{name}.schedule[{index}]",
        )
        if entry["sampleIndex"] != index:
            fail(f"{name} schedule indexes are not contiguous")
        if entry["targetOffsetNano"] != index * SAMPLE_INTERVAL_NANO:
            fail(f"{name} schedule target mismatch at sample {index}")
        start = require_non_negative_integer(
            entry["startLatenessNano"], f"{name}.startLatenessNano"
        )
        completion = require_non_negative_integer(
            entry["completionLatenessNano"], f"{name}.completionLatenessNano"
        )
        if completion < start:
            fail(f"{name} sample {index} completed before it started")


def validate_stages(stages: object, expected: dict, name: str) -> None:
    if not isinstance(stages, list) or len(stages) != SAMPLE_COUNT:
        fail(f"{name} must contain {SAMPLE_COUNT} proxy stages")
    active_samples = set(expected["activeSamples"])
    original_total = 0
    repeat_total = 0
    drop_total = 0
    for index, stage in enumerate(stages, start=1):
        stage = require_exact_keys(
            stage,
            {
                "sampleIndex",
                "active",
                "originalDataPackets",
                "repeatedDataPackets",
                "droppedOriginalDataPackets",
                "forwardedDataPackets",
                "forwardedControlPackets",
                "reversePackets",
            },
            f"{name}.proxyStages[{index}]",
        )
        if stage["sampleIndex"] != index or stage["active"] != (index in active_samples):
            fail(f"{name} proxy stage identity mismatch at sample {index}")
        for field in [
            "originalDataPackets",
            "repeatedDataPackets",
            "droppedOriginalDataPackets",
            "forwardedDataPackets",
            "forwardedControlPackets",
            "reversePackets",
        ]:
            require_non_negative_integer(stage[field], f"{name}.{field}")
        expected_drops = expected["dropsPerActiveSample"] if stage["active"] else 0
        if stage["droppedOriginalDataPackets"] != expected_drops:
            fail(f"{name} proxy drop mismatch at sample {index}")
        original_total += stage["originalDataPackets"]
        repeat_total += stage["repeatedDataPackets"]
        drop_total += stage["droppedOriginalDataPackets"]
    if original_total != TOTAL_MESSAGES or drop_total != TOTAL_LOSS:
        fail(f"{name} proxy totals do not match the workload")
    if repeat_total < TOTAL_LOSS:
        fail(f"{name} proxy did not observe all retransmissions")


def validate_roles(roles: object, expected: dict, name: str) -> dict:
    if not isinstance(roles, list) or len(roles) != 2:
        fail(f"{name} must contain sender and receiver role summaries")
    by_role = {}
    fields = [
        "lossPacketDelta",
        "retransmittedPacketDelta",
        "sentPacketDelta",
        "receivedPacketDelta",
        "uniqueSentPacketDelta",
        "uniqueReceivedPacketDelta",
    ]
    summary_fields = {
        "lossPacketDelta": "totalLossPackets",
        "retransmittedPacketDelta": "totalRetransmittedPackets",
        "sentPacketDelta": "totalSentPackets",
        "receivedPacketDelta": "totalReceivedPackets",
        "uniqueSentPacketDelta": "totalUniqueSentPackets",
        "uniqueReceivedPacketDelta": "totalUniqueReceivedPackets",
    }
    for role_value in roles:
        role_value = require_exact_keys(
            role_value,
            {
                "role",
                "totalLossPackets",
                "maximumLossPacketsPerSample",
                "positiveLossSamples",
                "totalRetransmittedPackets",
                "totalSentPackets",
                "totalReceivedPackets",
                "totalUniqueSentPackets",
                "totalUniqueReceivedPackets",
                "timeline",
            },
            f"{name}.role",
        )
        role = role_value["role"]
        if role not in {"sender", "receiver"} or role in by_role:
            fail(f"{name} contains an invalid or duplicate role: {role}")
        by_role[role] = role_value
        timeline = role_value["timeline"]
        if not isinstance(timeline, list) or len(timeline) != SAMPLE_COUNT:
            fail(f"{name}.{role} must contain {SAMPLE_COUNT} timeline entries")
        totals = {field: 0 for field in fields}
        positive = []
        maximum_loss = 0
        for index, entry in enumerate(timeline, start=1):
            entry = require_exact_keys(
                entry, {"sampleIndex", *fields}, f"{name}.{role}.timeline[{index}]"
            )
            if entry["sampleIndex"] != index:
                fail(f"{name}.{role} timeline indexes are not contiguous")
            for field in fields:
                totals[field] += require_non_negative_integer(
                    entry[field], f"{name}.{role}.{field}"
                )
            if entry["lossPacketDelta"] > 0:
                positive.append(index)
            maximum_loss = max(maximum_loss, entry["lossPacketDelta"])
        for field, summary in summary_fields.items():
            if role_value[summary] != totals[field]:
                fail(f"{name}.{role} summary {summary} does not match its timeline")
        if role_value["positiveLossSamples"] != positive:
            fail(f"{name}.{role} positive-loss summary does not match its timeline")
        if role_value["maximumLossPacketsPerSample"] != maximum_loss:
            fail(f"{name}.{role} maximum-loss summary does not match its timeline")

    expected_positive = expected["activeSamples"]
    expected_maximum = expected["dropsPerActiveSample"]
    for role in ["sender", "receiver"]:
        role_value = by_role[role]
        if role_value["totalLossPackets"] != TOTAL_LOSS:
            fail(f"{name}.{role} loss total is not {TOTAL_LOSS}")
        if role_value["positiveLossSamples"] != expected_positive:
            fail(f"{name}.{role} loss windows do not match the fault shape")
        if role_value["maximumLossPacketsPerSample"] != expected_maximum:
            fail(f"{name}.{role} maximum loss does not match the fault shape")
    sender = by_role["sender"]
    receiver = by_role["receiver"]
    expected_totals = {
        "sender.totalRetransmittedPackets": (sender["totalRetransmittedPackets"], TOTAL_LOSS),
        "sender.totalSentPackets": (sender["totalSentPackets"], TOTAL_MESSAGES + TOTAL_LOSS),
        "sender.totalUniqueSentPackets": (sender["totalUniqueSentPackets"], TOTAL_MESSAGES),
        "sender.totalReceivedPackets": (sender["totalReceivedPackets"], 0),
        "sender.totalUniqueReceivedPackets": (sender["totalUniqueReceivedPackets"], 0),
        "receiver.totalRetransmittedPackets": (receiver["totalRetransmittedPackets"], 0),
        "receiver.totalSentPackets": (receiver["totalSentPackets"], 0),
        "receiver.totalUniqueSentPackets": (receiver["totalUniqueSentPackets"], 0),
        "receiver.totalReceivedPackets": (receiver["totalReceivedPackets"], TOTAL_MESSAGES),
        "receiver.totalUniqueReceivedPackets": (
            receiver["totalUniqueReceivedPackets"],
            TOTAL_MESSAGES,
        ),
    }
    for field, (actual, wanted) in expected_totals.items():
        if actual != wanted:
            fail(f"{name}.{field} expected {wanted}, got {actual}")
    if [entry["lossPacketDelta"] for entry in sender["timeline"]] != [
        entry["lossPacketDelta"] for entry in receiver["timeline"]
    ]:
        fail(f"{name} sender and receiver loss timelines differ")
    return by_role


def validate_scenarios(result: dict) -> None:
    scenarios = result["scenarios"]
    if not isinstance(scenarios, list) or len(scenarios) != len(EXPECTED_SCENARIOS):
        fail("result must contain exactly four scenarios")
    observed = {}
    for index, scenario in enumerate(scenarios):
        scenario = require_exact_keys(
            scenario,
            {
                "direction",
                "senderProvider",
                "receiverProvider",
                "scenario",
                "fault",
                "sampling",
                "proxyStages",
                "roles",
            },
            f"scenarios[{index}]",
        )
        key = (scenario["direction"], scenario["scenario"])
        expected = EXPECTED_SCENARIOS.get(key)
        if expected is None or key in observed:
            fail(f"invalid or duplicate scenario: {key}")
        if scenario["senderProvider"] != expected["sender"]:
            fail(f"{key} sender provider mismatch")
        if scenario["receiverProvider"] != expected["receiver"]:
            fail(f"{key} receiver provider mismatch")
        fault = require_exact_keys(
            scenario["fault"],
            {"activeSamples", "dropsPerActiveSample", "totalDrops"},
            f"{key}.fault",
        )
        if fault != {
            "activeSamples": expected["activeSamples"],
            "dropsPerActiveSample": expected["dropsPerActiveSample"],
            "totalDrops": TOTAL_LOSS,
        }:
            fail(f"{key} fault declaration mismatch")
        validate_sampling(scenario["sampling"], f"{key}.sampling")
        validate_stages(scenario["proxyStages"], expected, str(key))
        observed[key] = validate_roles(scenario["roles"], expected, str(key))
    if set(observed) != set(EXPECTED_SCENARIOS):
        fail("scenario matrix is incomplete")

    for direction in ["robotweax-to-haivision", "haivision-to-robotweax"]:
        distributed = observed[(direction, "distributed-8x1")]
        concentrated = observed[(direction, "concentrated-1x8")]
        for role in ["sender", "receiver"]:
            for field in [
                "totalLossPackets",
                "totalRetransmittedPackets",
                "totalSentPackets",
                "totalReceivedPackets",
                "totalUniqueSentPackets",
                "totalUniqueReceivedPackets",
            ]:
                if distributed[role][field] != concentrated[role][field]:
                    fail(f"{direction}.{role} aggregate {field} differs between shapes")


def validate_cost_projection(
    result: dict, runtime_qualification: Optional[Path]
) -> None:
    projection = require_exact_keys(
        result["costProjection"],
        {
            "sourceResultSha256",
            "sourceTelemetryRevision",
            "benchmarkIsReleaseGate",
            "sampleRateHz",
            "providers",
            "combinedProjectedOneCorePercent",
        },
        "costProjection",
    )
    if not SHA256_PATTERN.fullmatch(projection["sourceResultSha256"]):
        fail("cost projection source digest is invalid")
    if projection["benchmarkIsReleaseGate"] is not False:
        fail("candidate runtime benchmark cannot be presented as a release gate")
    if projection["sampleRateHz"] != SAMPLE_RATE_HZ:
        fail("cost projection sample rate mismatch")
    providers = projection["providers"]
    if not isinstance(providers, list) or len(providers) != 2:
        fail("cost projection must contain both providers")
    observed = set()
    combined = 0.0
    for item in providers:
        item = require_exact_keys(
            item,
            {
                "provider",
                "sourceAdapterP50Nanoseconds",
                "projectedSamplingNanosecondsPerSecond",
                "projectedOneCorePercent",
            },
            "costProjection.provider",
        )
        provider = item["provider"]
        if provider not in {"robotweax-srt", "haivision-srt"} or provider in observed:
            fail(f"invalid cost projection provider: {provider}")
        observed.add(provider)
        p50 = item["sourceAdapterP50Nanoseconds"]
        projected_nano = item["projectedSamplingNanosecondsPerSecond"]
        projected_percent = item["projectedOneCorePercent"]
        for name, value in [
            ("p50", p50),
            ("projected nanoseconds", projected_nano),
            ("projected percent", projected_percent),
        ]:
            if not isinstance(value, (int, float)) or isinstance(value, bool) or value <= 0:
                fail(f"invalid {provider} {name}: {value!r}")
        if not math.isclose(projected_nano, p50 * SAMPLE_RATE_HZ, rel_tol=1e-12):
            fail(f"{provider} projected nanoseconds do not match p50 and sample rate")
        if not math.isclose(projected_percent, projected_nano / 10_000_000, rel_tol=1e-12):
            fail(f"{provider} projected core percent is inconsistent")
        combined += projected_percent
    if not math.isclose(
        projection["combinedProjectedOneCorePercent"], combined, rel_tol=1e-12
    ):
        fail("combined projected core percent is inconsistent")

    if runtime_qualification is not None:
        if sha256_file(runtime_qualification) != projection["sourceResultSha256"]:
            fail("runtime qualification digest does not match cost projection source")
        source = json.loads(runtime_qualification.read_text(encoding="utf-8"))
        if source["telemetry"]["revision"] != projection["sourceTelemetryRevision"]:
            fail("runtime qualification telemetry revision does not match projection")


def validate_decision(result: dict) -> None:
    decision = require_exact_keys(
        result["decision"],
        {
            "coarseLossShapeDiscriminated",
            "aggregateCountersEqual",
            "selectedApproach",
            "protocolExtensionRequiredForOperatorQuestion",
            "releaseCostGatePassed",
            "reason",
            "residualLimitation",
        },
        "decision",
    )
    expected = {
        "coarseLossShapeDiscriminated": True,
        "aggregateCountersEqual": True,
        "selectedApproach": "incident-mode-20hz-sdk-sampling",
        "protocolExtensionRequiredForOperatorQuestion": False,
        "releaseCostGatePassed": False,
    }
    for name, wanted in expected.items():
        if decision[name] != wanted:
            fail(f"decision {name} expected {wanted!r}, got {decision[name]!r}")
    for name in ["reason", "residualLimitation"]:
        if not isinstance(decision[name], str) or not decision[name]:
            fail(f"decision {name} is empty")


def validate_result(
    result_path: Path, runtime_qualification: Optional[Path]
) -> None:
    result = json.loads(result_path.read_text(encoding="utf-8"))
    if not isinstance(result, dict):
        fail("result root is not an object")
    validate_identity(result)
    validate_environment_and_workload(result)
    validate_providers(result)
    validate_scenarios(result)
    validate_cost_projection(result, runtime_qualification)
    validate_decision(result)
    print(
        "loss-shape observability result valid: "
        f"{len(result['scenarios'])} scenarios at {SAMPLE_RATE_HZ} Hz"
    )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate a public SRT loss-shape observability result"
    )
    parser.add_argument("result", type=Path)
    parser.add_argument("--runtime-qualification", type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if not arguments.result.is_file():
        fail(f"result does not exist: {arguments.result}")
    if (
        arguments.runtime_qualification is not None
        and not arguments.runtime_qualification.is_file()
    ):
        fail(f"runtime qualification does not exist: {arguments.runtime_qualification}")
    validate_result(arguments.result.resolve(), arguments.runtime_qualification)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"loss-shape observability validation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
