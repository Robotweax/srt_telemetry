#!/usr/bin/env python3

import argparse
import datetime as dt
import hashlib
import json
import platform
import statistics
import sys
import time
from pathlib import Path

from run_constant_loss_capture import (
    PAYLOAD_BYTES,
    FaultProxy,
    Peer,
    parse_prefixed_json,
    sha256_file,
    validate_sample,
)


SAMPLE_COUNT = 26
SAMPLE_INTERVAL_NANO = 50_000_000
SAMPLE_RATE_HZ = 20
TOTAL_MESSAGES = 240
SEND_PACING_MICROSECONDS = 2_000
TOTAL_EXPECTED_LOSS = 8

SCENARIOS = [
    {
        "name": "distributed-8x1",
        "activeSamples": [2, 3, 4, 5, 6, 7, 8, 9],
        "lossPattern": "burst",
        "dropEvery": 2,
        "burstStart": 2,
        "burstLength": 1,
        "dropsPerActiveSample": 1,
    },
    {
        "name": "concentrated-1x8",
        "activeSamples": [5],
        "lossPattern": "burst",
        "dropEvery": 2,
        "burstStart": 2,
        "burstLength": 8,
        "dropsPerActiveSample": 8,
    },
]


def payload_digest() -> str:
    digest = hashlib.sha256()
    payload = bytes([ord("A") + 1]) * PAYLOAD_BYTES
    for _ in range(TOTAL_MESSAGES):
        digest.update(payload)
    return digest.hexdigest()


def sample_pair(
    listener: Peer,
    caller: Peer,
    sample_index: int,
    receiver_provider: str,
    sender_provider: str,
) -> list:
    monotonic_nano = time.monotonic_ns()
    unix_nano = time.time_ns()
    command = f"SAMPLE {sample_index} {unix_nano} {monotonic_nano}"
    listener.send(command)
    caller.send(command)
    receiver = parse_prefixed_json(listener.wait_for("INCIDENT_SAMPLE"), "INCIDENT_SAMPLE")
    sender = parse_prefixed_json(caller.wait_for("INCIDENT_SAMPLE"), "INCIDENT_SAMPLE")
    validate_sample(receiver, receiver_provider, "receiver", sample_index)
    validate_sample(sender, sender_provider, "sender", sample_index)
    return [sender, receiver]


def summarize_role(samples: list, role: str) -> dict:
    selected = sorted(
        (sample for sample in samples if sample["role"] == role),
        key=lambda sample: sample["sampleIndex"],
    )
    timeline = []
    for sample in selected[1:]:
        delta = sample["delta"]
        loss_field = (
            "sender_loss_packet_delta" if role == "sender" else "receiver_loss_packet_delta"
        )
        timeline.append(
            {
                "sampleIndex": sample["sampleIndex"],
                "lossPacketDelta": delta[loss_field],
                "retransmittedPacketDelta": delta["retransmitted_packet_delta"],
                "sentPacketDelta": delta["sent_packet_delta"],
                "receivedPacketDelta": delta["received_packet_delta"],
                "uniqueSentPacketDelta": delta["unique_sent_packet_delta"],
                "uniqueReceivedPacketDelta": delta["unique_received_packet_delta"],
            }
        )
    return {
        "role": role,
        "totalLossPackets": sum(item["lossPacketDelta"] for item in timeline),
        "maximumLossPacketsPerSample": max(item["lossPacketDelta"] for item in timeline),
        "positiveLossSamples": [
            item["sampleIndex"] for item in timeline if item["lossPacketDelta"] > 0
        ],
        "totalRetransmittedPackets": sum(
            item["retransmittedPacketDelta"] for item in timeline
        ),
        "totalSentPackets": sum(item["sentPacketDelta"] for item in timeline),
        "totalReceivedPackets": sum(item["receivedPacketDelta"] for item in timeline),
        "totalUniqueSentPackets": sum(
            item["uniqueSentPacketDelta"] for item in timeline
        ),
        "totalUniqueReceivedPackets": sum(
            item["uniqueReceivedPacketDelta"] for item in timeline
        ),
        "timeline": timeline,
    }


