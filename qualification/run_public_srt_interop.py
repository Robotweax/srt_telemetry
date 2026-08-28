#!/usr/bin/env python3

import argparse
import datetime as dt
import hashlib
import json
import platform
import queue
import subprocess
import sys
import threading
import time
from pathlib import Path


RUNTIME_FIELDS = [
    "sent_packet_total",
    "received_packet_total",
    "unique_sent_packet_total",
    "unique_received_packet_total",
    "sent_wire_byte_total",
    "received_wire_byte_total",
    "unique_sent_wire_byte_total",
    "unique_received_wire_byte_total",
    "rtt_seconds",
]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class Peer:
    def __init__(self, executable: Path, arguments: list[str]) -> None:
        self.command = [str(executable), *arguments]
        self.process = subprocess.Popen(
            self.command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.lines: queue.Queue[str | None] = queue.Queue()
        self.stdout_lines: list[str] = []
        self.stderr_lines: list[str] = []
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

    def wait_for(self, expected: str, timeout_seconds: float = 15.0) -> str:
        deadline = time.monotonic() + timeout_seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError(f"timeout waiting for {expected!r}: {self.describe()}")
            try:
                line = self.lines.get(timeout=remaining)
            except queue.Empty as error:
                raise RuntimeError(
                    f"timeout waiting for {expected!r}: {self.describe()}"
                ) from error
            if line is None:
                raise RuntimeError(
                    f"peer exited before {expected!r}: {self.describe()}"
                )
            if line == expected or line.startswith(f"{expected} "):
                return line

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


def parse_prefixed_json(line: str, prefix: str) -> dict:
    marker = f"{prefix} "
    if not line.startswith(marker):
        raise RuntimeError(f"invalid {prefix} line: {line!r}")
    value = json.loads(line[len(marker) :])
    if not isinstance(value, dict):
        raise RuntimeError(f"{prefix} payload is not an object")
    return value


def validate_result(result: dict, provider: str, role: str) -> None:
    if result.get("provider") != provider or result.get("role") != role:
        raise RuntimeError(
            f"unexpected peer identity: expected {provider}/{role}, got {result}"
        )
    positive_fields = [
        "intervalNano",
        "sentPacketDelta",
        "receivedPacketDelta",
        "uniqueSentPacketDelta",
        "uniqueReceivedPacketDelta",
        "sentWireByteDelta",
        "receivedWireByteDelta",
        "uniqueSentWireByteDelta",
        "uniqueReceivedWireByteDelta",
    ]
    for field in positive_fields:
        value = result.get(field)
        if not isinstance(value, int) or value <= 0:
            raise RuntimeError(f"non-positive {field} in {result}")
    rtt = result.get("rttSeconds")
    if not isinstance(rtt, (int, float)) or rtt < 0:
        raise RuntimeError(f"invalid rttSeconds in {result}")


def run_scenario(
    name: str,
    listener_provider: str,
    listener_executable: Path,
    caller_provider: str,
    caller_executable: Path,
) -> dict:
    listener = Peer(listener_executable, ["--role", "listener"])
    caller: Peer | None = None
    try:
        ready = parse_prefixed_json(listener.wait_for("READY"), "READY")
        port = ready.get("port")
        if not isinstance(port, int) or port <= 0 or port > 65_535:
            raise RuntimeError(f"invalid listener port: {ready}")

        caller = Peer(caller_executable, ["--role", "caller", "--port", str(port)])
        listener.wait_for("CONNECTED")
        caller.wait_for("CONNECTED")

        listener.send("BASELINE")
        caller.send("BASELINE")
        listener.wait_for("BASELINED")
        caller.wait_for("BASELINED")

        listener.send("TRANSFER")
        caller.send("TRANSFER")
        listener.wait_for("TRANSFERRED")
        caller.wait_for("TRANSFERRED")
        listener.send("SAMPLE")
        caller.send("SAMPLE")
        listener_result = parse_prefixed_json(listener.wait_for("RESULT"), "RESULT")
        caller_result = parse_prefixed_json(caller.wait_for("RESULT"), "RESULT")
        listener.finish()
        caller.finish()
        validate_result(listener_result, listener_provider, "listener")
        validate_result(caller_result, caller_provider, "caller")
        return {
            "name": name,
            "listenerProvider": listener_provider,
            "callerProvider": caller_provider,
            "status": "pass",
            "participants": [listener_result, caller_result],
        }
    finally:
        listener.stop()
        if caller is not None:
            caller.stop()


def provider_record(
    name: str,
    version: str,
    revision: str,
    build_profile: str,
    peer: Path,
    library: Path,
) -> dict:
    return {
        "provider": name,
        "version": version,
        "revision": revision,
        "buildProfile": build_profile,
        "artifacts": {
            "peerExecutableSha256": sha256_file(peer),
            "providerLibrarySha256": sha256_file(library),
        },
    }


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run bidirectional Robotweax SRT / Haivision SRT telemetry interop"
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
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    paths = [
        arguments.robotweax_peer,
        arguments.robotweax_library,
        arguments.haivision_peer,
        arguments.haivision_library,
    ]
    for path in paths:
        if not path.is_file():
            raise RuntimeError(f"required artifact does not exist: {path}")

    scenarios = [
        run_scenario(
            "robotweax-listener-haivision-caller",
            "robotweax-srt",
            arguments.robotweax_peer,
            "haivision-srt",
            arguments.haivision_peer,
        ),
        run_scenario(
            "haivision-listener-robotweax-caller",
            "haivision-srt",
            arguments.haivision_peer,
            "robotweax-srt",
            arguments.robotweax_peer,
        ),
    ]
    manifest = {
        "schemaVersion": "1alpha1",
        "kind": "public-srt-interop-qualification",
        "status": "pass",
        "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "telemetry": {
            "revision": arguments.telemetry_revision,
            "adapterVersion": "0.1.0-alpha.0",
            "runtimeFields": RUNTIME_FIELDS,
        },
        "environment": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "pythonVersion": platform.python_version(),
            "transport": "ipv4-loopback",
            "mode": "live-message",
            "encryption": "none",
            "fec": "none",
            "group": False,
            "coordination": "out-of-band-stdio-barrier",
        },
        "payloads": [
            {
                "direction": "caller-to-listener",
                "bytes": 1_316,
                "sha256": hashlib.sha256(b"A" * 1_316).hexdigest(),
            },
            {
                "direction": "listener-to-caller",
                "bytes": 1_316,
                "sha256": hashlib.sha256(b"B" * 1_316).hexdigest(),
            },
        ],
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
        "scenarios": scenarios,
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"interop qualification passed: {arguments.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"interop qualification failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
