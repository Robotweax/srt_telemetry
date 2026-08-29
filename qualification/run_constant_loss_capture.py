#!/usr/bin/env python3

import argparse
import csv
import datetime as dt
import hashlib
import io
import json
import platform
import queue
import re
import selectors
import socket
import subprocess
import sys
import threading
from pathlib import Path
from typing import Optional


FIRST_ACTIVE_SAMPLE = 3
LAST_ACTIVE_SAMPLE = 6
LAST_SAMPLE = 8
LOGICAL_INTERVAL_NANO = 1_000_000_000
PAYLOAD_BYTES = 1_316
INCIDENT_PATTERN = re.compile(r"^incident-[0-9]{3,}-[a-z0-9-]+$")

COUNTER_FIELDS = [
    ("sent_packet_total", "sent_packet_delta", 1 << 0),
    ("received_packet_total", "received_packet_delta", 1 << 1),
    ("unique_sent_packet_total", "unique_sent_packet_delta", 1 << 2),
    ("unique_received_packet_total", "unique_received_packet_delta", 1 << 3),
    ("sender_loss_packet_total", "sender_loss_packet_delta", 1 << 4),
    ("receiver_loss_packet_total", "receiver_loss_packet_delta", 1 << 5),
    ("retransmitted_packet_total", "retransmitted_packet_delta", 1 << 6),
    ("sender_drop_packet_total", "sender_drop_packet_delta", 1 << 7),
    ("receiver_drop_packet_total", "receiver_drop_packet_delta", 1 << 8),
    ("receiver_undecryptable_packet_total", "receiver_undecryptable_packet_delta", 1 << 9),
    ("sent_wire_byte_total", "sent_wire_byte_delta", 1 << 10),
    ("received_wire_byte_total", "received_wire_byte_delta", 1 << 11),
    ("unique_sent_wire_byte_total", "unique_sent_wire_byte_delta", 1 << 12),
    ("unique_received_wire_byte_total", "unique_received_wire_byte_delta", 1 << 13),
    ("retransmitted_wire_byte_total", "retransmitted_wire_byte_delta", 1 << 14),
]

RTT_FIELD = 1 << 15

RATE_FIELDS = [
    ("sent_wire_bits_per_second", 1 << 0),
    ("received_wire_bits_per_second", 1 << 1),
    ("unique_sent_wire_bits_per_second", 1 << 2),
    ("unique_received_wire_bits_per_second", 1 << 3),
]

CSV_COLUMNS = [
    "role",
    "provider",
    "sample_index",
    "observed_at_unix_nano",
    "sample_monotonic_nano",
    *[raw for raw, _, _ in COUNTER_FIELDS],
    "rtt_seconds",
    "expected_quality_flags",
    "expected_interval_nano",
    *[f"expected_{delta}" for _, delta, _ in COUNTER_FIELDS],
    *[f"expected_{rate}" for rate, _ in RATE_FIELDS],
]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_prefixed_json(line: str, prefix: str) -> dict:
    marker = f"{prefix} "
    if not line.startswith(marker):
        raise RuntimeError(f"invalid {prefix} line: {line!r}")
    value = json.loads(line[len(marker) :])
    if not isinstance(value, dict):
        raise RuntimeError(f"{prefix} payload is not an object")
    return value


