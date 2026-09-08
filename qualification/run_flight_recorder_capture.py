#!/usr/bin/env python3
"""Instrumented host demo. Retains synthetic traffic statistics, never payloads.

This coordinator owns scheduling and persistence. SAMPLE_NOW stamps each peer's
real clocks; command round-trip brackets are retained to bound sample ordering.
The native host owns sockets, the injected public SRT adapter, and capture rings.
"""
import argparse
import hashlib
import json
import platform
import resource
import statistics
import subprocess
import time
from pathlib import Path

from run_constant_loss_capture import FaultProxy, Peer, parse_prefixed_json


def require(condition, reason):
    if not condition:
        raise RuntimeError(reason)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def recording_args(args, output, flow):
    return ["--jsonl-output", str(output), "--flow-id", flow,
            "--provider-version", args.provider_version,
            "--provider-revision", args.provider_revision,
            "--build-profile", args.build_profile, "--recorder-capacity", "64"]


def connect(args, directory, flow, record=True, loss=False):
    listener_args = ["--role", "listener"]
    if record:
        listener_args += recording_args(args, directory / "receiver.jsonl", flow)
    receiver = Peer(args.peer, listener_args)
    sender = None
    proxy = None
    try:
        port = parse_prefixed_json(receiver.wait_for("READY"), "READY")["port"]
        if loss:
            proxy = FaultProxy(port, "constant", 20, 0, 0)
            port = proxy.port
        caller_args = ["--role", "caller", "--port", str(port)]
        if record:
            caller_args += recording_args(args, directory / "sender.jsonl", flow)
        sender = Peer(args.peer, caller_args)
        receiver.wait_for("CONNECTED")
        sender.wait_for("CONNECTED")
        for peer in (receiver, sender):
            profile = parse_prefixed_json(peer.wait_for("PROFILE"), "PROFILE")
            peer.runtime_profile = profile
            require(profile["provider"] == args.provider and profile["transport"] == "live"
                    and profile["messageApi"] is True, "host profile mismatch")
        return receiver, sender, proxy
    except Exception:
        receiver.stop()
        if sender:
            sender.stop()
        if proxy:
            proxy.stop()
        raise


def sample(peers, index, brackets):
    start_wall, start = time.time_ns(), time.monotonic_ns()
    for peer in peers:
        peer.send(f"SAMPLE_NOW {index}")
    values = [parse_prefixed_json(peer.wait_for("INCIDENT_SAMPLE"), "INCIDENT_SAMPLE")
              for peer in peers]
    end, end_wall = time.monotonic_ns(), time.time_ns()
    for value in values:
        # Native steady_clock and Python monotonic can have different origins.
        # Map each observation by an offset INTERVAL from this command bracket.
        require(start_wall <= value["observedAtUnixNano"] <= end_wall,
                "wall-clock discontinuity inside sample bracket")
    brackets.append({"sampleIndex": index, "beginMonotonicNano": start,
                     "endMonotonicNano": end, "beginUnixNano": start_wall,
                     "endUnixNano": end_wall,
                     "endpointMonotonicNano": {v["role"]: v["sampleMonotonicNano"] for v in values},
                     "endpointToCoordinatorOffsetNano": {
                         v["role"]: [start - v["sampleMonotonicNano"], end - v["sampleMonotonicNano"]]
                         for v in values}})
    return values


def trigger(peers, events):
    start_wall, start = time.time_ns(), time.monotonic_ns()
    for peer in peers:
        peer.send("TRIGGER_NOW")
    states = [parse_prefixed_json(peer.wait_for("WINDOW_STATE"), "WINDOW_STATE") for peer in peers]
    end = time.monotonic_ns()
    events.append({"kind": "manual-trigger", "beginUnixNano": start_wall,
                   "endUnixNano": time.time_ns(), "beginMonotonicNano": start,
                   "endMonotonicNano": end,
                   "states": {role: state for role, state in zip(["receiver", "sender"], states)}})
    return states


def transfer(peers, index, count):
    for peer in peers:
        peer.send(f"TRANSFER {index} {count}")
    for peer in peers:
        peer.wait_for("TRANSFERRED")


def close(peers, directory, record=True, window=True):
    states = {}
    for peer in peers:
        peer.send("DONE")
    for role, peer in zip(["receiver", "sender"], peers):
        if record and window:
            states[role] = parse_prefixed_json(peer.wait_for("WINDOW_STATE"), "WINDOW_STATE")
        if record:
            peer.wait_for("RECORDED")
        peer.finish()
        (directory / f"{role}.stdout.log").write_text("\n".join(peer.stdout_lines) + "\n")
        (directory / f"{role}.stderr.log").write_text("\n".join(peer.stderr_lines) + "\n")
    return states


