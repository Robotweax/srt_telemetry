#!/usr/bin/env python3
"""Exercise the incident peer's connection-group mode on real peers.

A caller opens a Broadcast or Backup group with two members to one group-accepting
listener. The opaque media relay runs over the group handle, each member is sampled
through its own statistics session, and each member is recorded to its own file in the
unchanged single-socket schema.
"""
import argparse
import json
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from run_constant_loss_capture import Peer, parse_prefixed_json

RECEIVE_TIMEOUT_MS = 200
DATAGRAMS = 200
NO_GROUPS = ("created no connection group", "group admission")
BACKUP_STABLE_MS = 1000


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def payload(index: int) -> bytes:
    size = 1 + (index * 97) % 1316
    seed = index.to_bytes(4, "little")
    return (seed * (size // 4 + 1))[:size]


def lacks_groups(*peers) -> bool:
    """True when a peer ended because its SRT library has no connection groups."""
    for peer in peers:
        if peer is None:
            continue
        try:
            peer.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            continue
        peer.stderr_thread.join(timeout=2)
        if any(marker in line for line in peer.stderr_lines for marker in NO_GROUPS):
            return True
    return False


def sample(peer: Peer, index: int, members: int) -> tuple:
    """One SAMPLE_NOW: the group line first, then one sample per sampled member."""
    peer.send(f"SAMPLE_NOW {index}")
    group = parse_prefixed_json(peer.wait_for("GROUP_SAMPLE"), "GROUP_SAMPLE")
    assert group["sampleIndex"] == index and len(group["members"]) == members, group
    samples = {}
    for member in group["members"]:
        if member["sampled"]:
            line = parse_prefixed_json(peer.wait_for("INCIDENT_SAMPLE"), "INCIDENT_SAMPLE")
            assert line["member"] == member["index"] and line["sampleIndex"] == index, line
            samples[member["index"]] = line
    return group, samples


def run_group(peer_path: Path, provider: str, group_type: str) -> bool:
    output_port = free_udp_port()
    input_port = free_udp_port()
    sink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sink.bind(("127.0.0.1", output_port))
    source = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    common = ["--group", group_type, "--receive-timeout-ms", str(RECEIVE_TIMEOUT_MS)]
    with tempfile.TemporaryDirectory() as directory:
        recordings = {"receiver": Path(directory) / "receiver.jsonl",
                      "sender": Path(directory) / "sender.jsonl"}

        def recording(role: str) -> list:
            return ["--jsonl-output", str(recordings[role]), "--flow-id", "group-test",
                    "--provider-version", "test", "--provider-revision", "test",
                    "--build-profile", "test", "--recorder-capacity", "8"]

        listener = Peer(peer_path, [
            "--role", "listener", *common, "--group-members", "2",
            "--media-udp-output", f"127.0.0.1:{output_port}", *recording("receiver")])
        caller = None
        try:
            try:
                port = parse_prefixed_json(listener.wait_for("READY"), "READY")["port"]
            except RuntimeError:
                if lacks_groups(listener):
                    return False
                raise
            # A long stability timeout keeps a scheduling stall on a loaded
            # host from activating the Backup standby.
            stable = (["--group-min-stable-timeout-ms", str(BACKUP_STABLE_MS)]
                      if group_type == "backup" else [])
            caller = Peer(peer_path, [
                "--role", "caller", "--port", str(port), *common, *stable,
                "--member", "127.0.0.1,127.0.0.1,20", "--member", "127.0.0.1,127.0.0.1,10",
                "--media-udp-input", f"127.0.0.1:{input_port}", *recording("sender")])
            try:
                described = {name: parse_prefixed_json(peer.wait_for("GROUP"), "GROUP")
                             for name, peer in (("caller", caller), ("listener", listener))}
            except RuntimeError:
                if lacks_groups(caller, listener):
                    return False
                raise
            for name, peer in (("caller", caller), ("listener", listener)):
                peer.wait_for("CONNECTED")
                profile = parse_prefixed_json(peer.wait_for("PROFILE"), "PROFILE")
                assert profile["provider"] == provider, profile
                assert profile["group"] == group_type and profile["groupMembers"] == 2, profile
                group = described[name]
                assert group["type"] == group_type, group
                assert [member["index"] for member in group["members"]] == [0, 1], group
                assert len({member["socket"] for member in group["members"]}) == 2, group
            # The caller keeps the order of its --member options.
            assert [member["weight"] for member in described["caller"]["members"]] == [20, 10]
            assert all(member["peerPort"] == port for member in described["caller"]["members"])
            # Each side sees the other's ports: the listener's peers are the caller's locals.
            assert ({member["peerPort"] for member in described["listener"]["members"]}
                    == {member["localPort"] for member in described["caller"]["members"]})
            listener_peers = [(member["peerAddress"], member["peerPort"])
                              for member in described["listener"]["members"]]
            assert listener_peers == sorted(listener_peers), described["listener"]

            for peer, direction in ((listener, "srt-to-udp"), (caller, "udp-to-srt")):
                peer.send("START_MEDIA")
                started = parse_prefixed_json(peer.wait_for("MEDIA_STARTED"), "MEDIA_STARTED")
                assert started == {"direction": direction}, started
            sample(caller, 0, 2)
            sample(listener, 0, 2)

            sent = [payload(index) for index in range(DATAGRAMS)]
            received = []
            # Drain while sending: the sink's default receive buffer holds fewer
            # than DATAGRAMS full-size datagrams.
            sink.settimeout(0.002)
            for datagram in sent:
                source.sendto(datagram, ("127.0.0.1", input_port))
                try:
                    received.append(sink.recv(2048))
                except socket.timeout:
                    pass
            deadline = time.monotonic() + 10
            while len(received) < DATAGRAMS:
                sink.settimeout(max(0.01, deadline - time.monotonic()))
                received.append(sink.recv(2048))
            # A group delivers each message once, whatever number of members carried it.
            assert received == sent, "relayed payloads differ from input or order"
            sink.settimeout(0.5)
            try:
                extra = sink.recv(2048)
                raise AssertionError(f"group delivered a duplicate of {len(extra)} bytes")
            except socket.timeout:
                pass

            group, samples = sample(caller, 1, 2)
            states = sorted(member["state"] for member in group["members"])
            assert set(samples) == {0, 1}, group
            assert group["groupCounters"]["uniqueSentPacketTotal"] >= DATAGRAMS, group
            carried = [samples[index]["raw"]["unique_sent_packet_total"] for index in (0, 1)]
            if group_type == "broadcast":
                assert states == ["running", "running"], group
                assert all(count >= DATAGRAMS for count in carried), carried
            else:
                assert states == ["idle", "running"], group
                # The higher weight is the active member; the standby carries no media.
                assert group["members"][0]["state"] == "running", group
                assert carried[0] >= DATAGRAMS and carried[1] == 0, carried
            group, samples = sample(listener, 1, 2)
            assert group["groupCounters"]["uniqueReceivedPacketTotal"] >= DATAGRAMS, group
            assert set(samples) == {0, 1}, group

            for peer in (caller, listener):
                peer.send("STOP_MEDIA")
                stopped = parse_prefixed_json(peer.wait_for("MEDIA_STOPPED"), "MEDIA_STOPPED")
                assert stopped["messages"] == DATAGRAMS, stopped
                assert stopped["connectionBroken"] is False, stopped
            for role, peer in (("sender", caller), ("receiver", listener)):
                peer.send("DONE")
                for member in (0, 1):
                    recorded = parse_prefixed_json(peer.wait_for("RECORDED"), "RECORDED")
                    assert recorded["member"] == member and recorded["recordCount"] == 2, recorded
                peer.finish()
                assert not recordings[role].exists(), "group mode writes one file per member"
                for member in (0, 1):
                    path = recordings[role].with_name(f"{role}.member-{member}.jsonl")
                    lines = [json.loads(line)
                             for line in path.read_text(encoding="utf-8").splitlines()]
                    header = lines[0]
                    assert header["schemaVersion"] == "1alpha1", header
                    assert header["flowId"] == f"group-test-m{member}", header
                    assert len(lines) == 3, f"{path.name}: header and two records expected"
        finally:
            listener.stop()
            if caller:
                caller.stop()
            sink.close()
            source.close()
    return True


def run_misuse(peer_path: Path) -> None:
    # A trigger window is a single-socket feature; group mode refuses it.
    listener = Peer(peer_path, ["--role", "listener", "--group", "broadcast", "--group-members", "2",
                                "--jsonl-output", "/nonexistent/x.jsonl", "--flow-id", "f",
                                "--provider-version", "v", "--provider-revision", "r",
                                "--build-profile", "b"])
    caller = None
    try:
        port = parse_prefixed_json(listener.wait_for("READY"), "READY")["port"]
        caller = Peer(peer_path, ["--role", "caller", "--port", str(port), "--group", "broadcast",
                                  "--member", "127.0.0.1,127.0.0.1", "--member", "127.0.0.1,127.0.0.1"])
        for peer in (listener, caller):
            peer.wait_for("CONNECTED")
            peer.wait_for("PROFILE")
        listener.send("ARM 2 100")
        listener.process.wait(timeout=5)
        listener.stderr_thread.join(timeout=2)
        assert listener.process.returncode != 0
        assert any("ARM" in line for line in listener.stderr_lines), listener.stderr_lines
    finally:
        listener.stop()
        if caller:
            caller.stop()
    for invalid in (
        ("--role", "caller", "--port", "9", "--group", "ring",
         "--member", "127.0.0.1,127.0.0.1", "--member", "127.0.0.1,127.0.0.1"),
        ("--role", "caller", "--port", "9", "--group", "broadcast", "--member", "127.0.0.1,127.0.0.1"),
        ("--role", "caller", "--port", "9", "--group", "broadcast",
         "--member", "127.0.0.1", "--member", "127.0.0.1,127.0.0.1"),
        ("--role", "caller", "--port", "9", "--group", "broadcast",
         "--member", "127.0.0.1,localhost", "--member", "127.0.0.1,127.0.0.1"),
        ("--role", "caller", "--port", "9", "--group", "broadcast", "--remote-address", "127.0.0.2",
         "--member", "127.0.0.1,127.0.0.1", "--member", "127.0.0.1,127.0.0.1"),
        ("--role", "caller", "--port", "9", "--member", "127.0.0.1,127.0.0.1",
         "--member", "127.0.0.1,127.0.0.1"),
        ("--role", "caller", "--port", "9", "--group", "broadcast", "--group-min-stable-timeout-ms", "100",
         "--member", "127.0.0.1,127.0.0.1", "--member", "127.0.0.1,127.0.0.1"),
        ("--role", "caller", "--port", "9", "--group", "backup", "--group-min-stable-timeout-ms", "59",
         "--member", "127.0.0.1,127.0.0.1", "--member", "127.0.0.1,127.0.0.1"),
        ("--role", "listener", "--group", "broadcast"),
        ("--role", "listener", "--group", "broadcast", "--group-members", "1"),
        ("--role", "listener", "--group-members", "2"),
        ("--role", "listener", "--group", "broadcast", "--group-members", "2",
         "--member", "127.0.0.1,127.0.0.1"),
    ):
        result = subprocess.run([str(peer_path), *invalid], capture_output=True, text=True, timeout=3)
        assert result.returncode != 0 and "usage:" in result.stderr, invalid


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--peer", type=Path, required=True)
    parser.add_argument("--provider", required=True)
    args = parser.parse_args()
    for group_type in ("broadcast", "backup"):
        if not run_group(args.peer, args.provider, group_type):
            print("SKIP: this SRT library has no connection groups")
            sys.exit(77)
    run_misuse(args.peer)
    print("incident peer: broadcast and backup group relay, per-member recording and misuse checks passed")


if __name__ == "__main__":
    main()