class Peer:
    def __init__(self, executable: Path, arguments: list) -> None:
        self.command = [str(executable), *arguments]
        self.process = subprocess.Popen(
            self.command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.lines = queue.Queue()
        self.stdout_lines = []
        self.stderr_lines = []
        self.stdout_thread = threading.Thread(target=self._read_stdout, daemon=True)
        self.stderr_thread = threading.Thread(target=self._read_stderr, daemon=True)
        self.stdout_thread.start()
        self.stderr_thread.start()

    def _read_stdout(self) -> None:
        assert self.process.stdout is not None
        for raw_line in self.process.stdout:
            line = raw_line.rstrip("\r\n")
            self.stdout_lines.append(line)
            self.lines.put(line)
        self.lines.put(None)

    def _read_stderr(self) -> None:
        assert self.process.stderr is not None
        for raw_line in self.process.stderr:
            self.stderr_lines.append(raw_line.rstrip("\r\n"))

    def wait_for(self, expected: str, timeout_seconds: float = 30.0) -> str:
        try:
            line = self.lines.get(timeout=timeout_seconds)
        except queue.Empty as error:
            raise RuntimeError(f"timeout waiting for {expected!r}: {self.describe()}") from error
        while line is not None:
            if line == expected or line.startswith(f"{expected} "):
                return line
            try:
                line = self.lines.get(timeout=timeout_seconds)
            except queue.Empty as error:
                raise RuntimeError(
                    f"timeout waiting for {expected!r}: {self.describe()}"
                ) from error
        raise RuntimeError(f"peer exited before {expected!r}: {self.describe()}")

    def send(self, command: str) -> None:
        if self.process.stdin is None:
            raise RuntimeError(f"peer stdin unavailable: {self.describe()}")
        self.process.stdin.write(f"{command}\n")
        self.process.stdin.flush()

    def finish(self, timeout_seconds: float = 15.0) -> None:
        try:
            return_code = self.process.wait(timeout=timeout_seconds)
        except subprocess.TimeoutExpired as error:
            self.stop()
            raise RuntimeError(f"peer did not exit: {self.describe()}") from error
        self.stdout_thread.join(timeout=1.0)
        self.stderr_thread.join(timeout=1.0)
        if return_code != 0:
            raise RuntimeError(f"peer returned {return_code}: {self.describe()}")

    def stop(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2.0)

    def describe(self) -> str:
        return json.dumps(
            {
                "command": self.command,
                "returnCode": self.process.poll(),
                "stdout": self.stdout_lines,
                "stderr": self.stderr_lines,
            },
            sort_keys=True,
        )


class FaultProxy:
    def __init__(
        self,
        target_port: int,
        loss_pattern: str,
        drop_every: int,
        burst_start: int,
        burst_length: int,
    ) -> None:
        self.loss_pattern = loss_pattern
        self.drop_every = drop_every
        self.burst_start = burst_start
        self.burst_length = burst_length
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
        self.sequences = set()
        self.reverse_control_packet_types = {}
        self.lock = threading.Lock()
        self.stage = None
        self.error = None
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    @property
    def port(self) -> int:
        return self.listen_port

    def begin_stage(self, sample_index: int, active: bool) -> None:
        with self.lock:
            if self.stage is not None:
                raise RuntimeError("fault proxy stage already active")
            self.stage = self._new_stage(sample_index, active)
            self.reverse_control_packet_types = {}

    @staticmethod
    def _new_stage(sample_index: int, active: bool) -> dict:
        return {
            "sampleIndex": sample_index,
            "active": active,
            "originalDataPackets": 0,
            "repeatedDataPackets": 0,
            "droppedOriginalDataPackets": 0,
            "forwardedDataPackets": 0,
            "forwardedControlPackets": 0,
            "reversePackets": 0,
        }

    def advance_stage(self, sample_index: int, active: bool) -> dict:
        with self.lock:
            if self.stage is None:
                raise RuntimeError("fault proxy stage is not active")
            result = dict(self.stage)
            self.stage = self._new_stage(sample_index, active)
            self.reverse_control_packet_types = {}
        self.raise_if_failed()
        return result

    def finish_stage(self) -> dict:
        with self.lock:
            if self.stage is None:
                raise RuntimeError("fault proxy stage is not active")
            result = dict(self.stage)
            self.stage = None
        self.raise_if_failed()
        return result

    def raise_if_failed(self) -> None:
        if self.error is not None:
            raise RuntimeError(f"fault proxy failed: {self.error}")

    def describe(self) -> dict:
        with self.lock:
            stage = dict(self.stage) if self.stage is not None else None
            observed_sequences = len(self.sequences)
        return {
            "port": self.port,
            "clientAddressKnown": self.client_address is not None,
            "observedSequences": observed_sequences,
            "activeStage": stage,
            "reverseControlPacketTypes": dict(self.reverse_control_packet_types),
            "error": str(self.error) if self.error is not None else None,
        }

    def stop(self) -> None:
        self.stopping.set()
        self.thread.join(timeout=2.0)
        self.downstream.close()
        self.upstream.close()
        if self.thread.is_alive():
            raise RuntimeError("fault proxy did not stop")
        self.raise_if_failed()

    def _record_forward(self, packet: bytes) -> bool:
        if len(packet) < 4 or (packet[0] & 0x80) != 0:
            with self.lock:
                if self.stage is not None:
                    self.stage["forwardedControlPackets"] += 1
            return True
        sequence = int.from_bytes(packet[0:4], "big") & 0x7FFF_FFFF
        with self.lock:
            first_observation = sequence not in self.sequences
            self.sequences.add(sequence)
            if self.stage is None:
                return True
            if first_observation:
                self.stage["originalDataPackets"] += 1
                original_index = self.stage["originalDataPackets"]
                if self.loss_pattern == "constant":
                    matches_pattern = original_index % self.drop_every == 0
                else:
                    matches_pattern = (
                        self.burst_start
                        <= original_index
                        < self.burst_start + self.burst_length
                    )
                should_drop = self.stage["active"] and matches_pattern
                if should_drop:
                    self.stage["droppedOriginalDataPackets"] += 1
                    return False
            else:
                self.stage["repeatedDataPackets"] += 1
            self.stage["forwardedDataPackets"] += 1
            return True

    def _run(self) -> None:
        selector = selectors.DefaultSelector()
        selector.register(self.downstream, selectors.EVENT_READ, "downstream")
        selector.register(self.upstream, selectors.EVENT_READ, "upstream")
        try:
            while not self.stopping.is_set():
                for key, _ in selector.select(timeout=0.05):
                    if key.data == "downstream":
                        packet, address = self.downstream.recvfrom(65_535)
                        if self.client_address is None:
                            self.client_address = address
                        elif address != self.client_address:
                            raise RuntimeError(f"unexpected downstream client: {address}")
                        if self._record_forward(packet):
                            self.upstream.send(packet)
                    else:
                        packet = self.upstream.recv(65_535)
                        if self.client_address is None:
                            raise RuntimeError("upstream packet arrived before downstream client")
                        with self.lock:
                            if self.stage is not None:
                                self.stage["reversePackets"] += 1
                                if len(packet) >= 4 and (packet[0] & 0x80) != 0:
                                    packet_type = str(
                                        (int.from_bytes(packet[0:4], "big") >> 16)
                                        & 0x7FFF
                                    )
                                    self.reverse_control_packet_types[packet_type] = (
                                        self.reverse_control_packet_types.get(packet_type, 0) + 1
                                    )
                        self.downstream.sendto(packet, self.client_address)
        except Exception as error:
            self.error = error
            self.stopping.set()
        finally:
            selector.close()


def validate_sample(sample: dict, provider: str, role: str, sample_index: int) -> None:
    expected = {"provider": provider, "role": role, "sampleIndex": sample_index}
    for key, value in expected.items():
        if sample.get(key) != value:
            raise RuntimeError(f"unexpected sample identity {key}: {sample}")
    for key in [
        "availableFields",
        "observedAtUnixNano",
        "sampleMonotonicNano",
        "qualityFlags",
        "intervalNano",
        "availableDeltaFields",
        "availableRates",
    ]:
        if not isinstance(sample.get(key), int) or sample[key] < 0:
            raise RuntimeError(f"invalid sample field {key}: {sample}")
    if not isinstance(sample.get("raw"), dict) or not isinstance(sample.get("delta"), dict):
        raise RuntimeError(f"missing raw/delta sample objects: {sample}")


def sample_pair(
    listener: Peer,
    caller: Peer,
    sample_index: int,
    receiver_provider: str,
    sender_provider: str,
) -> list:
    monotonic_nano = (sample_index + 1) * LOGICAL_INTERVAL_NANO
    unix_nano = 1_800_000_000_000_000_000 + monotonic_nano
    command = f"SAMPLE {sample_index} {unix_nano} {monotonic_nano}"
    listener.send(command)
    caller.send(command)
    receiver = parse_prefixed_json(listener.wait_for("INCIDENT_SAMPLE"), "INCIDENT_SAMPLE")
    sender = parse_prefixed_json(caller.wait_for("INCIDENT_SAMPLE"), "INCIDENT_SAMPLE")
    validate_sample(receiver, receiver_provider, "receiver", sample_index)
    validate_sample(sender, sender_provider, "sender", sample_index)
    return [sender, receiver]


def peer_recording_arguments(role: str, recording: dict) -> list:
    endpoint = recording[role]
    return [
        "--jsonl-output",
        str(endpoint["output"]),
        "--flow-id",
        recording["flow_id"],
        "--provider-version",
        endpoint["version"],
        "--provider-revision",
        endpoint["revision"],
        "--build-profile",
        endpoint["build_profile"],
        "--recorder-capacity",
        str(recording["capacity"]),
    ]


def validate_recorded(line: str, role: str, capacity: int) -> None:
    observation = parse_prefixed_json(line, "RECORDED")
    expected = {
        "role": role,
        "recordCount": LAST_SAMPLE + 1,
        "capacity": capacity,
        "overwrittenCount": 0,
    }
    if observation != expected:
        raise RuntimeError(f"unexpected recording observation: {observation}")


def run_capture(
    receiver_peer: Path,
    sender_peer: Path,
    receiver_provider: str,
    sender_provider: str,
    messages: int,
    loss_pattern: str,
    drop_every: int,
    burst_start: int,
    burst_length: int,
    recording: Optional[dict] = None,
) -> tuple:
    listener_arguments = ["--role", "listener"]
    if recording is not None:
        listener_arguments.extend(peer_recording_arguments("receiver", recording))
    listener = Peer(receiver_peer, listener_arguments)
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
            loss_pattern,
            drop_every,
            burst_start,
            burst_length,
        )
        caller_arguments = ["--role", "caller", "--port", str(proxy.port)]
        if recording is not None:
            caller_arguments.extend(peer_recording_arguments("sender", recording))
        caller = Peer(sender_peer, caller_arguments)
        listener.wait_for("CONNECTED")
        caller.wait_for("CONNECTED")

        samples = sample_pair(
            listener, caller, 0, receiver_provider, sender_provider
        )
        stages = []
        for sample_index in range(1, LAST_SAMPLE + 1):
            active = FIRST_ACTIVE_SAMPLE <= sample_index <= LAST_ACTIVE_SAMPLE
            proxy.begin_stage(sample_index, active)
            transfer = f"TRANSFER {sample_index} {messages}"
            listener.send(transfer)
            caller.send(transfer)
            caller_transfer = parse_prefixed_json(caller.wait_for("TRANSFERRED"), "TRANSFERRED")
            listener_transfer = parse_prefixed_json(
                listener.wait_for("TRANSFERRED"), "TRANSFERRED"
            )
            for observation in [caller_transfer, listener_transfer]:
                if observation != {"sampleIndex": sample_index, "messages": messages}:
                    raise RuntimeError(f"unexpected transfer observation: {observation}")
            stage = proxy.finish_stage()
            if loss_pattern == "constant":
                expected_drops = messages // drop_every if active else 0
            else:
                expected_drops = burst_length if active else 0
            if stage["originalDataPackets"] != messages:
                raise RuntimeError(f"proxy observed unexpected original packet count: {stage}")
            if stage["droppedOriginalDataPackets"] != expected_drops:
                raise RuntimeError(f"proxy observed unexpected drop count: {stage}")
            if active and stage["repeatedDataPackets"] < expected_drops:
                raise RuntimeError(f"proxy did not observe retransmission attempts: {stage}")
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