def capture_epoch(args, directory, scenario, epoch_id, preceding_epoch=None):
    directory.mkdir(parents=True)
    flow = "flight-recorder-demo"
    receiver, sender, proxy = connect(args, directory, flow, loss=scenario == "loss-recovery")
    peers = [receiver, sender]
    brackets, events, values = [], [], []
    if preceding_epoch:
        events.append({"kind": "reconnect", "previousEpochId": preceding_epoch,
                       "epochId": epoch_id, "baselineReset": True,
                       "detail": "New sockets, telemetry sessions and rings; no delta crosses the connection boundary."})
    try:
        for peer in peers:
            peer.send("ARM 8 1000")
        for peer in peers:
            peer.wait_for("WINDOW_STATE")
        for index in range(12):
            if index in (1, 5, 9):
                transfer(peers, index, 40)
            values.extend(sample(peers, index, brackets))
            time.sleep(0.05)
        initial = [v for v in values if v["sampleIndex"] == 0]
        require(len(initial) == 2 and all(v["qualityFlags"] & 1 for v in initial),
                "new epoch did not start with two fresh SDK baselines")
        first = trigger(peers, events)
        if scenario == "loss-recovery":
            proxy.begin_stage(12, True)
            transfer(peers, 12, 401)  # A later DATA packet exposes the last loss gap.
            observation = proxy.finish_stage()
            require(observation["droppedOriginalDataPackets"] > 0, "no injected packet loss")
            events.append({"kind": "controlled-packet-loss", "observation": observation,
                           "evidenceSource": "harness-only; diagnostic classifier receives telemetry"})
        elif scenario == "receiver-pause":
            sender.send(f"START_TRANSFER 12 {args.pause_messages} 0")
            sender.wait_for("TRANSFER_STARTED")
            paused = time.monotonic_ns()
            for index in range(12, 18):
                time.sleep(0.05)
                values.extend(sample(peers, index, brackets))
            receiver.send(f"START_TRANSFER 12 {args.pause_messages} 0")
            receiver.wait_for("TRANSFER_STARTED")
            resumed = time.monotonic_ns()
            for peer in peers:
                peer.send(f"WAIT_TRANSFER 12 {args.pause_messages}")
            for peer in peers:
                peer.wait_for("TRANSFERRED")
            events.append({"kind": "receiver-read-pause", "beginMonotonicNano": paused,
                           "endMonotonicNano": resumed, "durationNano": resumed - paused,
                           "messages": args.pause_messages, "evidenceSource": "harness-only"})
        repeated = trigger(peers, events)
        require([v["deadlineMonotonicNano"] for v in first] ==
                [v["deadlineMonotonicNano"] for v in repeated], "repeated trigger extended capture")
        index = 18
        first_trigger = next(event for event in events if event["kind"] == "manual-trigger")
        while time.monotonic_ns() <= first_trigger["endMonotonicNano"] + 1_100_000_000:
            values.extend(sample(peers, index, brackets))
            index += 1
            time.sleep(0.05)
        if proxy:
            proxy.stop()
            proxy = None
        states = close(peers, directory)
        retained = {}
        for role, state in states.items():
            require(state["frozen"] and state["finishReason"] == "deadline", "window not frozen on deadline")
            require(state["preRecordCount"] == 8 and state["triggerCount"] == 2, "window pre/coalescing mismatch")
            require(state["recordCount"] <= 64 and state["allocatedRecords"] == 72, "resource cap")
            docs = [json.loads(line) for line in (directory / f"{role}.jsonl").read_text().splitlines()]
            require(len(docs) == state["recordCount"] + 1, "JSONL count mismatch")
            raw_records = [d["raw"] for d in docs[1:]]
            first_nano, last_nano = raw_records[0]["sample_monotonic_nano"], raw_records[-1]["sample_monotonic_nano"]
            retained[role] = {"firstMonotonicNano": first_nano, "lastMonotonicNano": last_nano,
                              "preCoverageNano": state["triggerMonotonicNano"] - first_nano,
                              "postCoverageNano": max(0, last_nano - state["triggerMonotonicNano"]),
                              "initialSdkBaselineVerifiedBeforeRingOverwrite": True}
        if scenario == "loss-recovery":
            require(any(v["raw"]["retransmitted_packet_total"] > 0 for v in values if v["role"] == "sender"),
                    "sender did not observe loss recovery")
        if scenario == "receiver-pause":
            require(max(v["raw"]["receive_buffer_bytes"] for v in values if v["role"] == "receiver") > 0,
                    "receiver pressure not observed")
        manifest = {
            "schemaVersion": 1, "flowId": flow, "evidenceClass": "new-experiment",
            "experiment": scenario, "epochId": epoch_id,
            "provider": {"name": args.provider, "version": args.provider_version,
                         "revision": args.provider_revision, "buildProfile": args.build_profile,
                         "librarySha256": hashlib.sha256(args.provider_library.read_bytes()).hexdigest(),
                         "peerSha256": hashlib.sha256(args.peer.read_bytes()).hexdigest(),
                         "observedRuntimeProfiles": {"sender": sender.runtime_profile,
                                                     "receiver": receiver.runtime_profile}},
            "clock": {"kind": "host-monotonic", "domainId": args.clock_domain,
                      "mapping": "per-endpoint-offset-intervals-from-command-brackets",
                      "endpointDomains": {role: args.clock_domain + "-" + epoch_id + "-" + role
                                          for role in ["sender", "receiver"]},
                      "uncertaintyNano": max(b["endMonotonicNano"] - b["beginMonotonicNano"] for b in brackets),
                      "scope": "Empirical within-run command bound, not UTC synchronization accuracy.",
                      "brackets": brackets},
            "epochs": [{"epochId": epoch_id, "sender": "sender.jsonl", "receiver": "receiver.jsonl",
                        "trigger": events[0] if not preceding_epoch else events[1],
                        "window": states, "retained": retained, "events": events}],
            "configuration": {"transport": "live", "api": "message", "roles": "IPv4 loopback caller sender/listener receiver",
                              "payloadBytes": 1316, "encryption": "none", "mediaPayloadRetained": False,
                              "samplingNominalHz": 20, "preRecords": 8, "postMilliseconds": 1000,
                              "capacityRecordsPerEndpoint": 64, "defaultProviderOptions": True,
                              "effectiveSocketOptions": {
                                  v["role"]: {"messageApi": True, "transport": "live",
                                              "senderTsbpdDelaySeconds": v["raw"]["sender_tsbpd_delay_seconds"],
                                              "receiverTsbpdDelaySeconds": v["raw"]["receiver_tsbpd_delay_seconds"]}
                                  for v in initial},
                              "effectiveOptionsSource": "Connected native PROFILE and initial available public SRT delay gauges; other options were provider defaults, not enumerated."},
            "files": {name: {"sha256": hashlib.sha256((directory / name).read_bytes()).hexdigest(),
                            "bytes": (directory / name).stat().st_size}
                      for name in ["sender.jsonl", "receiver.jsonl"]},
        }
        write_json(directory / "capture-manifest.json", manifest)
        print(f"captured {scenario}: {directory}", flush=True)
        return manifest
    finally:
        for peer in peers:
            peer.stop()
        if proxy:
            proxy.stop()


