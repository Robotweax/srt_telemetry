#!/usr/bin/env python3

import argparse
import datetime as dt
import hashlib
import json
import platform
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

from run_constant_loss_capture import (
    FIRST_ACTIVE_SAMPLE,
    INCIDENT_PATTERN,
    LAST_ACTIVE_SAMPLE,
    LAST_SAMPLE,
    LOGICAL_INTERVAL_NANO,
    PAYLOAD_BYTES,
    Peer,
    artifact_manifest,
    parse_prefixed_json,
    peer_recording_arguments,
    provider_manifest,
    sample_pair,
    samples_csv,
    sha256_file,
)


def capture_direction(arguments: argparse.Namespace) -> dict:
    providers = {
        "robotweax": {
            "name": "Robotweax SRT",
            "provider": "robotweax-srt",
            "peer": arguments.robotweax_peer,
            "library": arguments.robotweax_library,
            "version": arguments.robotweax_version,
            "revision": arguments.robotweax_revision,
            "build_profile": arguments.robotweax_build_profile,
        },
        "haivision": {
            "name": "Haivision SRT",
            "provider": "haivision-srt",
            "peer": arguments.haivision_peer,
            "library": arguments.haivision_library,
            "version": arguments.haivision_version,
            "revision": arguments.haivision_revision,
            "build_profile": arguments.haivision_build_profile,
        },
    }
    if arguments.direction == "robotweax-to-haivision":
        sender = providers["robotweax"]
        receiver = providers["haivision"]
        default_id = "incident-010-receiver-pause-robotweax-haivision"
    else:
        sender = providers["haivision"]
        receiver = providers["robotweax"]
        default_id = "incident-011-receiver-pause-haivision-robotweax"
    return {
        "incident_id": arguments.incident_id or default_id,
        "topology": (
            f"{sender['name']} sender directly connected over IPv4 loopback to "
            f"a paused {receiver['name']} application receiver"
        ),
        "sender": sender,
        "receiver": receiver,
    }


def recording_config(direction: dict, output_dir: Path) -> dict:
    return {
        "flow_id": direction["incident_id"],
        "capacity": LAST_SAMPLE + 1,
        "sender": {
            "output": output_dir / "sender.jsonl",
            "version": direction["sender"]["version"],
            "revision": direction["sender"]["revision"],
            "build_profile": direction["sender"]["build_profile"],
        },
        "receiver": {
            "output": output_dir / "receiver.jsonl",
            "version": direction["receiver"]["version"],
            "revision": direction["receiver"]["revision"],
            "build_profile": direction["receiver"]["build_profile"],
        },
    }


def validate_transfer(line: str, sample_index: int, messages: int) -> None:
    observation = parse_prefixed_json(line, "TRANSFERRED")
    if observation != {
        "sampleIndex": sample_index,
        "messages": messages,
    } and observation != {
        "transferId": sample_index,
        "messages": messages,
    }:
        raise RuntimeError(f"unexpected transfer observation: {observation}")


def normal_transfer(
    listener: Peer, caller: Peer, sample_index: int, messages: int
) -> None:
    command = f"TRANSFER {sample_index} {messages}"
    listener.send(command)
    caller.send(command)
    validate_transfer(caller.wait_for("TRANSFERRED"), sample_index, messages)
    validate_transfer(listener.wait_for("TRANSFERRED"), sample_index, messages)


def validate_recorded(line: str, role: str, capacity: int) -> None:
    observation = parse_prefixed_json(line, "RECORDED")
    expected = {
        "role": role,
        "recordCount": LAST_SAMPLE + 1,
        "capacity": capacity,
        "overwrittenCount": 0,
    }
    if observation != expected:
        raise RuntimeError(f"unexpected receiver-pause recording: {observation}")