def samples_csv(samples: list) -> bytes:
    output = io.StringIO(newline="")
    writer = csv.DictWriter(output, fieldnames=CSV_COLUMNS, lineterminator="\n")
    writer.writeheader()
    for sample in samples:
        row = {column: "" for column in CSV_COLUMNS}
        row.update(
            {
                "role": sample["role"],
                "provider": sample["provider"],
                "sample_index": sample["sampleIndex"],
                "observed_at_unix_nano": sample["observedAtUnixNano"],
                "sample_monotonic_nano": sample["sampleMonotonicNano"],
                "expected_quality_flags": sample["qualityFlags"],
                "expected_interval_nano": sample["intervalNano"],
            }
        )
        for raw_name, delta_name, field in COUNTER_FIELDS:
            if sample["availableFields"] & field:
                row[raw_name] = sample["raw"][raw_name]
            if sample["availableDeltaFields"] & field:
                row[f"expected_{delta_name}"] = sample["delta"][delta_name]
        if sample["availableFields"] & RTT_FIELD:
            row["rtt_seconds"] = sample["raw"]["rtt_seconds"]
        for rate_name, field in RATE_FIELDS:
            if sample["availableRates"] & field:
                row[f"expected_{rate_name}"] = sample["delta"][rate_name]
        writer.writerow(row)
    return output.getvalue().encode("utf-8")


