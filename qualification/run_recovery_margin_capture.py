#!/usr/bin/env python3

import argparse
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


class SequenceHoldProxy:
    def __init__(self, target_port: int, held_originals: int) -> None:
        self.held_originals = held_originals
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
        self.holding = False
        self.selected_sequences = set()
        self.seen_sequences = set()
        self.held_packets = []
        self.first_held_nano = None
        self.released_nano = None
        self.error = None
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    @property
    def port(self) -> int:
        return self.listen_port

    def set_holding(self, active: bool) -> None:
        with self.lock:
            self.holding = active

    def begin_stage(self, sample_index: int, active: bool) -> None:
        with self.lock:
            if self.stage is not None:
                raise RuntimeError("sequence-hold proxy stage already active")
            queued = len(self.held_packets)
            self.stage = {
                "sampleIndex": sample_index,
                "active": active,
                "queueAtStart": queued,
                "originalDataPackets": 0,
                "repeatedDataPackets": 0,
                "heldDataPackets": 0,
                "forwardedDataPackets": 0,
                "forwardedControlPackets": 0,
                "reversePackets": 0,
                "maximumQueuedPackets": queued,
                "releasedHeldPackets": 0,
            }

    def finish_stage(self) -> dict:
        with self.lock:
            if self.stage is None:
                raise RuntimeError("sequence-hold proxy stage is not active")
            self.stage["queueAtEnd"] = len(self.held_packets)
            result = dict(self.stage)
            self.stage = None
        self.raise_if_failed()
        return result

    def release(self) -> int:
        with self.lock:
            packets = self.held_packets
            self.held_packets = []
            self.holding = False
            self.released_nano = time.monotonic_ns()
            if self.stage is not None:
                self.stage["releasedHeldPackets"] += len(packets)
        for packet in packets:
            self.upstream.send(packet)
        return len(packets)

    def measured_hold_nano(self) -> int:
        with self.lock:
            if self.first_held_nano is None or self.released_nano is None:
                raise RuntimeError("sequence-hold duration is incomplete")
            return self.released_nano - self.first_held_nano

    def describe(self) -> dict:
        with self.lock:
            return {
                "port": self.port,
                "holding": self.holding,
                "selectedSequences": len(self.selected_sequences),
                "heldPackets": len(self.held_packets),
                "activeStage": dict(self.stage) if self.stage is not None else None,
                "error": str(self.error) if self.error is not None else None,
            }

    def raise_if_failed(self) -> None:
        if self.error is not None:
            raise RuntimeError(f"sequence-hold proxy failed: {self.error}")

    def stop(self) -> None:
        self.stopping.set()
        self.thread.join(timeout=2.0)
        self.downstream.close()
        self.upstream.close()
        if self.thread.is_alive():
            raise RuntimeError("sequence-hold proxy did not stop")
        self.raise_if_failed()

    @staticmethod
    def is_data(packet: bytes) -> bool:
        return len(packet) >= 4 and (packet[0] & 0x80) == 0

    def _record(self, field: str) -> None:
        if self.stage is not None:
            self.stage[field] += 1

    def _run(self) -> None:
        selector = selectors.DefaultSelector()
        selector.register(self.downstream, selectors.EVENT_READ, "downstream")
        selector.register(self.upstream, selectors.EVENT_READ, "reverse")
        try:
            while not self.stopping.is_set():
                for key, _ in selector.select(timeout=0.01):
                    if key.data == "reverse":
                        packet = self.upstream.recv(65_535)
                        if self.client_address is None:
                            raise RuntimeError(
                                "upstream packet arrived before downstream client"
                            )
                        with self.lock:
                            self._record("reversePackets")
                        self.downstream.sendto(packet, self.client_address)
                        continue

                    packet, address = self.downstream.recvfrom(65_535)
                    if self.client_address is None:
                        self.client_address = address
                    elif address != self.client_address:
                        raise RuntimeError(f"unexpected downstream client: {address}")
                    if not self.is_data(packet):
                        with self.lock:
                            self._record("forwardedControlPackets")
                        self.upstream.send(packet)
                        continue

                    sequence = int.from_bytes(packet[:4], "big") & 0x7FFF_FFFF
                    with self.lock:
                        first = sequence not in self.seen_sequences
                        if first:
                            self.seen_sequences.add(sequence)
                        else:
                            self._record("repeatedDataPackets")
                        if self.holding and first:
                            self._record("originalDataPackets")
                            if len(self.selected_sequences) < self.held_originals:
                                self.selected_sequences.add(sequence)
                        hold = self.holding and sequence in self.selected_sequences
                        if hold:
                            if self.first_held_nano is None:
                                self.first_held_nano = time.monotonic_ns()
                            self.held_packets.append(packet)
                            self._record("heldDataPackets")
                            if self.stage is not None:
                                self.stage["maximumQueuedPackets"] = max(
                                    self.stage["maximumQueuedPackets"],
                                    len(self.held_packets),
                                )
                        else:
                            self._record("forwardedDataPackets")
                    if not hold:
                        self.upstream.send(packet)
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
        default_id = "incident-014-recovery-margin-robotweax-haivision"
    else:
        sender = providers["haivision"]
        receiver = providers["robotweax"]
        default_id = "incident-015-recovery-margin-haivision-robotweax"
    return {
        "incident_id": arguments.incident_id or default_id,
        "topology": (
            f"{sender['name']} sender through a deterministic sequence-hold "
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
        raise RuntimeError(f"unexpected recovery-margin recording: {observation}")


def run_capture(
    direction: dict,
    sender_messages: int,
    receiver_messages: int,
    held_originals: int,
    steady_messages: int,
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
        proxy = SequenceHoldProxy(listener_port, held_originals)
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
        proxy.begin_stage(FIRST_ACTIVE_SAMPLE, True)
        proxy.set_holding(True)
        listener.send(f"START_TRANSFER {transfer_id} {receiver_messages} 0")
        caller.send(f"START_TRANSFER {transfer_id} {sender_messages} 0")
        expected_starts = [
            (listener, receiver_messages),
            (caller, sender_messages),
        ]
        for peer, messages in expected_starts:
            started = parse_prefixed_json(
                peer.wait_for("TRANSFER_STARTED"), "TRANSFER_STARTED"
            )
            if started != {"transferId": transfer_id, "messages": messages}:
                raise RuntimeError(f"unexpected active transfer start: {started}")
        caller.send(f"WAIT_TRANSFER {transfer_id} {sender_messages}")
        validate_transfer(
            caller.wait_for("TRANSFERRED", timeout_seconds=30.0),
            transfer_id,
            sender_messages,
        )
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

        listener.send(f"WAIT_TRANSFER {transfer_id} {receiver_messages}")
        validate_transfer(
            listener.wait_for("TRANSFERRED", timeout_seconds=30.0),
            transfer_id,
            receiver_messages,
        )
        proxy.begin_stage(7, False)
        released_packets = proxy.release()
        time.sleep(settle_millis / 1000.0)
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
        measured_hold_nano = proxy.measured_hold_nano()
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
        return samples, stages, measured_hold_nano, released_packets
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


def recovery_margin_evidence(
    samples: list,
    sender_messages: int,
    receiver_messages: int,
    steady_messages: int,
    held_originals: int,
) -> dict:
    by_key = {(sample["role"], sample["sampleIndex"]): sample for sample in samples}
    paired_loss = [
        index
        for index in range(1, LAST_SAMPLE + 1)
        if (by_key[("sender", index)]["delta"].get("sender_loss_packet_delta") or 0)
        > 0
        and (
            by_key[("receiver", index)]["delta"].get(
                "receiver_loss_packet_delta"
            )
            or 0
        )
        > 0
    ]
    retransmission = [
        index
        for index in range(1, LAST_SAMPLE + 1)
        if (
            by_key[("sender", index)]["delta"].get(
                "retransmitted_packet_delta"
            )
            or 0
        )
        > 0
    ]
    receiver_drops = [
        index
        for index in range(1, LAST_SAMPLE + 1)
        if (
            by_key[("receiver", index)]["delta"].get(
                "receiver_drop_packet_delta"
            )
            or 0
        )
        > 0
    ]
    if not paired_loss or not retransmission or not receiver_drops:
        raise RuntimeError(
            "recovery-margin counters are incomplete: "
            f"loss={paired_loss}, retransmission={retransmission}, "
            f"receiverDrops={receiver_drops}"
        )
    if not (
        paired_loss[0] <= receiver_drops[0] <= paired_loss[-1] + 1
    ):
        raise RuntimeError("receiver drops do not follow paired loss")
    expected_sender = sender_messages + 4 * steady_messages
    expected_receiver = receiver_messages + 4 * steady_messages
    sender_total = by_key[("sender", LAST_SAMPLE)]["raw"][
        "unique_sent_packet_total"
    ]
    receiver_total = by_key[("receiver", LAST_SAMPLE)]["raw"][
        "unique_received_packet_total"
    ]
    receiver_drop_total = by_key[("receiver", LAST_SAMPLE)]["raw"][
        "receiver_drop_packet_total"
    ]
    if sender_total != expected_sender or receiver_total != expected_receiver:
        raise RuntimeError(
            f"recovery-margin delivery totals are {sender_total}/{receiver_total}, "
            f"expected {expected_sender}/{expected_receiver}"
        )
    if sender_total - receiver_total != held_originals:
        raise RuntimeError("unique delivery gap does not match held originals")
    if receiver_drop_total != held_originals:
        raise RuntimeError("receiver drops do not account for the delivery gap")
    for index in range(1, LAST_SAMPLE + 1):
        sender = by_key[("sender", index)]
        receiver = by_key[("receiver", index)]
        if (sender["delta"].get("sender_drop_packet_delta") or 0) != 0:
            raise RuntimeError(f"unexpected sender drops at sample {index}")
        if (receiver["delta"].get("receiver_undecryptable_packet_delta") or 0) != 0:
            raise RuntimeError(f"unexpected undecryptable packets at sample {index}")
        delay = receiver["raw"].get("receiver_tsbpd_delay_seconds")
        if not isinstance(delay, (int, float)) or delay <= 0:
            raise RuntimeError(f"receiver TSBPD delay unavailable at sample {index}")
    return {
        "pairedLossSamples": paired_loss,
        "retransmissionSamples": retransmission,
        "receiverDropSamples": receiver_drops,
        "senderTotal": sender_total,
        "receiverTotal": receiver_total,
        "receiverDropTotal": receiver_drop_total,
    }


def payload_digest(sender_messages: int, steady_messages: int) -> str:
    digest = hashlib.sha256()
    for sample_index, messages in [
        (1, steady_messages),
        (2, steady_messages),
        (FIRST_ACTIVE_SAMPLE, sender_messages),
        (7, steady_messages),
        (8, steady_messages),
    ]:
        payload = bytes([ord("A") + sample_index % 26]) * PAYLOAD_BYTES
        for _ in range(messages):
            digest.update(payload)
    return digest.hexdigest()


def analyze_recordings(output_dir: Path, direction: dict) -> dict:
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
    if analysis["timeline"].get("sampleCount") != LAST_SAMPLE + 1:
        raise RuntimeError(f"unexpected analysis timeline: {analysis['timeline']}")
    selected = analysis.get("selectedHypothesis", {})
    if (
        selected.get("id") != "receiver-recovery-margin-exhausted"
        or selected.get("assessment") != "supported"
    ):
        raise RuntimeError(
            f"recording did not support recovery-margin exhaustion: {selected}"
        )
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
    for identifier, role, field, samples in [
        (
            "paired-sender-loss-positive",
            "sender",
            "sender_loss_packet_delta",
            evidence["pairedLossSamples"],
        ),
        (
            "paired-receiver-loss-positive",
            "receiver",
            "receiver_loss_packet_delta",
            evidence["pairedLossSamples"],
        ),
        (
            "sender-retransmission-positive",
            "sender",
            "retransmitted_packet_delta",
            evidence["retransmissionSamples"],
        ),
        (
            "receiver-drop-positive",
            "receiver",
            "receiver_drop_packet_delta",
            evidence["receiverDropSamples"],
        ),
    ]:
        expectations.append(
            {
                "id": identifier,
                "role": role,
                "field": field,
                "operator": "greater-than",
                "value": 0,
                "sampleRange": {"first": samples[0], "last": samples[-1]},
            }
        )
    expectations.extend(
        [
            {
                "id": "receiver-tsbpd-delay-available",
                "role": "receiver",
                "field": "receiver_tsbpd_delay_seconds",
                "source": "raw",
                "operator": "greater-than",
                "value": 0,
                "sampleRange": {"first": 1, "last": LAST_SAMPLE},
            },
            {
                "id": "sender-unique-total",
                "role": "sender",
                "field": "unique_sent_packet_total",
                "source": "raw",
                "operator": "equals",
                "value": evidence["senderTotal"],
                "sampleRange": {"first": LAST_SAMPLE, "last": LAST_SAMPLE},
            },
            {
                "id": "receiver-unique-total",
                "role": "receiver",
                "field": "unique_received_packet_total",
                "source": "raw",
                "operator": "equals",
                "value": evidence["receiverTotal"],
                "sampleRange": {"first": LAST_SAMPLE, "last": LAST_SAMPLE},
            },
            {
                "id": "receiver-drop-total",
                "role": "receiver",
                "field": "receiver_drop_packet_total",
                "source": "raw",
                "operator": "equals",
                "value": evidence["receiverDropTotal"],
                "sampleRange": {"first": LAST_SAMPLE, "last": LAST_SAMPLE},
            },
            {
                "id": "sender-drop-zero",
                "role": "sender",
                "field": "sender_drop_packet_delta",
                "operator": "equals",
                "value": 0,
                "sampleRange": {"first": 1, "last": LAST_SAMPLE},
            },
            {
                "id": "receiver-undecryptable-zero",
                "role": "receiver",
                "field": "receiver_undecryptable_packet_delta",
                "operator": "equals",
                "value": 0,
                "sampleRange": {"first": 1, "last": LAST_SAMPLE},
            },
        ]
    )
    return expectations


def manifest_bytes(
    arguments: argparse.Namespace,
    direction: dict,
    samples: bytes,
    stages: list,
    evidence: dict,
    measured_hold_nano: int,
    released_packets: int,
    recordings: Optional[dict],
) -> bytes:
    sender = direction["sender"]
    receiver = direction["receiver"]
    if stages[6]["releasedHeldPackets"] != released_packets:
        raise RuntimeError("recovery stage release accounting is inconsistent")
    manifest = {
        "schemaVersion": "1alpha1",
        "incidentId": direction["incident_id"],
        "failureClass": "receiver-recovery-margin-exhaustion",
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
                f"Hold the first {arguments.held_originals} original DATA packet "
                "sequences and all observed repeats during samples 3 through 6; "
                f"send {arguments.sender_messages} messages while the receiver "
                f"expects {arguments.receiver_messages}, then release the retained "
                "packets after receiver delivery completes."
            ),
            "firstActiveSample": FIRST_ACTIVE_SAMPLE,
            "lastActiveSample": LAST_ACTIVE_SAMPLE,
        },
        "provenance": {
            "kind": "captured-provider-run",
            "capturedFromProvider": True,
            "generator": "qualification/run_recovery_margin_capture.py",
            "telemetryVersion": "0.1.0-alpha.0",
            "description": (
                "Public srt_bistats loss, retransmission, receiver-drop, delivery, "
                "and configured TSBPD fields captured from exact provider-linked "
                "peers across a controlled sequence-hold boundary."
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
            "sequenceHoldProxy": {
                "clock": "time.monotonic_ns",
                "heldOriginalDataPackets": arguments.held_originals,
                "senderMessages": arguments.sender_messages,
                "receiverMessages": arguments.receiver_messages,
                "steadyMessagesPerInterval": arguments.steady_messages,
                "payloadBytesPerMessage": PAYLOAD_BYTES,
                "sampleIntervalMillis": arguments.sample_interval_ms,
                "settleMillis": arguments.settle_ms,
                "measuredHoldNano": measured_hold_nano,
                "stages": stages,
            },
        },
        "payloadDigest": payload_digest(
            arguments.sender_messages, arguments.steady_messages
        ),
        "expectedEvidence": [
            "paired sender-learned and receiver-observed loss with retransmission",
            "receiver drops exactly account for the unique packet delivery gap",
            "a positive configured receiver TSBPD delay is publicly observable",
            "loss, retransmission, and drop deltas recover after the deviation",
            "proxy ground truth retains selected sequences beyond receiver delivery",
        ],
        "machineExpectations": machine_expectations(evidence),
        "contradictions": [
            (
                "Receiver drops plus a configured TSBPD delay support a recovery-"
                "margin diagnosis, but do not prove that a larger latency setting "
                "would prevent the incident."
            ),
            (
                "Public non-destructive statistics do not expose packet belated-"
                "arrival counters or the packet delay distribution."
            ),
            (
                "The proxy proves which sequences were retained, but public SRT "
                "statistics alone do not identify an exact failing network hop."
            ),
            (
                "Logical sample timestamps make rate columns deterministic but do "
                "not measure wall-clock throughput."
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
            "Capture deterministic cross-provider receiver recovery-margin "
            "exhaustion"
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
    parser.add_argument("--sender-messages", type=int, default=1100)
    parser.add_argument("--receiver-messages", type=int, default=1000)
    parser.add_argument("--held-originals", type=int, default=100)
    parser.add_argument("--steady-messages", type=int, default=100)
    parser.add_argument("--sample-interval-ms", type=int, default=120)
    parser.add_argument("--settle-ms", type=int, default=120)
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
    if arguments.held_originals <= 0:
        raise RuntimeError("held-originals must be positive")
    if arguments.sender_messages - arguments.receiver_messages != (
        arguments.held_originals
    ):
        raise RuntimeError(
            "sender-messages minus receiver-messages must equal held-originals"
        )
    if arguments.receiver_messages < 500 or arguments.steady_messages <= 0:
        raise RuntimeError("capture transfer arguments are invalid")
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
    observations, stages, measured_hold_nano, released_packets = run_capture(
        direction,
        arguments.sender_messages,
        arguments.receiver_messages,
        arguments.held_originals,
        arguments.steady_messages,
        arguments.sample_interval_ms,
        arguments.settle_ms,
        recording,
    )
    evidence = recovery_margin_evidence(
        observations,
        arguments.sender_messages,
        arguments.receiver_messages,
        arguments.steady_messages,
        arguments.held_originals,
    )
    samples = samples_csv(observations)
    recordings = (
        analyze_recordings(arguments.output_dir, direction)
        if recording is not None
        else None
    )
    manifest = manifest_bytes(
        arguments,
        direction,
        samples,
        stages,
        evidence,
        measured_hold_nano,
        released_packets,
        recordings,
    )
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    (arguments.output_dir / "samples.csv").write_bytes(samples)
    (arguments.output_dir / "manifest.json").write_bytes(manifest)
    print(f"recovery-margin capture passed: {arguments.output_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"recovery-margin capture failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