def validate_scenario(result: dict, scenario: dict) -> None:
    roles = {role["role"]: role for role in result["roles"]}
    sender = roles["sender"]
    receiver = roles["receiver"]
    expected = {
        "sender.totalLossPackets": (sender["totalLossPackets"], TOTAL_EXPECTED_LOSS),
        "receiver.totalLossPackets": (receiver["totalLossPackets"], TOTAL_EXPECTED_LOSS),
        "sender.totalRetransmittedPackets": (
            sender["totalRetransmittedPackets"],
            TOTAL_EXPECTED_LOSS,
        ),
        "sender.totalUniqueSentPackets": (
            sender["totalUniqueSentPackets"],
            TOTAL_MESSAGES,
        ),
        "receiver.totalUniqueReceivedPackets": (
            receiver["totalUniqueReceivedPackets"],
            TOTAL_MESSAGES,
        ),
        "sender.totalSentPackets": (
            sender["totalSentPackets"],
            TOTAL_MESSAGES + TOTAL_EXPECTED_LOSS,
        ),
        "receiver.totalReceivedPackets": (
            receiver["totalReceivedPackets"],
            TOTAL_MESSAGES,
        ),
    }
    for name, (actual, wanted) in expected.items():
        if actual != wanted:
            raise RuntimeError(f"{name} expected {wanted}, got {actual}: {result}")
    original_packets = sum(stage["originalDataPackets"] for stage in result["proxyStages"])
    repeated_packets = sum(stage["repeatedDataPackets"] for stage in result["proxyStages"])
    dropped_packets = sum(
        stage["droppedOriginalDataPackets"] for stage in result["proxyStages"]
    )
    if original_packets != TOTAL_MESSAGES:
        raise RuntimeError(
            f"proxy expected {TOTAL_MESSAGES} original packets, got {original_packets}"
        )
    if repeated_packets < TOTAL_EXPECTED_LOSS:
        raise RuntimeError(
            f"proxy expected at least {TOTAL_EXPECTED_LOSS} repeats, got {repeated_packets}"
        )
    if dropped_packets != TOTAL_EXPECTED_LOSS:
        raise RuntimeError(
            f"proxy expected {TOTAL_EXPECTED_LOSS} drops, got {dropped_packets}"
        )
    for stage in result["proxyStages"]:
        expected_drops = scenario["dropsPerActiveSample"] if stage["active"] else 0
        if stage["droppedOriginalDataPackets"] != expected_drops:
            raise RuntimeError(f"unexpected proxy drop count: {stage}")
    sampling = result["sampling"]
    if sampling["deadlineMisses"] != 0:
        raise RuntimeError(f"sampling deadlines missed: {sampling}")
    if sampling["maximumIntervalNano"] >= 80_000_000:
        raise RuntimeError(f"sampling interval exceeded 80 ms: {sampling}")