def payload_digest(messages: int) -> str:
    digest = hashlib.sha256()
    for sample_index in range(1, LAST_SAMPLE + 1):
        payload = bytes([ord("A") + sample_index % 26]) * PAYLOAD_BYTES
        for _ in range(messages):
            digest.update(payload)
    return digest.hexdigest()


def provider_manifest(
    role: str, provider: str, version: str, revision: str, build_profile: str
) -> dict:
    return {
        "role": role,
        "provider": provider,
        "version": version,
        "revision": revision,
        "buildProfile": build_profile,
    }


def artifact_manifest(role: str, peer: Path, library: Path) -> dict:
    return {
        "role": role,
        "peerExecutableSha256": sha256_file(peer),
        "providerLibrarySha256": sha256_file(library),
    }


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
        if arguments.loss_pattern == "constant":
            incident_id = "incident-002-captured-constant-loss-robotweax-haivision"
        else:
            incident_id = "incident-004-captured-burst-loss-robotweax-haivision"
    else:
        sender = providers["haivision"]
        receiver = providers["robotweax"]
        if arguments.loss_pattern == "constant":
            incident_id = "incident-003-captured-constant-loss-haivision-robotweax"
        else:
            incident_id = "incident-005-captured-burst-loss-haivision-robotweax"
    return {
        "incident_id": arguments.incident_id or incident_id,
        "topology": (
            f"{sender['name']} sender through a deterministic UDP fault proxy "
            f"to a {receiver['name']} receiver"
        ),
        "sender": sender,
        "receiver": receiver,
    }