def overhead(args, directory, repeats):
    directory.mkdir()
    results = []
    for repetition in range(repeats):
        # Rotate ordering to reduce systematic warm-up bias; not a laboratory benchmark.
        modes = ["sampling-disabled", "sample-only", "sample-and-recorder"]
        modes = modes[repetition % 3:] + modes[:repetition % 3]
        for mode in modes:
            trial = directory / f"{repetition}-{mode}"
            trial.mkdir()
            before = resource.getrusage(resource.RUSAGE_CHILDREN)
            peers = connect(args, trial, "overhead", record=mode == "sample-and-recorder")[:2]
            brackets = []
            try:
                if mode == "sample-and-recorder":
                    for peer in peers:
                        peer.send("ARM 8 1000")
                        peer.wait_for("WINDOW_STATE")
                start = time.monotonic_ns()
                for peer in peers:
                    peer.send("START_TRANSFER 1 3000 300")
                    peer.wait_for("TRANSFER_STARTED")
                rss_samples = []
                for index in range(24):
                    if mode != "sampling-disabled":
                        sample(peers, index, brackets)
                    if index in (6, 18):
                        output = subprocess.check_output(["ps", "-o", "rss=", "-p",
                                                          ",".join(str(p.process.pid) for p in peers)], text=True, timeout=5)
                        rss_samples.append(sum(int(v) for v in output.split()))
                    time.sleep(0.05)
                for peer in peers:
                    peer.send("WAIT_TRANSFER 1 3000")
                    peer.wait_for("TRANSFERRED")
                end = time.monotonic_ns()
                states = close(peers, trial, record=mode == "sample-and-recorder")
                after = resource.getrusage(resource.RUSAGE_CHILDREN)
                results.append({"mode": mode, "repetition": repetition,
                                "wallSeconds": (end - start) / 1e9,
                                "childCpuSecondsIncludingStartupAndPs":
                                    after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime,
                                "samplePairRoundTripMedianMicroseconds": statistics.median(
                                    [(b["endMonotonicNano"] - b["beginMonotonicNano"]) / 1000 for b in brackets]) if brackets else None,
                                "observedPeakCombinedPeerRssKiB": max(rss_samples),
                                "window": states})
            finally:
                for peer in peers:
                    peer.stop()
    report = {"schemaVersion": 1, "platform": platform.platform(), "trials": results,
              "conditions": {"messages": 3000, "payloadBytes": 1316, "sendPacingMicroseconds": 300,
                             "samples": 24, "nominalSamplingHz": 20, "repetitions": repeats,
                             "scope": "Two loopback processes, synthetic traffic, Release build; IPC, JSON stdout and scheduling included. CPU includes startup and two ps probes. RSS sampled twice; not a true peak. Recorder trials remain armed; export cost included in CPU, excluded from transfer wall time. No controlled platform-wide claim."},
              "budgets": {"finalRecordsPerEndpoint": 64, "armedAllocatedRecordsPerEndpoint": 72,
                          "combinedPeerRssKiB": 65536, "medianExtraPairRoundTripMicroseconds": 1000,
                          "scope": "Provisional demo budget, this machine only; large default ring capacity must be budgeted separately."}}
    medians = {}
    for mode in ["sampling-disabled", "sample-only", "sample-and-recorder"]:
        selected = [r for r in results if r["mode"] == mode]
        medians[mode] = {key: statistics.median(r[key] for r in selected)
                         for key in ["wallSeconds", "childCpuSecondsIncludingStartupAndPs", "observedPeakCombinedPeerRssKiB"]}
        if mode != "sampling-disabled":
            medians[mode]["samplePairRoundTripMedianMicroseconds"] = statistics.median(
                r["samplePairRoundTripMedianMicroseconds"] for r in selected)
    report["medians"] = medians
    report["budgetChecks"] = {
        "observedRssWithinBudget": max(r["observedPeakCombinedPeerRssKiB"] for r in results) <= 65536,
        "measuredMedianRecorderExtraPairRoundTripWithinBudget":
            medians["sample-and-recorder"]["samplePairRoundTripMedianMicroseconds"] -
            medians["sample-only"]["samplePairRoundTripMedianMicroseconds"] <= 1000}
    write_json(directory / "overhead.json", report)
    print(f"overhead measured: {directory / 'overhead.json'}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--peer", type=Path, required=True)
    parser.add_argument("--provider-library", type=Path, required=True,
                        help="Exact shared library used by the peer build; digest retained per epoch")
    parser.add_argument("--provider", choices=["robotweax-srt", "haivision-srt"], required=True)
    parser.add_argument("--provider-version", required=True)
    parser.add_argument("--provider-revision", required=True)
    parser.add_argument("--build-profile", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pause-messages", type=int, default=1000,
                        help="Default1000 demonstrated. Larger counts are unqualified stress experiments.")
    parser.add_argument("--overhead-repeats", type=int, default=3)
    parser.add_argument("--skip-overhead", action="store_true")
    args = parser.parse_args()
    require(1 <= args.pause_messages <= 11000, "pause messages must be 1..11000")
    require(1 <= args.overhead_repeats <= 9, "overhead repeats must be 1..9")
    require(not args.output.exists(), "output must be new; captures are never overwritten")
    args.clock_domain = f"same-host-{time.monotonic_ns()}"
    args.output.mkdir(parents=True)
    first = capture_epoch(args, args.output / "loss-recovery", "loss-recovery", "epoch-1")
    second = capture_epoch(args, args.output / "receiver-pause", "receiver-pause", "epoch-2", "epoch-1")
    write_json(args.output / "capture-run.json", {"schemaVersion": 1, "provider": first["provider"],
               "peerSha256": hashlib.sha256(args.peer.read_bytes()).hexdigest(),
               "platform": platform.platform(), "epochs": [first, second],
               "reconnectEvidence": "Fresh peer processes/sockets/sessions/rings per sequential epoch; each stream begins with a new SDK baseline before bounded pre-window overwrites it."})
    if not args.skip_overhead:
        overhead(args, args.output / "overhead", args.overhead_repeats)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
