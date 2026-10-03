#!/usr/bin/env python3
"""End-to-end test of srt-live-transmit with SRT Telemetry (the reference
integration in integrations/srt-live-transmit).

Two patched srt-live-transmit processes carry a UDP feed over SRT on
loopback: a sender (udp -> srt caller) and a receiver (srt listener -> udp),
each with -telemetry. The test checks that:
- a sender started before the receiver shows as one lost row, which the
  connection then continues;
- each process serves /metrics, the snapshot API and the dashboard, with its
  endpoint graded OK, labelled with the stream name and the provider;
- when the receiver goes away, the sender shows the connection as lost;
- when the receiver comes back, the sender continues the same source (one
  row, same source_id) and recovers to OK;
- a caller refused for a wrong passphrase shows as one Failing row with one
  rejection event, however often it retries;
- a bad -telemetry spec stops the program with an error.

Usage: srt_live_transmit_e2e.py --slt BUILD/srt-live-transmit [--keep-logs]
"""

import argparse
import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

failures = []


def check(condition, message):
    print(("ok   " if condition else "FAIL ") + message)
    if not condition:
        failures.append(message)


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def get(port, path):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=5) as response:
        return response.status, response.read().decode()


def verdicts(port):
    """{(source_id, role): (verdict, labels)} from /metrics."""
    out = {}
    _, text = get(port, "/metrics")
    for line in text.splitlines():
        match = re.match(r'srt_health_verdict\{(.*)\} (\d+)$', line)
        if match:
            labels = dict(re.findall(r'(\w+)="((?:[^"\\]|\\.)*)"', match.group(1)))
            out[(labels["source_id"], labels["role"])] = (int(match.group(2)), labels)
    return out


def reasons(port):
    _, text = get(port, "/metrics")
    return dict(re.findall(r'srt_health_info\{source_id="(\d+)".*?reason="(\w+)"', text))