def run_scenario(
    direction_name: str,
    receiver_peer: Path,
    sender_peer: Path,
    receiver_provider: str,
    sender_provider: str,
    scenario: dict,
) -> dict:
    listener = Peer(receiver_peer, ["--role", "listener"])
    caller = None
    proxy = None
    primary_error = None
    try:
        ready = parse_prefixed_json(listener.wait_for("READY"), "READY")
        listener_port = ready.get("port")
        if not isinstance(listener_port, int) or not 0 < listener_port <= 65_535:
            raise RuntimeError(f"invalid listener port: {ready}")
        proxy = FaultProxy(
            listener_port,
            scenario["lossPattern"],
            scenario["dropEvery"],
            scenario["burstStart"],
            scenario["burstLength"],
        )
        caller = Peer(sender_peer, ["--role", "caller", "--port", str(proxy.port)])
        listener.wait_for("CONNECTED")
        caller.wait_for("CONNECTED")

        samples = sample_pair(listener, caller, 0, receiver_provider, sender_provider)
        stages = []
        active_samples = set(scenario["activeSamples"])
        proxy.begin_stage(1, 1 in active_samples)

        start_transfer = f"START_TRANSFER 1 {TOTAL_MESSAGES} {SEND_PACING_MICROSECONDS}"
        listener.send(start_transfer)
        listener_started = parse_prefixed_json(
            listener.wait_for("TRANSFER_STARTED"), "TRANSFER_STARTED"
        )
        sampling_started = time.monotonic_ns()
        caller.send(start_transfer)
        caller_started = parse_prefixed_json(
            caller.wait_for("TRANSFER_STARTED"), "TRANSFER_STARTED"
        )
        expected_transfer = {"transferId": 1, "messages": TOTAL_MESSAGES}
        if listener_started != expected_transfer or caller_started != expected_transfer:
            raise RuntimeError(
                f"unexpected transfer startup: {listener_started}, {caller_started}"
            )

        sample_wall_nano = [sampling_started]
        schedule = []
        deadline_misses = 0
        for sample_index in range(1, SAMPLE_COUNT + 1):
            target = sampling_started + sample_index * SAMPLE_INTERVAL_NANO
            remaining = target - time.monotonic_ns()
            if remaining > 0:
                time.sleep(remaining / 1_000_000_000)
            started = time.monotonic_ns()
            if started - target >= SAMPLE_INTERVAL_NANO:
                deadline_misses += 1
            if sample_index < SAMPLE_COUNT:
                stage = proxy.advance_stage(
                    sample_index + 1, sample_index + 1 in active_samples
                )
            else:
                stage = proxy.finish_stage()
            stages.append(stage)
            samples.extend(
                sample_pair(
                    listener,
                    caller,
                    sample_index,
                    receiver_provider,
                    sender_provider,
                )
            )
            completed = time.monotonic_ns()
            sample_wall_nano.append(completed)
            schedule.append(
                {
                    "sampleIndex": sample_index,
                    "targetOffsetNano": sample_index * SAMPLE_INTERVAL_NANO,
                    "startLatenessNano": started - target,
                    "completionLatenessNano": completed - target,
                }
            )

        wait_transfer = f"WAIT_TRANSFER 1 {TOTAL_MESSAGES}"
        listener.send(wait_transfer)
        caller.send(wait_transfer)
        caller_transfer = parse_prefixed_json(
            caller.wait_for("TRANSFERRED"), "TRANSFERRED"
        )
        listener_transfer = parse_prefixed_json(
            listener.wait_for("TRANSFERRED"), "TRANSFERRED"
        )
        if caller_transfer != expected_transfer or listener_transfer != expected_transfer:
            raise RuntimeError(
                f"unexpected transfer completion: {caller_transfer}, {listener_transfer}"
            )

        proxy.stop()
        proxy = None
        listener.send("DONE")
        caller.send("DONE")
        listener.finish()
        caller.finish()

        actual_intervals = [
            sample_wall_nano[index] - sample_wall_nano[index - 1]
            for index in range(1, len(sample_wall_nano))
        ]
        result = {
            "direction": direction_name,
            "senderProvider": sender_provider,
            "receiverProvider": receiver_provider,
            "scenario": scenario["name"],
            "fault": {
                "activeSamples": scenario["activeSamples"],
                "dropsPerActiveSample": scenario["dropsPerActiveSample"],
                "totalDrops": TOTAL_EXPECTED_LOSS,
            },
            "sampling": {
                "targetIntervalNano": SAMPLE_INTERVAL_NANO,
                "deadlineMisses": deadline_misses,
                "minimumIntervalNano": min(actual_intervals),
                "meanIntervalNano": statistics.fmean(actual_intervals),
                "maximumIntervalNano": max(actual_intervals),
                "schedule": schedule,
            },
            "proxyStages": stages,
            "roles": [
                summarize_role(samples, "sender"),
                summarize_role(samples, "receiver"),
            ],
        }
        validate_scenario(result, scenario)
        return result
    except Exception as error:
        primary_error = error
        diagnostic = {
            "receiver": listener.describe(),
            "sender": caller.describe() if caller is not None else None,
            "proxy": proxy.describe() if proxy is not None else None,
        }
        raise RuntimeError(
            f"{error}; observability diagnostics: {json.dumps(diagnostic, sort_keys=True)}"
        ) from error
    finally:
        listener.stop()
        if caller is not None:
            caller.stop()
        if proxy is not None:
            try:
                proxy.stop()
            except Exception:
                if primary_error is None:
                    raise


