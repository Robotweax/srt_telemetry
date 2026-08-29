#!/usr/bin/env python3

import argparse
import datetime as dt
import hashlib
import heapq
import json
import platform
import selectors
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Optional

from run_constant_loss_capture import (
    FIRST_ACTIVE_SAMPLE,
    INCIDENT_PATTERN,
    LAST_ACTIVE_SAMPLE,
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

RTT_LAST_SAMPLE = 12


class DelayProxy:
    def __init__(self, target_port: int, one_way_delay_millis: int) -> None:
        self.one_way_delay_millis = one_way_delay_millis
        self.downstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for endpoint in [self.downstream, self.upstream]:
            endpoint.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
            endpoint.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
            endpoint.bind(("127.0.0.1", 0))
            endpoint.setblocking(False)
        self.listen_port = int(self.downstream.getsockname()[1])
        self.upstream.connect(("127.0.0.1", target_port))
        self.client_address = None
        self.lock = threading.Lock()
        self.stage = None
        self.pending_count = 0
        self.error = None
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    @property
    def port(self) -> int:
        return self.listen_port

    @staticmethod
    def _new_stage(sample_index: int, active: bool) -> dict:
        return {
            "sampleIndex": sample_index,
            "active": active,
            "downstreamPackets": 0,
            "reversePackets": 0,
            "delayedDownstreamPackets": 0,
            "delayedReversePackets": 0,
            "maximumQueuedPackets": 0,
        }

    def begin_stage(self, sample_index: int, active: bool) -> None:
        with self.lock:
            if self.stage is not None:
                raise RuntimeError("delay proxy stage already active")
            self.stage = self._new_stage(sample_index, active)

    def finish_stage(self) -> dict:
        with self.lock:
            if self.stage is None:
                raise RuntimeError("delay proxy stage is not active")
            result = dict(self.stage)
            self.stage = None
        self.raise_if_failed()
        return result

    def describe(self) -> dict:
        with self.lock:
            stage = dict(self.stage) if self.stage is not None else None
            pending_count = self.pending_count
        return {
            "port": self.port,
            "clientAddressKnown": self.client_address is not None,
            "oneWayDelayMillis": self.one_way_delay_millis,
            "pendingPackets": pending_count,
            "activeStage": stage,
            "error": str(self.error) if self.error is not None else None,
        }

    def raise_if_failed(self) -> None:
        if self.error is not None:
            raise RuntimeError(f"delay proxy failed: {self.error}")

    def stop(self) -> None:
        self.stopping.set()
        self.thread.join(timeout=2.0)
        self.downstream.close()
        self.upstream.close()
        if self.thread.is_alive():
            raise RuntimeError("delay proxy did not stop")
        self.raise_if_failed()

    def _record_packet(self, direction: str) -> bool:
        with self.lock:
            active = self.stage is not None and self.stage["active"]
            if self.stage is not None:
                count_field = (
                    "downstreamPackets"
                    if direction == "downstream"
                    else "reversePackets"
                )
                self.stage[count_field] += 1
                if active:
                    delayed_field = (
                        "delayedDownstreamPackets"
                        if direction == "downstream"
                        else "delayedReversePackets"
                    )
                    self.stage[delayed_field] += 1
            return active

    def _update_pending(self, pending: int) -> None:
        with self.lock:
            self.pending_count = pending
            if self.stage is not None:
                self.stage["maximumQueuedPackets"] = max(
                    self.stage["maximumQueuedPackets"], pending
                )

    def _forward(self, direction: str, packet: bytes, address) -> None:
        if direction == "downstream":
            self.upstream.send(packet)
        else:
            self.downstream.sendto(packet, address)

    def _run(self) -> None:
        selector = selectors.DefaultSelector()
        selector.register(self.downstream, selectors.EVENT_READ, "downstream")
        selector.register(self.upstream, selectors.EVENT_READ, "reverse")
        delayed = []
        serial = 0
        delay_nano = self.one_way_delay_millis * 1_000_000
        try:
            while not self.stopping.is_set():
                now = time.monotonic_ns()
                while delayed and delayed[0][0] <= now:
                    _, _, direction, packet, address = heapq.heappop(delayed)
                    self._forward(direction, packet, address)
                    self._update_pending(len(delayed))
                timeout = 0.01
                if delayed:
                    timeout = min(timeout, max(0.0, (delayed[0][0] - now) / 1e9))
                for key, _ in selector.select(timeout=timeout):
                    if key.data == "downstream":
                        packet, address = self.downstream.recvfrom(65_535)
                        if self.client_address is None:
                            self.client_address = address
                        elif address != self.client_address:
                            raise RuntimeError(
                                f"unexpected downstream client: {address}"
                            )
                        direction = "downstream"
                        destination = None
                    else:
                        packet = self.upstream.recv(65_535)
                        if self.client_address is None:
                            raise RuntimeError(
                                "upstream packet arrived before downstream client"
                            )
                        direction = "reverse"
                        destination = self.client_address
                    active = self._record_packet(direction)
                    if active:
                        serial += 1
                        heapq.heappush(
                            delayed,
                            (
                                time.monotonic_ns() + delay_nano,
                                serial,
                                direction,
                                packet,
                                destination,
                            ),
                        )
                        self._update_pending(len(delayed))
                    else:
                        self._forward(direction, packet, destination)
        except Exception as error:
            self.error = error
            self.stopping.set()
        finally:
            selector.close()


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
        default_id = "incident-008-rtt-step-robotweax-haivision"
    else:
        sender = providers["haivision"]
        receiver = providers["robotweax"]
        default_id = "incident-009-rtt-step-haivision-robotweax"
    return {
        "incident_id": arguments.incident_id or default_id,
        "topology": (
            f"{sender['name']} sender through a deterministic UDP delay proxy "
            f"to a {receiver['name']} receiver"
        ),
        "sender": sender,
        "receiver": receiver,
    }


def recording_config(direction: dict, output_dir: Path) -> dict:
    return {
        "flow_id": direction["incident_id"],
        "capacity": RTT_LAST_SAMPLE + 1,
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


def run_capture(
    direction: dict,
    messages: int,
    one_way_delay_millis: int,
    settle_millis: int,
    recording: Optional[dict],
) -> tuple:
    listener_arguments = ["--role", "listener"]
    if recording is not None:
        listener_arguments.extend(peer_recording_arguments("receiver", recording))
    listener = Peer(direction["receiver"]["peer"], listener_arguments)
    caller = None
    proxy = None
    primary_error = None
    try:
        ready = parse_prefixed_json(listener.wait_for("READY"), "READY")
        listener_port = ready.get("port")
        if not isinstance(listener_port, int) or not 0 < listener_port <= 65_535:
            raise RuntimeError(f"invalid listener port: {ready}")
        proxy = DelayProxy(listener_port, one_way_delay_millis)
        caller_arguments = ["--role", "caller", "--port", str(proxy.port)]
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
        stages = []
        for sample_index in range(1, RTT_LAST_SAMPLE + 1):
            active = FIRST_ACTIVE_SAMPLE <= sample_index <= LAST_ACTIVE_SAMPLE
            proxy.begin_stage(sample_index, active)
            transfer = f"TRANSFER {sample_index} {messages}"
            listener.send(transfer)
            caller.send(transfer)
            caller_transfer = parse_prefixed_json(
                caller.wait_for("TRANSFERRED"), "TRANSFERRED"
            )
            listener_transfer = parse_prefixed_json(
                listener.wait_for("TRANSFERRED"), "TRANSFERRED"
            )
            for observation in [caller_transfer, listener_transfer]:
                if observation != {"sampleIndex": sample_index, "messages": messages}:
                    raise RuntimeError(
                        f"unexpected transfer observation: {observation}"
                    )
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
            stages.append(proxy.finish_stage())
            if active:
                time.sleep((one_way_delay_millis * 2 + 50) / 1000.0)

        proxy.stop()
        proxy = None
        listener.send("DONE")
        caller.send("DONE")
        if recording is not None:
            validate_rtt_recorded(
                listener.wait_for("RECORDED"), "receiver", recording["capacity"]
            )
            validate_rtt_recorded(
                caller.wait_for("RECORDED"), "sender", recording["capacity"]
            )
        listener.finish()
        caller.finish()
        return samples, stages
    except Exception as error:
        primary_error = error
        diagnostic = {
            "receiver": listener.describe(),
            "sender": caller.describe() if caller is not None else None,
            "proxy": proxy.describe() if proxy is not None else None,
        }
        raise RuntimeError(
            f"{error}; capture diagnostics: {json.dumps(diagnostic, sort_keys=True)}"
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


def validate_rtt_recorded(line: str, role: str, capacity: int) -> None:
    observation = parse_prefixed_json(line, "RECORDED")
    expected = {
        "role": role,
        "recordCount": RTT_LAST_SAMPLE + 1,
        "capacity": capacity,
        "overwrittenCount": 0,
    }
    if observation != expected:
        raise RuntimeError(f"unexpected RTT recording observation: {observation}")


def rtt_payload_digest(messages: int) -> str:
    digest = hashlib.sha256()
    for sample_index in range(1, RTT_LAST_SAMPLE + 1):
        payload = bytes([ord("A") + sample_index % 26]) * PAYLOAD_BYTES
        for _ in range(messages):
            digest.update(payload)
    return digest.hexdigest()


def rtt_evidence(samples: list) -> dict:
    by_key = {(sample["role"], sample["sampleIndex"]): sample for sample in samples}
    baselines = {}
    thresholds = {}
    for role in ["sender", "receiver"]:
        warm = [by_key[(role, index)]["raw"]["rtt_seconds"] for index in [1, 2]]
        if any(not isinstance(value, (int, float)) for value in warm):
            raise RuntimeError(f"{role} warm RTT baseline is unavailable: {warm}")
        baselines[role] = min(warm)
        thresholds[role] = max(baselines[role] * 2.0, baselines[role] + 0.020)
    elevated_by_role = {
        role: [
            index
            for index in range(FIRST_ACTIVE_SAMPLE, RTT_LAST_SAMPLE + 1)
            if by_key[(role, index)]["raw"]["rtt_seconds"] >= thresholds[role]
        ]
        for role in ["sender", "receiver"]
    }
    paired_active = [
        index
        for index in range(FIRST_ACTIVE_SAMPLE, LAST_ACTIVE_SAMPLE + 1)
        if all(index in elevated_by_role[role] for role in ["sender", "receiver"])
    ]
    if len(paired_active) < 2:
        raise RuntimeError(
            f"paired RTT step has fewer than two active samples: "
            f"{paired_active}, {thresholds}"
        )
    deviating = sorted(set().union(*elevated_by_role.values()))
    last_deviation = deviating[-1]
    recovery = last_deviation + 1 if last_deviation < RTT_LAST_SAMPLE else None
    if recovery is None:
        raise RuntimeError(f"paired RTT did not recover below thresholds: {thresholds}")
    for index in range(1, RTT_LAST_SAMPLE + 1):
        for role in ["sender", "receiver"]:
            delta = by_key[(role, index)]["delta"]
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
                        f"RTT step has competing counter evidence at {role}/{index}: "
                        f"{field}={value}"
                    )
    return {
        "baselines": baselines,
        "thresholds": thresholds,
        "firstDeviationSample": deviating[0],
        "lastDeviationSample": last_deviation,
        "recoverySample": recovery,
        "elevatedByRole": elevated_by_role,
        "pairedActiveSamples": paired_active,
    }


def analyze_recordings(output_dir: Path, direction: dict, expected: dict) -> dict:
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
        "sampleCount": RTT_LAST_SAMPLE + 1,
        "firstDeviationSample": expected["firstDeviationSample"],
        "lastDeviationSample": expected["lastDeviationSample"],
        "recoverySample": expected["recoverySample"],
    }
    for field, value in expected_timeline.items():
        if analysis["timeline"].get(field) != value:
            raise RuntimeError(
                f"analysis {field} expected {value}: {analysis['timeline']}"
            )
    selected = analysis.get("selectedHypothesis", {})
    if (
        selected.get("id") != "transmission-path-rtt-step-with-recovery"
        or selected.get("assessment") != "supported"
    ):
        raise RuntimeError(f"recording did not support RTT-step hypothesis: {selected}")
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
            "records": RTT_LAST_SAMPLE + 1,
        },
        "receiver": {
            "file": "receiver.jsonl",
            "sha256": sha256_file(output_dir / "receiver.jsonl"),
            "records": RTT_LAST_SAMPLE + 1,
        },
        "analysis": {
            "file": "analysis.json",
            "sha256": sha256_file(analysis_path),
            "schemaVersion": "1alpha1",
        },
    }