def wait(condition, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if condition():
                return True
        except OSError:
            pass
        time.sleep(0.25)
    try:
        return bool(condition())
    except OSError:
        return False


class Feeder(threading.Thread):
    """A 5.3 Mbit/s UDP feed: 1316-byte packets, 500 per second."""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.running = True

    def run(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        payload = bytes(1316)
        next_time = time.monotonic()
        while self.running:
            sock.sendto(payload, ("127.0.0.1", self.port))
            next_time += 0.002
            delay = next_time - time.monotonic()
            if delay > 0:
                time.sleep(delay)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--slt", required=True)
    parser.add_argument("--keep-logs", action="store_true")
    args = parser.parse_args()
    slt = os.path.abspath(args.slt)
    work = tempfile.mkdtemp(prefix="srt-live-transmit-e2e-")
    processes = {}

    def start(name, *argv):
        log = open(os.path.join(work, name + ".log"), "w")
        processes[name] = subprocess.Popen([slt, *argv], stdout=log, stderr=subprocess.STDOUT,
                                           stdin=subprocess.DEVNULL, start_new_session=True)

    def stop(name):
        process = processes.pop(name)
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

    srt_port = free_port(socket.SOCK_DGRAM)
    udp_in = free_port(socket.SOCK_DGRAM)
    udp_out = free_port(socket.SOCK_DGRAM)
    tx_port, rx_port = free_port(), free_port()
    receiver_args = ["-telemetry", f"127.0.0.1:{rx_port},name=cam-1", f"srt://:{srt_port}?mode=listener",
                     f"udp://127.0.0.1:{udp_out}"]
    feeder = Feeder(udp_in)
    try:
        # Without -telemetry the program is unchanged; a bad spec is refused.
        help_text = subprocess.run([slt, "-h"], capture_output=True, text=True, timeout=10)
        check("-telemetry" in help_text.stdout + help_text.stderr, "-telemetry listed in the help")
        bad = subprocess.run([slt, "-telemetry", "bogus", "udp://:1", "udp://127.0.0.1:2"], capture_output=True,
                             text=True, timeout=10)
        check(bad.returncode != 0 and "SRT Telemetry" in bad.stderr, "a bad spec stops the program with an error")

        def graded(port, role):
            found = [v for (sid, r), v in verdicts(port).items() if r == role]
            return len(found) == 1 and found[0][0] == 1

        # The sender starts while the receiver is down: its connection
        # attempts show as one row, lost (Failing), not as nothing.
        start("sender", "-telemetry", f"127.0.0.1:{tx_port},name=cam-1", f"udp://:{udp_in}",
              f"srt://127.0.0.1:{srt_port}")
        feeder.start()
        check(wait(lambda: len(verdicts(tx_port)) == 1 and
                   list(reasons(tx_port).values()) == ["connection_lost"], 15),
              "sender visible as lost while the receiver is down")
        early = list(verdicts(tx_port))
        start("receiver", *receiver_args)

        check(wait(lambda: graded(tx_port, "sender"), 20), "sender endpoint graded OK")
        check(wait(lambda: graded(rx_port, "receiver"), 20), "receiver endpoint graded OK")
        sender = verdicts(tx_port)
        sender_id = next(iter(sender))[0] if sender else None
        check(list(sender) == early, "the connected sender continues the row of its attempts")
        labels = next(iter(sender.values()))[1] if sender else {}
        check(labels.get("stream") == "cam-1" and labels.get("provider") in ("haivision-srt", "robotweax-srt"),
              f"stream and provider labels ({labels})")
        status, snapshot = get(tx_port, "/api/v1/snapshot")
        sources = json.loads(snapshot).get("sources", [])
        check(status == 200 and len(sources) == 1 and sources[0].get("connection") == "connected",
              "snapshot: one connected source")
        check(get(rx_port, "/")[0] == 200, "dashboard served")
        _, events = get(tx_port, "/api/v1/events")
        check(any(e["kind"] == "connected" for e in json.loads(events)["events"]), "connected event recorded")

        # The receiver goes away: the sender's endpoint shows the loss.
        stop("receiver")
        check(wait(lambda: reasons(tx_port).get(sender_id) == "connection_lost" and
                   verdicts(tx_port)[(sender_id, "sender")][0] == 4, 10),
              "sender shows connection lost when the receiver stops")

        # It comes back: the same source continues and recovers.
        start("receiver", *receiver_args)
        check(wait(lambda: list(verdicts(tx_port)) == [(sender_id, "sender")] and graded(tx_port, "sender"), 30),
              "sender continues the same source and recovers to OK")
        check(wait(lambda: graded(rx_port, "receiver"), 20), "restarted receiver graded OK")
        _, metrics = get(tx_port, "/metrics")
        check(f'srt_sender_unique_sent_bits_per_second{{source_id="{sender_id}"' in metrics or
              wait(lambda: f'srt_sender_unique_sent_bits_per_second{{source_id="{sender_id}"' in
                   get(tx_port, "/metrics")[1], 15),
              "sender rates reported after the reconnection")

        # A caller with the wrong passphrase: one Failing row and one
        # rejection event, however often it retries.
        secret_port, bad_port = free_port(socket.SOCK_DGRAM), free_port()
        start("guarded", f"srt://:{secret_port}?mode=listener&passphrase=correct-horse-1",
              f"udp://127.0.0.1:{udp_out}")
        start("intruder", "-telemetry", f"127.0.0.1:{bad_port},name=cam-2", f"udp://:{free_port(socket.SOCK_DGRAM)}",
              f"srt://127.0.0.1:{secret_port}?passphrase=wrong-battery-2")
        time.sleep(6)
        _, events = get(bad_port, "/api/v1/events")
        kinds = [e["kind"] for e in json.loads(events)["events"]]
        check(len(verdicts(bad_port)) == 1 and list(reasons(bad_port).values()) == ["connection_lost"],
              "a refused caller shows as one Failing row")
        check(kinds.count("rejected") == 1 and "km_state_changed" not in kinds,
              f"one rejection event for repeated refusals ({kinds.count('rejected')})")
    except Exception as error:
        failures.append(str(error))
        print(f"FAIL {error}")
    finally:
        feeder.running = False
        for name in list(processes):
            stop(name)
        if failures or args.keep_logs:
            for name in ("sender", "receiver"):
                path = os.path.join(work, name + ".log")
                if os.path.exists(path):
                    print(f"--- {name}.log (tail)")
                    print("".join(open(path).readlines()[-15:]), end="")
            print(f"logs: {work}")

    if failures:
        print(f"{len(failures)} srt-live-transmit checks failed")
        return 1
    print("all srt-live-transmit checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
