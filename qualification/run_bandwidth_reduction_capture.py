#!/usr/bin/env python3

import argparse
import collections
import datetime as dt
import hashlib
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
from run_receiver_pause_capture import normal_transfer, validate_transfer


class BandwidthProxy:
    def __init__(
        self,
        target_port: int,
        active_packets_per_second: int,
        recovery_packets_per_second: int,
    ) -> None:
        self.active_packets_per_second = active_packets_per_second
        self.recovery_packets_per_second = recovery_packets_per_second
        self.downstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for endpoint in [self.downstream, self.upstream]:
            endpoint.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
            endpoint.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 8 * 1024 * 1024)
            endpoint.bind(("127.0.0.1", 0))
            endpoint.setblocking(False)
        self.listen_port = int(self.downstream.getsockname()[1])
        self.upstream.connect(("127.0.0.1", target_port))
        self.client_address = None
        self.lock = threading.Lock()
        self.stage = None
        self.shaping = False
        self.pending_count = 0
        self.error = None
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    @property
    def port(self) -> int:
        return self.listen_port

    def set_shaping(self, active: bool) -> None:
        with self.lock:
            self.shaping = active

    def begin_stage(self, sample_index: int, active: bool) -> None:
        with self.lock:
            if self.stage is not None:
                raise RuntimeError("bandwidth proxy stage already active")
            self.stage = {
                "sampleIndex": sample_index,
                "active": active,
                "queueAtStart": self.pending_count,
                "downstreamDataPackets": 0,
                "queuedDataPackets": 0,
                "forwardedDataPackets": 0,
                "downstreamControlPackets": 0,
                "reversePackets": 0,
                "maximumQueuedPackets": self.pending_count,
            }

    def finish_stage(self) -> dict:
        with self.lock:
            if self.stage is None:
                raise RuntimeError("bandwidth proxy stage is not active")
            self.stage["queueAtEnd"] = self.pending_count
            result = dict(self.stage)
            self.stage = None
        self.raise_if_failed()
        return result

    def wait_empty(self, timeout_seconds: float = 10.0) -> None:
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            self.raise_if_failed()
            with self.lock:
                if self.pending_count == 0:
                    return
            time.sleep(0.01)
        raise RuntimeError(f"bandwidth proxy queue did not drain: {self.describe()}")

    def describe(self) -> dict:
        with self.lock:
            return {
                "port": self.port,
                "shaping": self.shaping,
                "pendingPackets": self.pending_count,
                "activeStage": dict(self.stage) if self.stage is not None else None,
                "error": str(self.error) if self.error is not None else None,
            }

    def raise_if_failed(self) -> None:
        if self.error is not None:
            raise RuntimeError(f"bandwidth proxy failed: {self.error}")

    def stop(self) -> None:
        self.stopping.set()
        self.thread.join(timeout=2.0)
        self.downstream.close()
        self.upstream.close()
        if self.thread.is_alive():
            raise RuntimeError("bandwidth proxy did not stop")
        self.raise_if_failed()

    @staticmethod
    def is_data(packet: bytes) -> bool:
        return len(packet) >= 4 and (packet[0] & 0x80) == 0

    def _record(self, field: str) -> None:
        with self.lock:
            if self.stage is not None:
                self.stage[field] += 1

    def _set_pending(self, pending: int) -> None:
        with self.lock:
            self.pending_count = pending
            if self.stage is not None:
                self.stage["maximumQueuedPackets"] = max(
                    self.stage["maximumQueuedPackets"], pending
                )

    def _rate(self) -> int:
        with self.lock:
            return (
                self.active_packets_per_second
                if self.shaping
                else self.recovery_packets_per_second
            )

    def _run(self) -> None:
        selector = selectors.DefaultSelector()
        selector.register(self.downstream, selectors.EVENT_READ, "downstream")
        selector.register(self.upstream, selectors.EVENT_READ, "reverse")
        pending = collections.deque()
        next_release = time.monotonic_ns()
        try:
            while not self.stopping.is_set():
                now = time.monotonic_ns()
                interval = max(1, 1_000_000_000 // self._rate())
                if pending and now >= next_release:
                    self.upstream.send(pending.popleft())
                    self._record("forwardedDataPackets")
                    self._set_pending(len(pending))
                    next_release = max(next_release + interval, now)
                timeout = 0.005
                if pending:
                    timeout = min(timeout, max(0.0, (next_release - now) / 1e9))
                for key, _ in selector.select(timeout=timeout):
                    if key.data == "downstream":
                        packet, address = self.downstream.recvfrom(65_535)
                        if self.client_address is None:
                            self.client_address = address
                        elif address != self.client_address:
                            raise RuntimeError(f"unexpected downstream client: {address}")
                        if not self.is_data(packet):
                            self._record("downstreamControlPackets")
                            self.upstream.send(packet)
                            continue
                        self._record("downstreamDataPackets")
                        with self.lock:
                            queue_packet = self.shaping or bool(pending)
                        if queue_packet:
                            pending.append(packet)
                            self._record("queuedDataPackets")
                            self._set_pending(len(pending))
                            if len(pending) == 1:
                                next_release = max(
                                    next_release, time.monotonic_ns()
                                )
                        else:
                            self.upstream.send(packet)
                            self._record("forwardedDataPackets")
                    else:
                        packet = self.upstream.recv(65_535)
                        if self.client_address is None:
                            raise RuntimeError(
                                "upstream packet arrived before downstream client"
                            )
                        self._record("reversePackets")
                        self.downstream.sendto(packet, self.client_address)
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
        default_id = "incident-012-bandwidth-reduction-robotweax-haivision"
    else:
        sender = providers["haivision"]
        receiver = providers["robotweax"]
        default_id = "incident-013-bandwidth-reduction-haivision-robotweax"
    return {
        "incident_id": arguments.incident_id or default_id,
        "topology": (
            f"{sender['name']} sender through a deterministic egress-capacity "
            f"proxy to a {receiver['name']} receiver"
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


def validate_recorded(line: str, role: str, capacity: int) -> None:
    observation = parse_prefixed_json(line, "RECORDED")
    expected = {
        "role": role,
        "recordCount": LAST_SAMPLE + 1,
        "capacity": capacity,
        "overwrittenCount": 0,
    }
    if observation != expected:
        raise RuntimeError(f"unexpected bandwidth recording: {observation}")


def run_capture(
    direction: dict,
    active_messages: int,
    steady_messages: int,
    active_packets_per_second: int,
    recovery_packets_per_second: int,
    sample_interval_millis: int,
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
        proxy = BandwidthProxy(
            listener_port, active_packets_per_second, recovery_packets_per_second
        )
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
        for sample_index in [1, 2]:
            proxy.begin_stage(sample_index, False)
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
            stages.append(proxy.finish_stage())

        transfer_id = FIRST_ACTIVE_SAMPLE
        proxy.set_shaping(True)
        proxy.begin_stage(FIRST_ACTIVE_SAMPLE, True)
        listener.send(f"START_TRANSFER {transfer_id} {active_messages} 0")
        caller.send(f"START_TRANSFER {transfer_id} {active_messages} 0")
        for peer in [listener, caller]:
            started = parse_prefixed_json(
                peer.wait_for("TRANSFER_STARTED"), "TRANSFER_STARTED"
            )
            if started != {"transferId": transfer_id, "messages": active_messages}:
                raise RuntimeError(f"unexpected active transfer start: {started}")
        time.sleep(sample_interval_millis / 1000.0)
        samples.extend(
            sample_pair(
                listener,
                caller,
                FIRST_ACTIVE_SAMPLE,
                direction["receiver"]["provider"],
                direction["sender"]["provider"],
            )
        )
        stages.append(proxy.finish_stage())
        for sample_index in range(FIRST_ACTIVE_SAMPLE + 1, LAST_ACTIVE_SAMPLE + 1):
            proxy.begin_stage(sample_index, True)
            time.sleep(sample_interval_millis / 1000.0)
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

        proxy.set_shaping(False)
        proxy.begin_stage(7, False)
        proxy.wait_empty()
        listener.send(f"WAIT_TRANSFER {transfer_id} {active_messages}")
        caller.send(f"WAIT_TRANSFER {transfer_id} {active_messages}")
        validate_transfer(
            caller.wait_for("TRANSFERRED", timeout_seconds=60.0),
            transfer_id,
            active_messages,
        )
        validate_transfer(
            listener.wait_for("TRANSFERRED", timeout_seconds=60.0),
            transfer_id,
            active_messages,
        )
        normal_transfer(listener, caller, 7, steady_messages)
        time.sleep(settle_millis / 1000.0)
        samples.extend(
            sample_pair(
                listener,
                caller,
                7,
                direction["receiver"]["provider"],
                direction["sender"]["provider"],
            )
        )
        stages.append(proxy.finish_stage())

        proxy.begin_stage(8, False)
        normal_transfer(listener, caller, 8, steady_messages)
        time.sleep(settle_millis / 1000.0)
        samples.extend(
            sample_pair(
                listener,
                caller,
                8,
                direction["receiver"]["provider"],
                direction["sender"]["provider"],
            )
        )
        stages.append(proxy.finish_stage())

        proxy.stop()
        proxy = None
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


def pressure_threshold(baseline: float, absolute_step: float) -> float:
    return max(baseline * 2.0, baseline + absolute_step)


def sender_pressure_evidence(
    samples: list, active_messages: int, steady_messages: int
) -> dict:
    by_key = {(sample["role"], sample["sampleIndex"]): sample for sample in samples}
    rules = {
        "senderSendBufferBytes": ("sender", "send_buffer_bytes", 65_536),
        "senderFlightSizePackets": ("sender", "flight_size_packets", 8),
        "receiverReceiveBufferBytes": (
            "receiver",
            "receive_buffer_bytes",
            1_048_576,
        ),
    }
    baselines = {}
    thresholds = {}
    for identifier, (role, field, step) in rules.items():
        warm = [by_key[(role, index)]["raw"].get(field) for index in [1, 2]]
        if any(not isinstance(value, (int, float)) for value in warm):
            raise RuntimeError(f"{identifier} warm baseline is unavailable: {warm}")
        baselines[identifier] = max(warm)
        thresholds[identifier] = pressure_threshold(baselines[identifier], step)
    sender_buffer = [
        index
        for index in range(FIRST_ACTIVE_SAMPLE, LAST_ACTIVE_SAMPLE + 1)
        if by_key[("sender", index)]["raw"]["send_buffer_bytes"]
        >= thresholds["senderSendBufferBytes"]
    ]
    sender_flight = [
        index
        for index in range(FIRST_ACTIVE_SAMPLE, LAST_ACTIVE_SAMPLE + 1)
        if by_key[("sender", index)]["raw"]["flight_size_packets"]
        >= thresholds["senderFlightSizePackets"]
    ]
    overlap = sorted(set(sender_buffer) & set(sender_flight))
    receiver_normal = [
        index
        for index in overlap
        if by_key[("receiver", index)]["raw"]["receive_buffer_bytes"]
        < thresholds["receiverReceiveBufferBytes"]
    ]
    if len(sender_buffer) < 2 or not overlap or receiver_normal != overlap:
        raise RuntimeError(
            "sender pressure evidence is incomplete: "
            f"buffer={sender_buffer}, flight={sender_flight}, "
            f"receiverNormal={receiver_normal}"
        )
    recovery_sample = sender_buffer[-1] + 1
    if recovery_sample > LAST_SAMPLE:
        raise RuntimeError("sender pressure did not recover inside the capture")
    recovery_sender = by_key[("sender", recovery_sample)]["raw"]
    recovery_receiver = by_key[("receiver", recovery_sample)]["raw"]
    if (
        recovery_sender["send_buffer_bytes"]
        >= thresholds["senderSendBufferBytes"]
        or recovery_sender["flight_size_packets"]
        >= thresholds["senderFlightSizePackets"]
        or recovery_receiver["receive_buffer_bytes"]
        >= thresholds["receiverReceiveBufferBytes"]
    ):
        raise RuntimeError("sender pressure did not recover below all thresholds")
    for sample_index in range(1, LAST_SAMPLE + 1):
        for role in ["sender", "receiver"]:
            delta = by_key[(role, sample_index)]["delta"]
            for field in [
                "sender_loss_packet_delta",
                "receiver_loss_packet_delta",
                "retransmitted_packet_delta",
                "sender_drop_packet_delta",
                "receiver_drop_packet_delta",
                "receiver_undecryptable_packet_delta",
            ]:
                value = delta.get(field)
                if value is not None and value != 0:
                    raise RuntimeError(
                        f"bandwidth capture has competing evidence at "
                        f"{role}/{sample_index}: {field}={value}"
                    )
    expected_messages = active_messages + 4 * steady_messages
    sender_total = by_key[("sender", LAST_SAMPLE)]["raw"][
        "unique_sent_packet_total"
    ]
    receiver_total = by_key[("receiver", LAST_SAMPLE)]["raw"][
        "unique_received_packet_total"
    ]
    if sender_total != expected_messages or receiver_total != expected_messages:
        raise RuntimeError(
            f"bandwidth delivery is incomplete: {sender_total}/{receiver_total}, "
            f"expected {expected_messages}"
        )
    return {
        "baselines": baselines,
        "thresholds": thresholds,
        "senderBufferSamples": sender_buffer,
        "senderFlightSamples": sender_flight,
        "overlapSamples": overlap,
        "receiverNormalSamples": receiver_normal,
        "firstDeviationSample": min(sender_buffer[0], sender_flight[0]),
        "lastDeviationSample": max(sender_buffer[-1], sender_flight[-1]),
        "recoverySample": recovery_sample,
        "expectedMessages": expected_messages,
    }


def payload_digest(active_messages: int, steady_messages: int) -> str:
    digest = hashlib.sha256()
    for sample_index, messages in [
        (1, steady_messages),
        (2, steady_messages),
        (FIRST_ACTIVE_SAMPLE, active_messages),
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
    for field, expected in expected_timeline.items():
        if analysis["timeline"].get(field) != expected:
            raise RuntimeError(
                f"analysis {field} expected {expected}: {analysis['timeline']}"
            )
    selected = analysis.get("selectedHypothesis", {})
    if (
        selected.get("id") != "sender-side-pressure"
        or selected.get("assessment") != "supported"
    ):
        raise RuntimeError(f"recording did not support sender pressure: {selected}")
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
    expectations = [
        {
            "id": "sender-send-buffer-elevated",
            "role": "sender",
            "field": "send_buffer_bytes",
            "source": "raw",
            "operator": "greater-than",
            "value": evidence["thresholds"]["senderSendBufferBytes"],
            "sampleRange": {
                "first": evidence["senderBufferSamples"][0],
                "last": evidence["senderBufferSamples"][-1],
            },
        },
        {
            "id": "sender-flight-size-elevated",
            "role": "sender",
            "field": "flight_size_packets",
            "source": "raw",
            "operator": "greater-than",
            "value": evidence["thresholds"]["senderFlightSizePackets"],
            "sampleRange": {
                "first": evidence["senderFlightSamples"][0],
                "last": evidence["senderFlightSamples"][-1],
            },
        },
        {
            "id": "receiver-buffer-below-pressure-threshold",
            "role": "receiver",
            "field": "receive_buffer_bytes",
            "source": "raw",
            "operator": "less-than",
            "value": evidence["thresholds"]["receiverReceiveBufferBytes"],
            "sampleRange": {
                "first": evidence["overlapSamples"][0],
                "last": evidence["overlapSamples"][-1],
            },
        },
        {
            "id": "sender-buffer-recovered",
            "role": "sender",
            "field": "send_buffer_bytes",
            "source": "raw",
            "operator": "less-than",
            "value": evidence["thresholds"]["senderSendBufferBytes"],
            "sampleRange": {
                "first": evidence["recoverySample"],
                "last": evidence["recoverySample"],
            },
        },
    ]
    for role, field in [
        ("sender", "sender_loss_packet_delta"),
        ("sender", "retransmitted_packet_delta"),
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
    stages: list,
    evidence: dict,
    recordings: Optional[dict],
) -> bytes:
    sender = direction["sender"]
    receiver = direction["receiver"]
    manifest = {
        "schemaVersion": "1alpha1",
        "incidentId": direction["incident_id"],
        "failureClass": "bandwidth-reduction-with-queue-growth",
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
                f"Offer {arguments.active_messages} messages without application "
                f"pacing while limiting downstream data release to "
                f"{arguments.active_packets_per_second} packets per second during "
                f"samples {FIRST_ACTIVE_SAMPLE} through {LAST_ACTIVE_SAMPLE}, "
                "then drain at the recovery rate."
            ),
            "firstActiveSample": FIRST_ACTIVE_SAMPLE,
            "lastActiveSample": LAST_ACTIVE_SAMPLE,
        },
        "provenance": {
            "kind": "captured-provider-run",
            "capturedFromProvider": True,
            "generator": "qualification/run_bandwidth_reduction_capture.py",
            "telemetryVersion": "0.1.0-alpha.0",
            "description": (
                "Public srt_bistats sender buffer and flight gauges captured from "
                "exact provider-linked peers across a controlled egress-capacity "
                "boundary."
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
                "transport": "ipv4-loopback-udp-proxy",
                "timingSource": "orchestrator-logical-one-second",
            },
            "artifacts": [
                artifact_manifest("sender", sender["peer"], sender["library"]),
                artifact_manifest("receiver", receiver["peer"], receiver["library"]),
            ],
            "bandwidthProxy": {
                "clock": "time.monotonic_ns",
                "activeDataPacketsPerSecond": arguments.active_packets_per_second,
                "recoveryDataPacketsPerSecond": (
                    arguments.recovery_packets_per_second
                ),
                "activeTransferMessages": arguments.active_messages,
                "steadyMessagesPerInterval": arguments.steady_messages,
                "payloadBytesPerMessage": PAYLOAD_BYTES,
                "senderPacingMicroseconds": 0,
                "sampleIntervalMillis": arguments.sample_interval_ms,
                "settleMillis": arguments.settle_ms,
                "stages": stages,
            },
        },
        "payloadDigest": payload_digest(
            arguments.active_messages, arguments.steady_messages
        ),
        "expectedEvidence": [
            "sender buffer occupancy and flight size rise together",
            "receiver buffer occupancy remains below its pressure threshold",
            "sender pressure recovers after the proxy queue drains",
            "all application messages arrive without loss, drop, or retransmission",
        ],
        "machineExpectations": machine_expectations(evidence),
        "contradictions": [
            (
                "The proxy proves a controlled data-packet release restriction, "
                "but public SRT statistics alone do not identify that exact cause."
            ),
            (
                "Logical sample timestamps make rate columns deterministic but do "
                "not measure wall-clock throughput."
            ),
            (
                "Packet release rate is not a claim about exact payload, UDP, or "
                "wire bit rate."
            ),
            (
                "IPv4 loopback does not qualify physical network devices, "
                "wide-area paths, encryption, FEC, groups, or other profiles."
            ),
        ],
    }
    if recordings is not None:
        manifest["recordings"] = recordings
    return (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Capture deterministic cross-provider bandwidth reduction with "
            "sender queue growth"
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
    parser.add_argument("--active-messages", type=int, default=2000)
    parser.add_argument("--steady-messages", type=int, default=100)
    parser.add_argument("--active-packets-per-second", type=int, default=1400)
    parser.add_argument("--recovery-packets-per-second", type=int, default=20_000)
    parser.add_argument("--sample-interval-ms", type=int, default=150)
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
    if arguments.active_messages < 1000:
        raise RuntimeError("active-messages must be at least 1000")
    if arguments.steady_messages <= 0:
        raise RuntimeError("steady-messages must be positive")
    if arguments.active_packets_per_second < 100:
        raise RuntimeError("active-packets-per-second must be at least 100")
    if (
        arguments.recovery_packets_per_second
        <= arguments.active_packets_per_second
    ):
        raise RuntimeError("recovery rate must exceed the active packet rate")
    if arguments.sample_interval_ms < 50 or arguments.settle_ms <= 0:
        raise RuntimeError("capture timing arguments are invalid")
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
    observations, stages = run_capture(
        direction,
        arguments.active_messages,
        arguments.steady_messages,
        arguments.active_packets_per_second,
        arguments.recovery_packets_per_second,
        arguments.sample_interval_ms,
        arguments.settle_ms,
        recording,
    )
    evidence = sender_pressure_evidence(
        observations, arguments.active_messages, arguments.steady_messages
    )
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
    print(f"bandwidth-reduction capture passed: {arguments.output_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"bandwidth-reduction capture failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