def provider_record(
    provider: str,
    version: str,
    revision: str,
    build_profile: str,
    peer: Path,
    library: Path,
) -> dict:
    return {
        "provider": provider,
        "version": version,
        "revision": revision,
        "buildProfile": build_profile,
        "artifacts": {
            "peerExecutableSha256": sha256_file(peer),
            "providerLibrarySha256": sha256_file(library),
        },
    }


def load_cost_projection(arguments: argparse.Namespace) -> dict:
    source = json.loads(arguments.runtime_qualification.read_text(encoding="utf-8"))
    providers = {item["provider"]: item for item in source["providers"]}
    expected = {
        "robotweax-srt": (arguments.robotweax_revision, arguments.robotweax_library),
        "haivision-srt": (arguments.haivision_revision, arguments.haivision_library),
    }
    projection = []
    for provider, (revision, library) in expected.items():
        record = providers.get(provider)
        if record is None or record.get("revision") != revision:
            raise RuntimeError(f"runtime qualification revision mismatch for {provider}")
        if record["artifacts"]["providerLibrarySha256"] != sha256_file(library):
            raise RuntimeError(f"runtime qualification library mismatch for {provider}")
        p50_nano = record["benchmark"]["observations"]["providerCall"]["adapter"][
            "p50Nanoseconds"
        ]
        projected_nano_per_second = p50_nano * SAMPLE_RATE_HZ
        projection.append(
            {
                "provider": provider,
                "sourceAdapterP50Nanoseconds": p50_nano,
                "projectedSamplingNanosecondsPerSecond": projected_nano_per_second,
                "projectedOneCorePercent": projected_nano_per_second / 10_000_000,
            }
        )
    return {
        "sourceResultSha256": sha256_file(arguments.runtime_qualification),
        "sourceTelemetryRevision": source["telemetry"]["revision"],
        "benchmarkIsReleaseGate": source["interpretation"]["benchmarkIsReleaseGate"],
        "sampleRateHz": SAMPLE_RATE_HZ,
        "providers": projection,
        "combinedProjectedOneCorePercent": sum(
            item["projectedOneCorePercent"] for item in projection
        ),
    }


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare distributed and concentrated SRT loss at 20 Hz sampling"
    )
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--robotweax-library", type=Path, required=True)
    parser.add_argument("--robotweax-version", required=True)
    parser.add_argument("--robotweax-revision", required=True)
    parser.add_argument("--robotweax-build-profile", required=True)
    parser.add_argument("--haivision-peer", type=Path, required=True)
    parser.add_argument("--haivision-library", type=Path, required=True)
    parser.add_argument("--haivision-version", required=True)
    parser.add_argument("--haivision-revision", required=True)
    parser.add_argument("--haivision-build-profile", required=True)
    parser.add_argument("--telemetry-revision", required=True)
    parser.add_argument("--runtime-qualification", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    for path in [
        arguments.robotweax_peer,
        arguments.robotweax_library,
        arguments.haivision_peer,
        arguments.haivision_library,
        arguments.runtime_qualification,
    ]:
        if not path.is_file():
            raise RuntimeError(f"required artifact does not exist: {path}")
    if arguments.output.exists():
        raise RuntimeError(f"refusing to overwrite retained result: {arguments.output}")

    directions = [
        {
            "name": "robotweax-to-haivision",
            "senderPeer": arguments.robotweax_peer,
            "senderProvider": "robotweax-srt",
            "receiverPeer": arguments.haivision_peer,
            "receiverProvider": "haivision-srt",
        },
        {
            "name": "haivision-to-robotweax",
            "senderPeer": arguments.haivision_peer,
            "senderProvider": "haivision-srt",
            "receiverPeer": arguments.robotweax_peer,
            "receiverProvider": "robotweax-srt",
        },
    ]
    results = []
    for direction in directions:
        for scenario in SCENARIOS:
            results.append(
                run_scenario(
                    direction["name"],
                    direction["receiverPeer"],
                    direction["senderPeer"],
                    direction["receiverProvider"],
                    direction["senderProvider"],
                    scenario,
                )
            )

    for direction in directions:
        selected = [result for result in results if result["direction"] == direction["name"]]
        aggregates = {
            scenario["scenario"]: {
                role["role"]: (
                    role["totalLossPackets"],
                    role["totalRetransmittedPackets"],
                )
                for role in scenario["roles"]
            }
            for scenario in selected
        }
        if aggregates["distributed-8x1"] != aggregates["concentrated-1x8"]:
            raise RuntimeError(f"aggregate counters differ for {direction['name']}: {aggregates}")

        by_name = {scenario["scenario"]: scenario for scenario in selected}
        distributed = {role["role"]: role for role in by_name["distributed-8x1"]["roles"]}
        concentrated = {
            role["role"]: role for role in by_name["concentrated-1x8"]["roles"]
        }
        for role in ["sender", "receiver"]:
            distributed_role = distributed[role]
            concentrated_role = concentrated[role]
            if len(distributed_role["positiveLossSamples"]) <= len(
                concentrated_role["positiveLossSamples"]
            ) or distributed_role["maximumLossPacketsPerSample"] >= concentrated_role[
                "maximumLossPacketsPerSample"
            ]:
                raise RuntimeError(
                    f"20 Hz samples did not discriminate loss shape for "
                    f"{direction['name']} {role}: {distributed_role}, {concentrated_role}"
                )

    result = {
        "schemaVersion": "1alpha1",
        "kind": "public-srt-loss-shape-observability",
        "status": "pass",
        "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "operatorQuestion": (
            "Were equal aggregate packet losses concentrated in one 50 ms "
            "observation window or distributed across multiple 50 ms windows?"
        ),
        "telemetry": {
            "revision": arguments.telemetry_revision,
            "adapterVersion": "0.1.0-alpha.0",
        },
        "environment": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "pythonVersion": platform.python_version(),
            "transport": "ipv4-loopback-udp-proxy",
            "mode": "live-message",
            "sampleRateHz": SAMPLE_RATE_HZ,
            "sampleIntervalNano": SAMPLE_INTERVAL_NANO,
            "wallClockPaced": True,
            "sampleTimestampSource": "host-monotonic-clock",
        },
        "workload": {
            "samples": SAMPLE_COUNT,
            "continuousTransfer": True,
            "sendPacingMicroseconds": SEND_PACING_MICROSECONDS,
            "payloadBytesPerMessage": PAYLOAD_BYTES,
            "payloadSha256": payload_digest(),
            "totalMessagesPerScenario": TOTAL_MESSAGES,
        },
        "providers": [
            provider_record(
                "robotweax-srt",
                arguments.robotweax_version,
                arguments.robotweax_revision,
                arguments.robotweax_build_profile,
                arguments.robotweax_peer,
                arguments.robotweax_library,
            ),
            provider_record(
                "haivision-srt",
                arguments.haivision_version,
                arguments.haivision_revision,
                arguments.haivision_build_profile,
                arguments.haivision_peer,
                arguments.haivision_library,
            ),
        ],
        "scenarios": results,
        "costProjection": load_cost_projection(arguments),
        "decision": {
            "coarseLossShapeDiscriminated": True,
            "aggregateCountersEqual": True,
            "selectedApproach": "incident-mode-20hz-sdk-sampling",
            "protocolExtensionRequiredForOperatorQuestion": False,
            "releaseCostGatePassed": False,
            "reason": (
                "The distributed case produces eight one-packet loss windows; "
                "the concentrated case produces one eight-packet loss window in "
                "both provider directions while aggregate loss and retransmission "
                "totals remain equal."
            ),
            "residualLimitation": (
                "Sampling distinguishes concentration across 50 ms windows but "
                "does not prove packet-sequence adjacency inside one window."
            ),
        },
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"loss-shape observability qualification passed: {arguments.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"loss-shape observability qualification failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