def run_capture(
    direction: dict,
    pause_messages: int,
    steady_messages: int,
    pause_sample_millis: int,
    settle_millis: int,
    recording: Optional[dict],
) -> tuple:
    listener_arguments = ["--role", "listener"]
    if recording is not None:
        listener_arguments.extend(peer_recording_arguments("receiver", recording))
    listener = Peer(direction["receiver"]["peer"], listener_arguments)
    caller = None
    try:
        ready = parse_prefixed_json(listener.wait_for("READY"), "READY")
        listener_port = ready.get("port")
        if not isinstance(listener_port, int) or not 0 < listener_port <= 65_535:
            raise RuntimeError(f"invalid listener port: {ready}")
        caller_arguments = ["--role", "caller", "--port", str(listener_port)]
        if recording is not None:
            caller_arguments.extend(peer_recording_arguments("sender", recording))
        caller = Peer(direction["sender"]["peer"], caller_arguments)
        listener.wait_for("CONNECTED")
        caller.wait_for("CONNECTED")

        samples = sample_pair(
            listener,
            caller,
            0,
            direction["receiver"]["provider"],
            direction["sender"]["provider"],
        )
        for sample_index in [1, 2]:
            normal_transfer(listener, caller, sample_index, steady_messages)
            time.sleep(settle_millis / 1000.0)
            samples.extend(
                sample_pair(
                    listener,
                    caller,
                    sample_index,
                    direction["receiver"]["provider"],
                    direction["sender"]["provider"],
                )
            )

        transfer_id = FIRST_ACTIVE_SAMPLE
        caller.send(f"START_TRANSFER {transfer_id} {pause_messages} 0")
        started = parse_prefixed_json(
            caller.wait_for("TRANSFER_STARTED"), "TRANSFER_STARTED"
        )
        if started != {"transferId": transfer_id, "messages": pause_messages}:
            raise RuntimeError(f"unexpected sender transfer start: {started}")
        pause_started = time.monotonic_ns()
        pause_samples = []
        for sample_index in range(FIRST_ACTIVE_SAMPLE, LAST_ACTIVE_SAMPLE + 1):
            time.sleep(pause_sample_millis / 1000.0)
            samples.extend(
                sample_pair(
                    listener,
                    caller,
                    sample_index,
                    direction["receiver"]["provider"],
                    direction["sender"]["provider"],
                )
            )
            pause_samples.append(
                {
                    "sampleIndex": sample_index,
                    "elapsedMonotonicNano": time.monotonic_ns() - pause_started,
                }
            )

        listener.send(f"START_TRANSFER {transfer_id} {pause_messages} 0")
        receiver_started = parse_prefixed_json(
            listener.wait_for("TRANSFER_STARTED"), "TRANSFER_STARTED"
        )
        if receiver_started != {"transferId": transfer_id, "messages": pause_messages}:
            raise RuntimeError(
                f"unexpected receiver transfer start: {receiver_started}"
            )
        measured_pause_nano = time.monotonic_ns() - pause_started
        listener.send(f"WAIT_TRANSFER {transfer_id} {pause_messages}")
        caller.send(f"WAIT_TRANSFER {transfer_id} {pause_messages}")
        validate_transfer(
            caller.wait_for("TRANSFERRED", timeout_seconds=60.0),
            transfer_id,
            pause_messages,
        )
        validate_transfer(
            listener.wait_for("TRANSFERRED", timeout_seconds=60.0),
            transfer_id,
            pause_messages,
        )

        for sample_index in [7, 8]:
            normal_transfer(listener, caller, sample_index, steady_messages)
            time.sleep(settle_millis / 1000.0)
            samples.extend(
                sample_pair(
                    listener,
                    caller,
                    sample_index,
                    direction["receiver"]["provider"],
                    direction["sender"]["provider"],
                )
            )

        listener.send("DONE")
        caller.send("DONE")
        if recording is not None:
            validate_recorded(
                listener.wait_for("RECORDED"), "receiver", recording["capacity"]
            )
            validate_recorded(
                caller.wait_for("RECORDED"), "sender", recording["capacity"]
            )
        listener.finish()
        caller.finish()
        return samples, {
            "clock": "time.monotonic_ns",
            "pausedRole": "receiver",
            "transferId": transfer_id,
            "pausedTransferMessages": pause_messages,
            "steadyMessagesPerInterval": steady_messages,
            "payloadBytesPerMessage": PAYLOAD_BYTES,
            "pauseSampleIntervalMillis": pause_sample_millis,
            "settleMillis": settle_millis,
            "measuredPauseNano": measured_pause_nano,
            "samples": pause_samples,
        }
    except Exception as error:
        diagnostic = {
            "receiver": listener.describe(),
            "sender": caller.describe() if caller is not None else None,
        }
        raise RuntimeError(
            f"{error}; capture diagnostics: {json.dumps(diagnostic, sort_keys=True)}"
        ) from error
    finally:
        listener.stop()
        if caller is not None:
            caller.stop()


