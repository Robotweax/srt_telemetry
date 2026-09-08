#!/usr/bin/env python3
"""Real peer tests for bounded post truncation and persistence refusal."""
import argparse
import json
import tempfile
from pathlib import Path

from run_constant_loss_capture import Peer, parse_prefixed_json
from test_jsonl_peer_recording import recording_arguments


def run(args, directory, block_export=False):
    paths = {role: directory / f"{role}.jsonl" for role in ["sender", "receiver"]}
    if block_export:
        for path in paths.values():
            path.write_text("sentinel-existing-capture\n")
    listener = Peer(args.peer, recording_arguments("listener", paths["receiver"], "trigger-test", args.provider, 4))
    caller = None
    try:
        port = parse_prefixed_json(listener.wait_for("READY"), "READY")["port"]
        caller = Peer(args.peer, recording_arguments("caller", paths["sender"], "trigger-test", args.provider, 4) + ["--port", str(port)])
        peers = [listener, caller]
        for peer in peers:
            peer.wait_for("CONNECTED")
            peer.send("ARM 2 1000")
            peer.wait_for("WINDOW_STATE")
        for index in range(4):
            for peer in peers:
                peer.send(f"SAMPLE {index} {1_800_000_000_000_000_000 + index * 1_000_000} {(index + 1) * 1_000_000}")
                peer.wait_for("INCIDENT_SAMPLE")
        for peer in peers:
            peer.send("TRIGGER 4000000")
            peer.wait_for("WINDOW_STATE")
            peer.send("TRIGGER 5000000")
            state = parse_prefixed_json(peer.wait_for("WINDOW_STATE"), "WINDOW_STATE")
            assert state["deadlineMonotonicNano"] == 1_004_000_000 and state["triggerCount"] == 2
            peer.send("SAMPLE 4 1800000000004000000 6000000")
            peer.wait_for("INCIDENT_SAMPLE")
            peer.send("DONE")
        for role, peer in zip(["receiver", "sender"], peers):
            state = parse_prefixed_json(peer.wait_for("WINDOW_STATE"), "WINDOW_STATE")
            assert state["finishReason"] == "connection-ended" and state["recordCount"] == 3
            assert state["preRecordCount"] == 2 and state["preOverwrittenCount"] == 2
            if block_export:
                assert peer.process.wait(timeout=10) != 0
                assert paths[role].read_text() == "sentinel-existing-capture\n"
            else:
                peer.wait_for("RECORDED")
                peer.finish()
                lines = [json.loads(line) for line in paths[role].read_text().splitlines()]
                assert len(lines) == 4
                assert [d["raw"]["sample_monotonic_nano"] for d in lines[1:]] == [3_000_000, 4_000_000, 6_000_000]
    finally:
        listener.stop()
        if caller:
            caller.stop()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--peer", type=Path, required=True)
    parser.add_argument("--provider", required=True)
    args = parser.parse_args()
    for block_export in (False, True):
        with tempfile.TemporaryDirectory(prefix="trigger-peer-") as temporary:
            run(args, Path(temporary), block_export)
    print("native trigger peer: truncated post, repeated trigger and no-clobber export passed")


if __name__ == "__main__":
    main()
