#!/usr/bin/env python3
"""Exercise the incident peer's opaque UDP media relay and peer-idle option on real peers.

The caller relays datagrams from a local UDP input into one SRT message each; the
listener relays each SRT message to a UDP destination. The relay never inspects the
payload, tolerates input gaps, records counters, and leaves the existing transfer,
sampling and recording commands unchanged.
"""
import argparse
import json
import socket
import subprocess
import tempfile
import time
from pathlib import Path

from run_constant_loss_capture import Peer, parse_prefixed_json

IDLE_MS = 7000
RECEIVE_TIMEOUT_MS = 200
FIRST_BATCH = 200
SECOND_BATCH = 50
GAP_SECONDS = 1.5


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def payload(index: int) -> bytes:
    size = 1 + (index * 97) % 1316
    seed = index.to_bytes(4, "little")
    return (seed * (size // 4 + 1))[:size]


def receive_exactly(sink: socket.socket, count: int, deadline: float) -> list:
    received = []
    while len(received) < count:
        sink.settimeout(max(0.01, deadline - time.monotonic()))
        try:
            received.append(sink.recv(2048))
        except socket.timeout as error:
            raise RuntimeError(f"received {len(received)} of {count} relayed datagrams") from error
    return received


def run_relay(peer_path: Path, provider: str) -> None:
    output_port = free_udp_port()
    input_port = free_udp_port()
    sink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sink.bind(("127.0.0.1", output_port))
    source = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    common = ["--peer-idle-timeout-ms", str(IDLE_MS), "--receive-timeout-ms", str(RECEIVE_TIMEOUT_MS)]
    with tempfile.TemporaryDirectory() as directory:
        recording = Path(directory) / "receiver.jsonl"
        listener = Peer(peer_path, [
            "--role", "listener", *common, "--media-udp-output", f"127.0.0.1:{output_port}",
            "--jsonl-output", str(recording), "--flow-id", "media-relay-test",
            "--provider-version", "test", "--provider-revision", "test",
            "--build-profile", "test", "--recorder-capacity", "8"])
        caller = None
        try:
            port = parse_prefixed_json(listener.wait_for("READY"), "READY")["port"]
            caller = Peer(peer_path, [
                "--role", "caller", "--port", str(port), *common,
                "--media-udp-input", f"127.0.0.1:{input_port}"])
            for peer, direction in ((listener, "srt-to-udp"), (caller, "udp-to-srt")):
                peer.wait_for("CONNECTED")
                profile = parse_prefixed_json(peer.wait_for("PROFILE"), "PROFILE")
                assert profile["provider"] == provider
                assert profile["peerIdleTimeoutMillis"] == IDLE_MS, profile
                assert profile["receiveTimeoutMillis"] == RECEIVE_TIMEOUT_MS, profile
                assert profile["mediaRelay"] == direction, profile
            for peer, direction in ((listener, "srt-to-udp"), (caller, "udp-to-srt")):
                peer.send("START_MEDIA")
                started = parse_prefixed_json(peer.wait_for("MEDIA_STARTED"), "MEDIA_STARTED")
                assert started == {"direction": direction}, started
            # Sampling runs on the command thread while the relay worker moves data.
            listener.send("SAMPLE_NOW 0")
            parse_prefixed_json(listener.wait_for("INCIDENT_SAMPLE"), "INCIDENT_SAMPLE")

            sent = [payload(index) for index in range(FIRST_BATCH + SECOND_BATCH)]
            for datagram in sent[:FIRST_BATCH]:
                source.sendto(datagram, ("127.0.0.1", input_port))
                time.sleep(0.001)
            source.sendto(b"x" * 1317, ("127.0.0.1", input_port))
            first = receive_exactly(sink, FIRST_BATCH, time.monotonic() + 10)
            listener.send("SAMPLE_NOW 1")
            parse_prefixed_json(listener.wait_for("INCIDENT_SAMPLE"), "INCIDENT_SAMPLE")
            time.sleep(GAP_SECONDS)
            for datagram in sent[FIRST_BATCH:]:
                source.sendto(datagram, ("127.0.0.1", input_port))
                time.sleep(0.001)
            second = receive_exactly(sink, SECOND_BATCH, time.monotonic() + 10)
            assert first + second == sent, "relayed payloads differ from input or order"

            stopped = {}
            for name, peer in (("caller", caller), ("listener", listener)):
                peer.send("STOP_MEDIA")
                stopped[name] = parse_prefixed_json(peer.wait_for("MEDIA_STOPPED"), "MEDIA_STOPPED")
            total_bytes = sum(len(datagram) for datagram in sent)
            for name in ("caller", "listener"):
                counters = stopped[name]
                assert counters["messages"] == len(sent), counters
                assert counters["bytes"] == total_bytes, counters
                assert counters["connectionBroken"] is False, counters
                assert counters["sendFailures"] == 0 and counters["outputFailures"] == 0, counters
                assert counters["longestGapNano"] >= int(GAP_SECONDS * 0.8 * 1e9), counters
                assert counters["lastMessageMonotonicNano"] > counters["firstMessageMonotonicNano"]
            assert stopped["caller"]["oversizeDatagrams"] == 1, stopped["caller"]
            assert stopped["listener"]["oversizeDatagrams"] == 0, stopped["listener"]
            # The listener's 200 ms receive timeout repeats through the input gap.
            assert stopped["listener"]["receiveTimeouts"] >= 3, stopped["listener"]

            listener.send("SAMPLE_NOW 2")
            sample = parse_prefixed_json(listener.wait_for("INCIDENT_SAMPLE"), "INCIDENT_SAMPLE")
            assert sample["raw"]["unique_received_packet_total"] >= len(sent), sample
            for peer in (listener, caller):
                peer.send("DONE")
            recorded = parse_prefixed_json(listener.wait_for("RECORDED"), "RECORDED")
            assert recorded["recordCount"] == 3, recorded
            for peer in (listener, caller):
                peer.finish()
            header = json.loads(recording.read_text(encoding="utf-8").splitlines()[0])
            assert header["schemaVersion"] == "1alpha1", header
        finally:
            listener.stop()
            if caller:
                caller.stop()
            sink.close()
            source.close()


def run_defaults_and_misuse(peer_path: Path) -> None:
    listener = Peer(peer_path, ["--role", "listener"])
    caller = None
    try:
        port = parse_prefixed_json(listener.wait_for("READY"), "READY")["port"]
        caller = Peer(peer_path, ["--role", "caller", "--port", str(port)])
        listener.wait_for("CONNECTED")
        profile = parse_prefixed_json(listener.wait_for("PROFILE"), "PROFILE")
        assert profile["mediaRelay"] == "none", profile
        assert profile["peerIdleTimeoutMillis"] == 5000, profile
        caller.wait_for("CONNECTED")
        caller.wait_for("PROFILE")
        # START_MEDIA without a configured endpoint fails closed.
        caller.send("START_MEDIA")
        caller.process.wait(timeout=5)
        assert caller.process.returncode != 0
        listener.send("DONE")
        listener.finish()
    finally:
        listener.stop()
        if caller:
            caller.stop()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--peer", type=Path, required=True)
    parser.add_argument("--provider", required=True)
    args = parser.parse_args()
    run_relay(args.peer, args.provider)
    run_defaults_and_misuse(args.peer)
    for invalid in (
        ("--role", "listener", "--media-udp-input", "127.0.0.1:5000"),
        ("--role", "caller", "--port", "9", "--media-udp-output", "127.0.0.1:5000"),
        ("--role", "listener", "--media-udp-output", "127.0.0.1"),
        ("--role", "listener", "--media-udp-output", "localhost:5000"),
        ("--role", "listener", "--media-udp-output", "[::1:5000"),
        ("--role", "listener", "--peer-idle-timeout-ms", "999"),
        ("--role", "listener", "--media-udp-output", "127.0.0.1:1",
         "--media-udp-output", "127.0.0.1:2"),
    ):
        result = subprocess.run([str(args.peer), *invalid], capture_output=True, text=True, timeout=3)
        assert result.returncode != 0 and "usage:" in result.stderr, invalid
    print("incident peer: media relay, peer idle option and misuse checks passed")


if __name__ == "__main__":
    main()