def loss_configuration(arguments: argparse.Namespace) -> dict:
    if arguments.loss_pattern == "constant":
        expected_drops = arguments.messages_per_interval // arguments.drop_every
        trailing_originals = arguments.messages_per_interval % arguments.drop_every
        return {
            "failure_class": "constant-packet-loss",
            "pattern": "constant-every-nth",
            "expected_drops": expected_drops,
            "trailing_originals": trailing_originals,
            "description": (
                f"Drop every {arguments.drop_every}th first-observed sender DATA "
                f"sequence during samples {FIRST_ACTIVE_SAMPLE} through "
                f"{LAST_ACTIVE_SAMPLE}; forward retransmissions."
            ),
            "proxy_fields": {
                "dropEveryOriginalDataPacket": arguments.drop_every,
            },
        }
    burst_last = arguments.burst_start + arguments.burst_length - 1
    return {
        "failure_class": "burst-packet-loss",
        "pattern": "contiguous-burst",
        "expected_drops": arguments.burst_length,
        "trailing_originals": arguments.messages_per_interval - burst_last,
        "description": (
            f"Drop first-observed sender DATA positions {arguments.burst_start} "
            f"through {burst_last} as one contiguous burst during samples "
            f"{FIRST_ACTIVE_SAMPLE} through {LAST_ACTIVE_SAMPLE}; forward "
            "retransmissions."
        ),
        "proxy_fields": {
            "burstStartOriginalDataPacket": arguments.burst_start,
            "burstLengthPackets": arguments.burst_length,
        },
    }


def require_incident_evidence(samples: list) -> None:
    by_key = {(sample["role"], sample["sampleIndex"]): sample for sample in samples}
    for sample_index in range(FIRST_ACTIVE_SAMPLE, LAST_ACTIVE_SAMPLE + 1):
        sender = by_key[("sender", sample_index)]["delta"]
        receiver = by_key[("receiver", sample_index)]["delta"]
        if sender["sender_loss_packet_delta"] <= 0 or sender["retransmitted_packet_delta"] <= 0:
            raise RuntimeError(f"sender loss evidence absent at sample {sample_index}: {sender}")
        if receiver["receiver_loss_packet_delta"] <= 0:
            raise RuntimeError(f"receiver loss evidence absent at sample {sample_index}: {receiver}")
    for sample_index in [1, 2, 7, 8]:
        sender = by_key[("sender", sample_index)]["delta"]
        receiver = by_key[("receiver", sample_index)]["delta"]
        if sender["sender_loss_packet_delta"] != 0 or sender["retransmitted_packet_delta"] != 0:
            raise RuntimeError(f"sender loss leaked outside fault window: {sender}")
        if receiver["receiver_loss_packet_delta"] != 0:
            raise RuntimeError(f"receiver loss leaked outside fault window: {receiver}")


