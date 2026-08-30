#!/usr/bin/env python3

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from run_constant_loss_capture import Peer, parse_prefixed_json, sample_pair


def recording_arguments(
    role: str, output: Path, flow_id: str, provider: str, capacity: int
) -> list:
    return [
        "--role",
        role,
        "--jsonl-output",
        str(output),
        "--flow-id",
        flow_id,
        "--provider-version",
        f"{provider}-runtime-test",
        "--provider-revision",
        "0123456789abcdef",
        "--build-profile",
        "ctest-release",
        "--recorder-capacity",
        str(capacity),
    ]


def require_recorded(line: str, role: str, count: int) -> None:
    result = parse_prefixed_json(line, "RECORDED")
    expected = {
        "role": role,
        "recordCount": count,
        "capacity": count,
        "overwrittenCount": 0,
    }
    if result != expected:
        raise RuntimeError(f"unexpected recording result: {result}")


def run(arguments: argparse.Namespace, directory: Path) -> None:
    sample_count = 3
    flow_id = f"ctest-{arguments.provider}-jsonl"
    sender_path = directory / "sender.jsonl"
    receiver_path = directory / "receiver.jsonl"
    listener = Peer(
        arguments.peer,
        recording_arguments(
            "listener", receiver_path, flow_id, arguments.provider, sample_count
        )
        + ["--bind-address", "::1"],
    )
    caller = None
    try:
        ready = parse_prefixed_json(listener.wait_for("READY"), "READY")
        port = ready.get("port")
        if not isinstance(port, int) or not 0 < port <= 65_535:
            raise RuntimeError(f"invalid listener port: {ready}")
        caller_arguments = recording_arguments(
            "caller", sender_path, flow_id, arguments.provider, sample_count
        )
        caller_arguments.extend(
            ["--remote-address", "::1", "--port", str(port)]
        )
        caller = Peer(arguments.peer, caller_arguments)
        listener.wait_for("CONNECTED")
        caller.wait_for("CONNECTED")

        sample_pair(listener, caller, 0, arguments.provider, arguments.provider)
        transfer = "TRANSFER 1 100"
        listener.send(transfer)
        caller.send(transfer)
        caller_result = parse_prefixed_json(caller.wait_for("TRANSFERRED"), "TRANSFERRED")
        listener_result = parse_prefixed_json(
            listener.wait_for("TRANSFERRED"), "TRANSFERRED"
        )
        if caller_result != {"sampleIndex": 1, "messages": 100} or listener_result != {
            "sampleIndex": 1,
            "messages": 100,
        }:
            raise RuntimeError(
                f"unexpected transfer result: {caller_result}, {listener_result}"
            )
        sample_pair(listener, caller, 1, arguments.provider, arguments.provider)
        sample_pair(listener, caller, 2, arguments.provider, arguments.provider)

        listener.send("DONE")
        caller.send("DONE")
        require_recorded(listener.wait_for("RECORDED"), "receiver", sample_count)
        require_recorded(caller.wait_for("RECORDED"), "sender", sample_count)
        listener.finish()
        caller.finish()

        completed = subprocess.run(
            [
                sys.executable,
                str(arguments.analyzer),
                "--jsonl",
                str(sender_path),
                str(receiver_path),
                "--compact",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        analysis = json.loads(completed.stdout)
        if analysis["source"]["format"] != "robotweax-srt-telemetry-jsonl-v1":
            raise RuntimeError("analyzer did not consume the real JSONL recording")
        if analysis["timeline"]["sampleCount"] != sample_count:
            raise RuntimeError(f"unexpected analysis timeline: {analysis['timeline']}")
        if analysis["selectedHypothesis"]["assessment"] != "insufficient-evidence":
            raise RuntimeError("healthy recording must not select a failure hypothesis")
        providers = {stream["provider"] for stream in analysis["source"]["streams"]}
        if providers != {arguments.provider}:
            raise RuntimeError(f"unexpected recording providers: {providers}")
    finally:
        listener.stop()
        if caller is not None:
            caller.stop()


def main() -> int:
    parser = argparse.ArgumentParser(description="Test incident-peer JSONL recording")
    parser.add_argument("--peer", type=Path, required=True)
    parser.add_argument(
        "--provider", choices=["robotweax-srt", "haivision-srt"], required=True
    )
    parser.add_argument("--analyzer", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    arguments = parser.parse_args()
    if not arguments.peer.is_file() or not arguments.analyzer.is_file():
        raise RuntimeError("peer or analyzer does not exist")
    arguments.work_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=f"{arguments.provider}-", dir=arguments.work_dir
    ) as temporary:
        run(arguments, Path(temporary))
    print(f"JSONL peer recording valid: {arguments.provider}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"JSONL peer recording test failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
