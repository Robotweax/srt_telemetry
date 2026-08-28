#!/usr/bin/env python3

import argparse
import csv
import datetime as dt
import hashlib
import io
import json
import platform
import queue
import selectors
import socket
import subprocess
import sys
import threading
from pathlib import Path


FIRST_ACTIVE_SAMPLE = 3
LAST_ACTIVE_SAMPLE = 6
LAST_SAMPLE = 8
LOGICAL_INTERVAL_NANO = 1_000_000_000
PAYLOAD_BYTES = 1_316

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
    def __init__(self, target_port: int, drop_every: int) -> None:
        self.drop_every = drop_every
        self.downstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for endpoint in [self.downstream, self.upstream]:
            endpoint.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
            endpoint.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
            endpoint.bind(("127.0.0.1", 0))
            endpoint.setblocking(False)
        self.upstream.connect(("127.0.0.1", target_port))
        self.client_address = None
        self.sequences = set()
        self.lock = threading.Lock()
        self.stage = None
        self.error = None
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    @property
    def port(self) -> int:
        return int(self.downstream.getsockname()[1])

    def begin_stage(self, sample_index: int, active: bool) -> None:
        with self.lock:
            if self.stage is not None:
                raise RuntimeError("fault proxy stage already active")
            self.stage = {
                "sampleIndex": sample_index,
                "active": active,
                "originalDataPackets": 0,
                "repeatedDataPackets": 0,
                "droppedOriginalDataPackets": 0,
                "forwardedDataPackets": 0,
                "forwardedControlPackets": 0,
                "reversePackets": 0,
            }

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
                should_drop = (
                    self.stage["active"]
                    and self.stage["originalDataPackets"] % self.drop_every == 0
                )
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


def sample_pair(listener: Peer, caller: Peer, sample_index: int) -> list:
    monotonic_nano = (sample_index + 1) * LOGICAL_INTERVAL_NANO
    unix_nano = 1_800_000_000_000_000_000 + monotonic_nano
    command = f"SAMPLE {sample_index} {unix_nano} {monotonic_nano}"
    listener.send(command)
    caller.send(command)
    receiver = parse_prefixed_json(listener.wait_for("INCIDENT_SAMPLE"), "INCIDENT_SAMPLE")
    sender = parse_prefixed_json(caller.wait_for("INCIDENT_SAMPLE"), "INCIDENT_SAMPLE")
    validate_sample(receiver, "haivision-srt", "receiver", sample_index)
    validate_sample(sender, "robotweax-srt", "sender", sample_index)
    return [sender, receiver]


def run_capture(haivision_peer: Path, robotweax_peer: Path, messages: int, drop_every: int) -> tuple:
    listener = Peer(haivision_peer, ["--role", "listener"])
    caller = None
    proxy = None
    try:
        ready = parse_prefixed_json(listener.wait_for("READY"), "READY")
        listener_port = ready.get("port")
        if not isinstance(listener_port, int) or not 0 < listener_port <= 65_535:
            raise RuntimeError(f"invalid listener port: {ready}")
        proxy = FaultProxy(listener_port, drop_every)
        caller = Peer(robotweax_peer, ["--role", "caller", "--port", str(proxy.port)])
        listener.wait_for("CONNECTED")
        caller.wait_for("CONNECTED")

        samples = sample_pair(listener, caller, 0)
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
            expected_drops = messages // drop_every if active else 0
            if stage["originalDataPackets"] != messages:
                raise RuntimeError(f"proxy observed unexpected original packet count: {stage}")
            if stage["droppedOriginalDataPackets"] != expected_drops:
                raise RuntimeError(f"proxy observed unexpected drop count: {stage}")
            if active and stage["repeatedDataPackets"] < expected_drops:
                raise RuntimeError(f"proxy did not observe retransmission attempts: {stage}")
            stages.append(stage)
            samples.extend(sample_pair(listener, caller, sample_index))

        listener.send("DONE")
        caller.send("DONE")
        listener.finish()
        caller.finish()
        proxy.stop()
        proxy = None
        return samples, stages
    finally:
        listener.stop()
        if caller is not None:
            caller.stop()
        if proxy is not None:
            proxy.stop()


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


def manifest_bytes(arguments: argparse.Namespace, samples: bytes, stages: list) -> bytes:
    expected_drops = arguments.messages_per_interval // arguments.drop_every
    manifest = {
        "schemaVersion": "1alpha1",
        "incidentId": "incident-002-captured-constant-loss-robotweax-haivision",
        "failureClass": "constant-packet-loss",
        "topology": "Robotweax SRT sender through a deterministic UDP fault proxy to a Haivision SRT receiver",
        "direction": "sender-to-receiver",
        "providers": [
            provider_manifest(
                "sender",
                "robotweax-srt",
                arguments.robotweax_version,
                arguments.robotweax_revision,
                arguments.robotweax_build_profile,
            ),
            provider_manifest(
                "receiver",
                "haivision-srt",
                arguments.haivision_version,
                arguments.haivision_revision,
                arguments.haivision_build_profile,
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
            "description": f"Drop every {arguments.drop_every}th first-observed sender DATA sequence during samples {FIRST_ACTIVE_SAMPLE} through {LAST_ACTIVE_SAMPLE}; forward retransmissions.",
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
                artifact_manifest("sender", arguments.robotweax_peer, arguments.robotweax_library),
                artifact_manifest("receiver", arguments.haivision_peer, arguments.haivision_library),
            ],
            "faultProxy": {
                "classification": "SRT DATA when the first header bit is zero; first observation keyed by the 31-bit packet sequence",
                "messagesPerInterval": arguments.messages_per_interval,
                "payloadBytesPerMessage": PAYLOAD_BYTES,
                "dropEveryOriginalDataPacket": arguments.drop_every,
                "expectedDropsPerActiveInterval": expected_drops,
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
        "contradictions": [
            "Logical sample timestamps make rate columns deterministic but do not measure wall-clock throughput.",
            "IPv4 loopback through a user-space proxy does not qualify physical network devices or wide-area paths.",
            "The capture qualifies only the declared unencrypted Live/Message direction and exact provider artifacts.",
        ],
    }
    return (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture a deterministic Robotweax-to-Haivision constant-loss incident"
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
    parser.add_argument("--messages-per-interval", type=int, default=200)
    parser.add_argument("--drop-every", type=int, default=20)
    parser.add_argument("--output-dir", type=Path, required=True)
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
    if arguments.messages_per_interval <= 0 or arguments.drop_every <= 1:
        raise RuntimeError("messages-per-interval must be positive and drop-every must exceed one")
    if arguments.messages_per_interval % arguments.drop_every != 0:
        raise RuntimeError("messages-per-interval must be divisible by drop-every")
    retained_paths = [arguments.output_dir / "manifest.json", arguments.output_dir / "samples.csv"]
    if any(path.exists() for path in retained_paths):
        raise RuntimeError(f"refusing to overwrite retained capture in {arguments.output_dir}")

    samples, stages = run_capture(
        arguments.haivision_peer,
        arguments.robotweax_peer,
        arguments.messages_per_interval,
        arguments.drop_every,
    )
    require_incident_evidence(samples)
    samples_contents = samples_csv(samples)
    manifest_contents = manifest_bytes(arguments, samples_contents, stages)
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    (arguments.output_dir / "samples.csv").write_bytes(samples_contents)
    (arguments.output_dir / "manifest.json").write_bytes(manifest_contents)
    print(f"constant-loss capture passed: {arguments.output_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"constant-loss capture failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