def manifest_bytes(
    arguments: argparse.Namespace,
    direction: dict,
    samples: bytes,
    stages: list,
    evidence: dict,
    recordings: Optional[dict],
) -> bytes:
    sender = direction["sender"]
    receiver = direction["receiver"]
    manifest = {
        "schemaVersion": "1alpha1",
        "incidentId": direction["incident_id"],
        "failureClass": "round-trip-time-step",
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
                f"Add {arguments.one_way_delay_ms} ms one-way delay in both directions "
                f"during samples {FIRST_ACTIVE_SAMPLE} through {LAST_ACTIVE_SAMPLE}."
            ),
            "firstActiveSample": FIRST_ACTIVE_SAMPLE,
            "lastActiveSample": LAST_ACTIVE_SAMPLE,
        },
        "provenance": {
            "kind": "captured-provider-run",
            "capturedFromProvider": True,
            "generator": "qualification/run_rtt_step_capture.py",
            "telemetryVersion": "0.1.0-alpha.0",
            "description": (
                "Public srt_bistats RTT observations captured from exact "
                "provider-linked peers through a monotonic-clock delay boundary."
            ),
        },
        "dataset": {
            "format": "robotweax-srt-telemetry-csv-v1",
            "samplesFile": "samples.csv",
            "sha256": hashlib.sha256(samples).hexdigest(),
            "rows": 2 * (RTT_LAST_SAMPLE + 1),
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
                "transport": "ipv4-loopback-udp-proxy",
                "timingSource": "orchestrator-logical-one-second",
            },
            "artifacts": [
                artifact_manifest("sender", sender["peer"], sender["library"]),
                artifact_manifest("receiver", receiver["peer"], receiver["library"]),
            ],
            "delayProxy": {
                "clock": "time.monotonic_ns",
                "oneWayDelayMillis": arguments.one_way_delay_ms,
                "settleMillis": arguments.settle_ms,
                "messagesPerInterval": arguments.messages_per_interval,
                "payloadBytesPerMessage": PAYLOAD_BYTES,
                "stages": stages,
            },
        },
        "payloadDigest": rtt_payload_digest(arguments.messages_per_interval),
        "expectedEvidence": [
            (
                "paired sender and receiver RTT cross conservative "
                "warm-baseline thresholds"
            ),
            "RTT returns below both thresholds after removal of the delay",
            "loss, drop, and undecryptable deltas remain zero",
            (
                "transient retransmission without paired loss is treated as a "
                "delay response"
            ),
        ],
        "machineExpectations": [],
        "contradictions": [
            (
                "Logical sample timestamps make rate columns deterministic but do "
                "not measure wall-clock throughput."
            ),
            (
                "The SRT RTT estimator can smooth the injected boundary and delay "
                "the first observable step."
            ),
            (
                "IPv4 loopback through a user-space proxy does not qualify physical "
                "network devices or wide-area paths."
            ),
            (
                "Round-trip time does not identify one-way delay, its direction, "
                "or the exact failing hop."
            ),
            (
                "The capture qualifies only the declared unencrypted Live/Message "
                "direction and exact provider artifacts."
            ),
        ],
    }
    for role in ["sender", "receiver"]:
        elevated = evidence["elevatedByRole"][role]
        manifest["machineExpectations"].extend(
            [
                {
                    "id": f"{role}-rtt-elevated",
                    "role": role,
                    "field": "rtt_seconds",
                    "source": "raw",
                    "operator": "greater-than",
                    "value": evidence["thresholds"][role],
                    "sampleRange": {
                        "first": elevated[0],
                        "last": elevated[-1],
                    },
                },
                {
                    "id": f"{role}-rtt-recovered",
                    "role": role,
                    "field": "rtt_seconds",
                    "source": "raw",
                    "operator": "less-than",
                    "value": evidence["thresholds"][role],
                    "sampleRange": {
                        "first": evidence["recoverySample"],
                        "last": evidence["recoverySample"],
                    },
                },
            ]
        )
    if recordings is not None:
        manifest["recordings"] = recordings
    return (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture a deterministic cross-provider SRT RTT-step incident"
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
    parser.add_argument("--messages-per-interval", type=int, default=1000)
    parser.add_argument("--one-way-delay-ms", type=int, default=25)
    parser.add_argument("--settle-ms", type=int, default=500)
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
    if arguments.messages_per_interval <= 0:
        raise RuntimeError("messages-per-interval must be positive")
    if arguments.one_way_delay_ms < 20:
        raise RuntimeError("one-way-delay-ms must be at least 20")
    if arguments.settle_ms <= arguments.one_way_delay_ms * 2:
        raise RuntimeError("settle-ms must exceed the injected round-trip delay")
    if arguments.incident_id is not None and not INCIDENT_PATTERN.fullmatch(
        arguments.incident_id
    ):
        raise RuntimeError(f"invalid incident id: {arguments.incident_id}")
    retained_names = ["manifest.json", "samples.csv"]
    if arguments.record_jsonl:
        retained_names.extend(["sender.jsonl", "receiver.jsonl", "analysis.json"])
    retained_paths = [arguments.output_dir / name for name in retained_names]
    if any(path.exists() for path in retained_paths):
        raise RuntimeError(
            f"refusing to overwrite retained capture in {arguments.output_dir}"
        )

    direction = capture_direction(arguments)
    recording = None
    if arguments.record_jsonl:
        arguments.output_dir.mkdir(parents=True, exist_ok=True)
        recording = recording_config(direction, arguments.output_dir)
    observations, stages = run_capture(
        direction,
        arguments.messages_per_interval,
        arguments.one_way_delay_ms,
        arguments.settle_ms,
        recording,
    )
    evidence = rtt_evidence(observations)
    samples = samples_csv(observations)
    recordings = (
        analyze_recordings(arguments.output_dir, direction, evidence)
        if recording is not None
        else None
    )
    manifest = manifest_bytes(
        arguments, direction, samples, stages, evidence, recordings
    )
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    (arguments.output_dir / "samples.csv").write_bytes(samples)
    (arguments.output_dir / "manifest.json").write_bytes(manifest)
    print(f"RTT-step capture passed: {arguments.output_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"RTT-step capture failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