def pressure_threshold(baseline: float, absolute_step: float) -> float:
    return max(baseline * 2.0, baseline + absolute_step)


def pressure_evidence(samples: list, pause_messages: int, steady_messages: int) -> dict:
    by_key = {(sample["role"], sample["sampleIndex"]): sample for sample in samples}
    field_rules = {
        "receiverReceiveBufferBytes": ("receiver", "receive_buffer_bytes", 65_536),
        "senderSendBufferBytes": ("sender", "send_buffer_bytes", 65_536),
        "senderFlightSizePackets": ("sender", "flight_size_packets", 8),
    }
    baselines = {}
    thresholds = {}
    elevated = {}
    for identifier, (role, field, absolute_step) in field_rules.items():
        warm = [by_key[(role, index)]["raw"].get(field) for index in [1, 2]]
        if any(not isinstance(value, (int, float)) for value in warm):
            raise RuntimeError(f"{identifier} warm baseline is unavailable: {warm}")
        baselines[identifier] = max(warm)
        thresholds[identifier] = pressure_threshold(
            baselines[identifier], absolute_step
        )
        elevated[identifier] = [
            index
            for index in range(FIRST_ACTIVE_SAMPLE, LAST_ACTIVE_SAMPLE + 1)
            if by_key[(role, index)]["raw"][field] >= thresholds[identifier]
        ]
    receiver_elevated = elevated["receiverReceiveBufferBytes"]
    upstream = sorted(
        set(elevated["senderSendBufferBytes"])
        | set(elevated["senderFlightSizePackets"])
    )
    overlap = sorted(set(receiver_elevated) & set(upstream))
    if len(receiver_elevated) < 2 or not overlap:
        raise RuntimeError(
            f"receiver pressure evidence is incomplete: {elevated}, {thresholds}"
        )
    recovery_sample = receiver_elevated[-1] + 1
    if recovery_sample > LAST_SAMPLE:
        raise RuntimeError("receiver buffer did not recover inside the capture")
    if (
        by_key[("receiver", recovery_sample)]["raw"]["receive_buffer_bytes"]
        >= thresholds["receiverReceiveBufferBytes"]
    ):
        raise RuntimeError("receiver buffer remained elevated after application resume")

    for sample_index in range(1, LAST_SAMPLE + 1):
        for role in ["sender", "receiver"]:
            delta = by_key[(role, sample_index)]["delta"]
            for field in [
                "sender_loss_packet_delta",
                "receiver_loss_packet_delta",
                "sender_drop_packet_delta",
                "receiver_drop_packet_delta",
                "receiver_undecryptable_packet_delta",
            ]:
                value = delta.get(field)
                if value is not None and value != 0:
                    raise RuntimeError(
                        f"receiver pause has competing evidence at {role}/{sample_index}: "
                        f"{field}={value}"
                    )
    expected_messages = pause_messages + 4 * steady_messages
    sender_total = by_key[("sender", LAST_SAMPLE)]["raw"]["unique_sent_packet_total"]
    receiver_total = by_key[("receiver", LAST_SAMPLE)]["raw"][
        "unique_received_packet_total"
    ]
    if sender_total != expected_messages or receiver_total != expected_messages:
        raise RuntimeError(
            f"receiver pause delivery is incomplete: {sender_total}/{receiver_total}, "
            f"expected {expected_messages}"
        )
    return {
        "baselines": baselines,
        "thresholds": thresholds,
        "elevated": elevated,
        "overlapSamples": overlap,
        "firstDeviationSample": receiver_elevated[0],
        "lastDeviationSample": receiver_elevated[-1],
        "recoverySample": recovery_sample,
        "expectedMessages": expected_messages,
    }