def manifest_bytes(
    arguments: argparse.Namespace,
    samples: bytes,
    stages: list,
    recordings: Optional[dict] = None,
) -> bytes:
    loss = loss_configuration(arguments)
    direction = capture_direction(arguments)
    sender = direction["sender"]
    receiver = direction["receiver"]
    contradictions = [
        "Logical sample timestamps make rate columns deterministic but do not measure wall-clock throughput.",
        "IPv4 loopback through a user-space proxy does not qualify physical network devices or wide-area paths.",
        "The capture qualifies only the declared unencrypted Live/Message direction and exact provider artifacts.",
    ]
    trailing_originals = loss["trailing_originals"]
    if trailing_originals != 0:
        packet_label = "packet" if trailing_originals == 1 else "packets"
        contradictions.append(
            f"Each active interval sends {trailing_originals} non-dropped original "
            f"DATA {packet_label} after the final deliberate drop; flight-tail recovery "
            "without later DATA remains unqualified."
        )
    manifest = {
        "schemaVersion": "1alpha1",
        "incidentId": direction["incident_id"],
        "failureClass": loss["failure_class"],
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
            "description": loss["description"],
            "firstActiveSample": FIRST_ACTIVE_SAMPLE,
            "lastActiveSample": LAST_ACTIVE_SAMPLE,
        },
        "provenance": {
            "kind": "captured-provider-run",
            "capturedFromProvider": True,
            "generator": "qualification/run_constant_loss_capture.py",
            "telemetryVersion": "0.1.0-alpha.0",
            "description": "Public srt_bistats observations captured from exact provider-linked peers through a user-space UDP fault boundary.",
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
            "faultProxy": {
                "classification": "SRT DATA when the first header bit is zero; first observation keyed by the 31-bit packet sequence",
                "pattern": loss["pattern"],
                "messagesPerInterval": arguments.messages_per_interval,
                "payloadBytesPerMessage": PAYLOAD_BYTES,
                **loss["proxy_fields"],
                "expectedDropsPerActiveInterval": loss["expected_drops"],
                "stages": stages,
            },
        },
        "payloadDigest": payload_digest(arguments.messages_per_interval),
        "expectedEvidence": [
            "receiver-observed loss is positive only during the proxy fault window",
            "sender-learned loss and retransmission are positive during the same window",
            "the proxy forwards retransmissions for every deliberately dropped original sequence",
            "loss and retransmission deltas return to zero after fault removal",
        ],
        "machineExpectations": [
            {
                "id": "receiver-loss-active",
                "role": "receiver",
                "field": "receiver_loss_packet_delta",
                "operator": "greater-than",
                "value": 0,
                "sampleRange": {"first": FIRST_ACTIVE_SAMPLE, "last": LAST_ACTIVE_SAMPLE},
            },
            {
                "id": "sender-loss-active",
                "role": "sender",
                "field": "sender_loss_packet_delta",
                "operator": "greater-than",
                "value": 0,
                "sampleRange": {"first": FIRST_ACTIVE_SAMPLE, "last": LAST_ACTIVE_SAMPLE},
            },
            {
                "id": "retransmission-active",
                "role": "sender",
                "field": "retransmitted_packet_delta",
                "operator": "greater-than",
                "value": 0,
                "sampleRange": {"first": FIRST_ACTIVE_SAMPLE, "last": LAST_ACTIVE_SAMPLE},
            },
            {
                "id": "receiver-loss-recovered",
                "role": "receiver",
                "field": "receiver_loss_packet_delta",
                "operator": "equals",
                "value": 0,
                "sampleRange": {"first": LAST_ACTIVE_SAMPLE + 1, "last": LAST_SAMPLE},
            },
            {
                "id": "sender-loss-recovered",
                "role": "sender",
                "field": "sender_loss_packet_delta",
                "operator": "equals",
                "value": 0,
                "sampleRange": {"first": LAST_ACTIVE_SAMPLE + 1, "last": LAST_SAMPLE},
            },
        ],
        "contradictions": contradictions,
    }
    if recordings is not None:
        manifest["recordings"] = recordings
    return (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")


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
    expected_timeline = {
        "sampleCount": LAST_SAMPLE + 1,
        "firstDeviationSample": FIRST_ACTIVE_SAMPLE,
        "lastDeviationSample": LAST_ACTIVE_SAMPLE,
        "recoverySample": LAST_ACTIVE_SAMPLE + 1,
    }
    for field, expected in expected_timeline.items():
        if analysis["timeline"].get(field) != expected:
            raise RuntimeError(f"analysis {field} expected {expected}: {analysis['timeline']}")
    selected = analysis.get("selectedHypothesis", {})
    if selected.get("id") != "transmission-path-loss-with-recovery" or selected.get(
        "assessment"
    ) != "supported":
        raise RuntimeError(f"real recording did not support path-loss hypothesis: {selected}")
    streams = {stream["role"]: stream for stream in analysis["source"]["streams"]}
    for role in ["sender", "receiver"]:
        expected_provider = direction[role]["provider"]
        if streams.get(role, {}).get("provider") != expected_provider:
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


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture a deterministic cross-provider packet-loss incident"
    )
    parser.add_argument(
        "--direction",
        choices=["robotweax-to-haivision", "haivision-to-robotweax"],
        default="robotweax-to-haivision",
        help="sender-to-receiver provider direction (default: %(default)s)",
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
    parser.add_argument("--incident-id", help="optional retained incident identity override")
    parser.add_argument(
        "--loss-pattern",
        choices=["constant", "burst"],
        default="constant",
        help="deterministic loss pattern (default: %(default)s)",
    )
    parser.add_argument("--messages-per-interval", type=int, default=200)
    parser.add_argument("--drop-every", type=int, default=20)
    parser.add_argument("--burst-start", type=int, default=81)
    parser.add_argument("--burst-length", type=int, default=20)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--record-jsonl",
        action="store_true",
        help="record both real endpoint streams and run the local analyzer",
    )
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
    if arguments.incident_id is not None and not INCIDENT_PATTERN.fullmatch(
        arguments.incident_id
    ):
        raise RuntimeError(f"invalid incident id: {arguments.incident_id}")
    if arguments.loss_pattern == "constant":
        if arguments.drop_every <= 1:
            raise RuntimeError("drop-every must exceed one")
        if arguments.messages_per_interval < arguments.drop_every:
            raise RuntimeError("messages-per-interval must be at least drop-every")
    else:
        if arguments.burst_start <= 0 or arguments.burst_length <= 0:
            raise RuntimeError("burst-start and burst-length must be positive")
        burst_last = arguments.burst_start + arguments.burst_length - 1
        if burst_last > arguments.messages_per_interval:
            raise RuntimeError("burst range must fit inside messages-per-interval")
    retained_paths = [arguments.output_dir / "manifest.json", arguments.output_dir / "samples.csv"]
    if arguments.record_jsonl:
        retained_paths.extend(
            arguments.output_dir / name
            for name in ["sender.jsonl", "receiver.jsonl", "analysis.json"]
        )
    if any(path.exists() for path in retained_paths):
        raise RuntimeError(f"refusing to overwrite retained capture in {arguments.output_dir}")

    direction = capture_direction(arguments)
    recording = None
    if arguments.record_jsonl:
        arguments.output_dir.mkdir(parents=True, exist_ok=True)
        recording = recording_config(direction, arguments.output_dir)
    samples, stages = run_capture(
        direction["receiver"]["peer"],
        direction["sender"]["peer"],
        direction["receiver"]["provider"],
        direction["sender"]["provider"],
        arguments.messages_per_interval,
        arguments.loss_pattern,
        arguments.drop_every,
        arguments.burst_start,
        arguments.burst_length,
        recording,
    )
    require_incident_evidence(samples)
    samples_contents = samples_csv(samples)
    recordings = analyze_recordings(arguments.output_dir, direction) if recording else None
    manifest_contents = manifest_bytes(arguments, samples_contents, stages, recordings)
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    (arguments.output_dir / "samples.csv").write_bytes(samples_contents)
    (arguments.output_dir / "manifest.json").write_bytes(manifest_contents)
    print(f"{arguments.loss_pattern}-loss capture passed: {arguments.output_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"packet-loss capture failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
