#!/usr/bin/env python3
"""Exercise explicit socket settings and numbered application delivery on real peers."""
import argparse
import subprocess
from pathlib import Path

from run_constant_loss_capture import Peer, parse_prefixed_json


def run_transfer(peer_path, provider, drop_setting):
    options = ["--too-late-packet-drop", drop_setting, "--message-ttl-ms", "-1",
               "--receive-timeout-ms", "3000", "--verify-message-sequence"]
    listener = Peer(peer_path, ["--role", "listener", *options])
    caller = None
    try:
        port = parse_prefixed_json(listener.wait_for("READY"), "READY")["port"]
        caller = Peer(peer_path, ["--role", "caller", "--port", str(port), *options])
        for peer in (listener, caller):
            peer.wait_for("CONNECTED")
            profile = parse_prefixed_json(peer.wait_for("PROFILE"), "PROFILE")
            assert profile["provider"] == provider
            assert profile["tooLatePacketDrop"] is (drop_setting == "enabled")
            assert profile["configuredMessageTtlMillis"] == -1
            assert profile["receiveTimeoutMillis"] == 3000
            assert profile["sequenceVerification"] is True
        for peer in (caller, listener):
            peer.send("START_TRANSFER 42 128 0")
            started = parse_prefixed_json(peer.wait_for("TRANSFER_STARTED"), "TRANSFER_STARTED")
            assert started == {"transferId": 42, "messages": 128}
        for peer in (caller, listener):
            peer.send("WAIT_TRANSFER 42 128")
        for peer in (caller, listener):
            completed = parse_prefixed_json(peer.wait_for("TRANSFERRED"), "TRANSFERRED")
            assert completed == {"transferId": 42, "messages": 128,
                                 "completedMessages": 128}
        for peer in (listener, caller):
            peer.send("DONE")
            peer.finish()
    finally:
        listener.stop()
        if caller:
            caller.stop()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--peer", type=Path, required=True)
    parser.add_argument("--provider", required=True)
    args = parser.parse_args()
    for drop_setting in ("disabled", "enabled"):
        run_transfer(args.peer, args.provider, drop_setting)
    for invalid in (("--message-bytes", "8", "--verify-message-sequence"),
                    ("--too-late-packet-drop", "invalid")):
        result = subprocess.run([str(args.peer), "--role", "listener", *invalid],
                                capture_output=True, text=True, timeout=3)
        assert result.returncode != 0 and "usage:" in result.stderr
    print("incident peer: explicit settings and numbered delivery passed")


if __name__ == "__main__":
    main()