def payload_digest(pause_messages: int, steady_messages: int) -> str:
    digest = hashlib.sha256()
    for sample_index, messages in [
        (1, steady_messages),
        (2, steady_messages),
        (FIRST_ACTIVE_SAMPLE, pause_messages),
        (7, steady_messages),
        (8, steady_messages),
    ]:
        payload = bytes([ord("A") + sample_index % 26]) * PAYLOAD_BYTES
        for _ in range(messages):
            digest.update(payload)
    return digest.hexdigest()


def analyze_recordings(output_dir: Path, direction: dict, evidence: dict) -> dict:
    analyzer = Path(__file__).resolve().parents[1] / "tools" / "analyze_incident.py"
    analysis_path = output_dir / "analysis.json"
    completed = subprocess.run(
        [
            sys.executable,
            str(analyzer),
            "--jsonl",
            str(output_dir / "sender.jsonl"),
            str(output_dir / "receiver.jsonl"),
            "--output",
            str(analysis_path),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    if not completed.stdout.startswith("incident analysis written:"):
        raise RuntimeError(f"unexpected analyzer result: {completed.stdout!r}")
    analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
    expected_timeline = {
        "sampleCount": LAST_SAMPLE + 1,
        "firstDeviationSample": evidence["firstDeviationSample"],
        "lastDeviationSample": evidence["lastDeviationSample"],
        "recoverySample": evidence["recoverySample"],
    }
    for field, value in expected_timeline.items():
        if analysis["timeline"].get(field) != value:
            raise RuntimeError(
                f"analysis {field} expected {value}: {analysis['timeline']}"
            )
    selected = analysis.get("selectedHypothesis", {})
    if (
        selected.get("id") != "receiver-side-pressure"
        or selected.get("assessment") != "supported"
    ):
        raise RuntimeError(f"recording did not support receiver pressure: {selected}")
    streams = {stream["role"]: stream for stream in analysis["source"]["streams"]}
    for role in ["sender", "receiver"]:
        if streams.get(role, {}).get("provider") != direction[role]["provider"]:
            raise RuntimeError(f"analysis provider mismatch for {role}: {streams}")
    return {
        "schemaVersion": "1alpha1",
        "format": "robotweax-srt-telemetry-jsonl-v1",
        "flowId": direction["incident_id"],
        "sender": {
            "file": "sender.jsonl",
            "sha256": sha256_file(output_dir / "sender.jsonl"),
            "records": LAST_SAMPLE + 1,
        },
        "receiver": {
            "file": "receiver.jsonl",
            "sha256": sha256_file(output_dir / "receiver.jsonl"),
            "records": LAST_SAMPLE + 1,
        },
        "analysis": {
            "file": "analysis.json",
            "sha256": sha256_file(analysis_path),
            "schemaVersion": "1alpha1",
        },
    }


def machine_expectations(evidence: dict) -> list:
    expectations = []
    field_names = {
        "receiverReceiveBufferBytes": ("receiver", "receive_buffer_bytes"),
        "senderSendBufferBytes": ("sender", "send_buffer_bytes"),
        "senderFlightSizePackets": ("sender", "flight_size_packets"),
    }
    for identifier, indexes in evidence["elevated"].items():
        if not indexes:
            continue
        role, field = field_names[identifier]
        expectations.append(
            {
                "id": f"{role}-{field.replace('_', '-')}-elevated",
                "role": role,
                "field": field,
                "source": "raw",
                "operator": "greater-than",
                "value": evidence["thresholds"][identifier],
                "sampleRange": {"first": indexes[0], "last": indexes[-1]},
            }
        )
    expectations.append(
        {
            "id": "receiver-buffer-recovered",
            "role": "receiver",
            "field": "receive_buffer_bytes",
            "source": "raw",
            "operator": "less-than",
            "value": evidence["thresholds"]["receiverReceiveBufferBytes"],
            "sampleRange": {
                "first": evidence["recoverySample"],
                "last": evidence["recoverySample"],
            },
        }
    )
    for role, field in [
        ("sender", "sender_loss_packet_delta"),
        ("sender", "sender_drop_packet_delta"),
        ("receiver", "receiver_loss_packet_delta"),
        ("receiver", "receiver_drop_packet_delta"),
    ]:
        expectations.append(
            {
                "id": f"{role}-{field.replace('_', '-')}-zero",
                "role": role,
                "field": field,
                "operator": "equals",
                "value": 0,
                "sampleRange": {"first": 1, "last": LAST_SAMPLE},
            }
        )
    return expectations


def manifest_bytes(
    arguments: argparse.Namespace,
    direction: dict,
    samples: bytes,
    application_pause: dict,
    evidence: dict,
    recordings: Optional[dict],
) -> bytes:
    sender = direction["sender"]
    receiver = direction["receiver"]
    manifest = {
        "schemaVersion": "1alpha1",
        "incidentId": direction["incident_id"],
        "failureClass": "receiver-application-pause",
        "topology": direction["topology"],
        "direction": "sender-to-receiver",
        "providers": [
            provider_manifest(
                "sender",
                sender["provider"],
                sender["version"],
                sender["revision"],
                sender["build_profile"],
            ),
            provider_manifest(
                "receiver",
                receiver["provider"],
                receiver["version"],
                receiver["revision"],
                receiver["build_profile"],
            ),
        ],
        "transportProfile": {
            "mode": "live",
            "api": "message",
            "addressFamily": "ipv4",
            "encryption": "none",
            "fec": "none",
            "group": "none",
        },
        "faultPlan": {
            "seed": 0,
            "description": (
                f"Start {arguments.pause_messages} sender messages, withhold all "
                f"receiver application reads during samples {FIRST_ACTIVE_SAMPLE} "
                f"through {LAST_ACTIVE_SAMPLE}, then drain and resume."
            ),
            "firstActiveSample": FIRST_ACTIVE_SAMPLE,
            "lastActiveSample": LAST_ACTIVE_SAMPLE,
        },
        "provenance": {
            "kind": "captured-provider-run",
            "capturedFromProvider": True,
            "generator": "qualification/run_receiver_pause_capture.py",
            "telemetryVersion": "0.1.0-alpha.0",
            "description": (
                "Public srt_bistats buffer and flight gauges captured from exact "
                "provider-linked peers across a controlled receiver application pause."
            ),
        },
        "dataset": {
            "format": "robotweax-srt-telemetry-csv-v1",
            "samplesFile": "samples.csv",
            "sha256": hashlib.sha256(samples).hexdigest(),
            "rows": 2 * (LAST_SAMPLE + 1),
            "roles": ["sender", "receiver"],
            "sampleIntervalNano": LOGICAL_INTERVAL_NANO,
        },
        "capture": {
            "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
            "telemetryRevision": arguments.telemetry_revision,
            "environment": {
                "platform": platform.platform(),
                "machine": platform.machine(),
                "pythonVersion": platform.python_version(),
                "transport": "ipv4-loopback-direct",
                "timingSource": "orchestrator-logical-one-second",
            },
            "artifacts": [
                artifact_manifest("sender", sender["peer"], sender["library"]),
                artifact_manifest("receiver", receiver["peer"], receiver["library"]),
            ],
            "applicationPause": application_pause,
        },
        "payloadDigest": payload_digest(
            arguments.pause_messages, arguments.steady_messages
        ),
        "expectedEvidence": [
            "receiver buffer occupancy rises from a warm zero baseline",
            "sender buffer or flight growth overlaps the receiver buffer growth",
            "receiver buffer occupancy returns to zero after application resume",
            "all application messages arrive without public loss or drop evidence",
        ],
        "machineExpectations": machine_expectations(evidence),
        "contradictions": [
            (
                "Logical sample timestamps make rate columns deterministic but do "
                "not measure wall-clock throughput."
            ),
            (
                "Public SRT gauges localize receiver-side pressure but do not expose "
                "exact application scheduling or consumption timing."
            ),
            (
                "IPv4 loopback does not qualify physical network devices, wide-area "
                "paths, encryption, FEC, groups, or other deployment profiles."
            ),
        ],
    }
    if recordings is not None:
        manifest["recordings"] = recordings
    return (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Capture a deterministic cross-provider receiver application pause"
        )
    )
    parser.add_argument(
        "--direction",
        choices=["robotweax-to-haivision", "haivision-to-robotweax"],
        default="robotweax-to-haivision",
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
    parser.add_argument("--incident-id")
    parser.add_argument("--pause-messages", type=int, default=7000)
    parser.add_argument("--steady-messages", type=int, default=100)
    parser.add_argument("--pause-sample-ms", type=int, default=100)
    parser.add_argument("--settle-ms", type=int, default=100)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--record-jsonl", action="store_true")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    for path in [
        arguments.robotweax_peer,
        arguments.robotweax_library,
        arguments.haivision_peer,
        arguments.haivision_library,
    ]:
        if not path.is_file():
            raise RuntimeError(f"required artifact does not exist: {path}")
    if arguments.pause_messages < 1000 or arguments.pause_messages > 7500:
        raise RuntimeError("pause-messages must be between 1000 and 7500")
    if arguments.steady_messages <= 0:
        raise RuntimeError("steady-messages must be positive")
    if arguments.pause_sample_ms < 50 or arguments.pause_sample_ms > 250:
        raise RuntimeError("pause-sample-ms must be between 50 and 250")
    if arguments.settle_ms <= 0:
        raise RuntimeError("settle-ms must be positive")
    if arguments.incident_id is not None and not INCIDENT_PATTERN.fullmatch(
        arguments.incident_id
    ):
        raise RuntimeError(f"invalid incident id: {arguments.incident_id}")
    retained_names = ["manifest.json", "samples.csv"]
    if arguments.record_jsonl:
        retained_names.extend(["sender.jsonl", "receiver.jsonl", "analysis.json"])
    if any((arguments.output_dir / name).exists() for name in retained_names):
        raise RuntimeError(
            f"refusing to overwrite retained capture in {arguments.output_dir}"
        )

    direction = capture_direction(arguments)
    recording = None
    if arguments.record_jsonl:
        arguments.output_dir.mkdir(parents=True, exist_ok=True)
        recording = recording_config(direction, arguments.output_dir)
    observations, application_pause = run_capture(
        direction,
        arguments.pause_messages,
        arguments.steady_messages,
        arguments.pause_sample_ms,
        arguments.settle_ms,
        recording,
    )
    evidence = pressure_evidence(
        observations, arguments.pause_messages, arguments.steady_messages
    )
    samples = samples_csv(observations)
    recordings = (
        analyze_recordings(arguments.output_dir, direction, evidence)
        if recording is not None
        else None
    )
    manifest = manifest_bytes(
        arguments,
        direction,
        samples,
        application_pause,
        evidence,
        recordings,
    )
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    (arguments.output_dir / "samples.csv").write_bytes(samples)
    (arguments.output_dir / "manifest.json").write_bytes(manifest)
    print(f"receiver-pause capture passed: {arguments.output_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"receiver-pause capture failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
