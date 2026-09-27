#!/usr/bin/env python3
"""Verify the retained Robotweax SRT 0.2.6 qualification evidence."""

import hashlib
import json
from pathlib import Path
import tarfile


HERE = Path(__file__).resolve().parent
ARCHIVE = HERE / "evidence.tar.gz"
ARCHIVE_SHA256 = "c3e9e73e698e9e616cfbdcae3612c5dcc779ea1c6d650fc4ff9280e3515fb548"
ROBOTWEAX = "50cba37bc89f423ee7c0a292c3c3eae71b5fc7f9"
HAIVISION = "b6b4ae990daa8193625a4ddeaeaed03023b23125"
TELEMETRY = "4fdc7cf8f643d52ffcbc7def0dd818462a6ba433"

# (scenario, direction, capture gate, analyzer selection)
RUNS = {
    "netem-baseline-r2h": ("baseline", "robotweax-to-haivision", "pass", None),
    "netem-baseline-h2r": ("baseline", "haivision-to-robotweax", "pass", None),
    "netem-pause-r2h": ("receiver-pause", "robotweax-to-haivision", "pass", "receiver-side-pressure"),
    "netem-pause-robotweax-to-haivision-2": ("receiver-pause", "robotweax-to-haivision", "rejected", None),
    "netem-pause-robotweax-to-haivision-3": ("receiver-pause", "robotweax-to-haivision", "rejected", None),
    "netem-pause-h2r": ("receiver-pause", "haivision-to-robotweax", "rejected", None),
    "netem-pause-haivision-to-robotweax-2": ("receiver-pause", "haivision-to-robotweax", "rejected", None),
    "netem-pause-haivision-to-robotweax-3": ("receiver-pause", "haivision-to-robotweax", "rejected", "transmission-path-loss-with-recovery"),
    "netem-bandwidth-r2h": ("bandwidth-reduction", "robotweax-to-haivision", "rejected", "sender-side-pressure"),
    "netem-bandwidth-robotweax-to-haivision-2": ("bandwidth-reduction", "robotweax-to-haivision", "rejected", "sender-side-pressure"),
    "netem-bandwidth-robotweax-to-haivision-3": ("bandwidth-reduction", "robotweax-to-haivision", "rejected", "sender-side-pressure"),
    "netem-bandwidth-h2r": ("bandwidth-reduction", "haivision-to-robotweax", "pass", "sender-side-pressure"),
    "netem-bandwidth-haivision-to-robotweax-2": ("bandwidth-reduction", "haivision-to-robotweax", "pass", "sender-side-pressure"),
    "netem-bandwidth-haivision-to-robotweax-3": ("bandwidth-reduction", "haivision-to-robotweax", "pass", "sender-side-pressure"),
    "netem-bandwidth-r2h-2800": ("bandwidth-reduction", "robotweax-to-haivision", "pass", "sender-side-pressure"),
    "netem-pause-h2r-250": ("receiver-pause", "haivision-to-robotweax", "rejected", None),
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def read_json(archive, name):
    member = archive.extractfile(name)
    require(member is not None, f"missing {name}")
    return json.load(member)


def main():
    digest = hashlib.sha256(ARCHIVE.read_bytes()).hexdigest()
    require(digest == ARCHIVE_SHA256, f"archive SHA-256 mismatch: {digest}")

    with tarfile.open(ARCHIVE, "r:gz") as archive:
        members = set(archive.getnames())
        run_files = {name for name in members if name.startswith("netem-") and name.endswith("/run.json")}
        require(run_files == {f"{name}/run.json" for name in RUNS}, "Linux run inventory mismatch")

        for filename in ("interop.json", "runtime.json"):
            result = read_json(archive, filename)
            require(result["status"] == "pass", f"{filename} did not pass")
            providers = {item["provider"]: item["revision"] for item in result["providers"]}
            require(providers == {"robotweax-srt": ROBOTWEAX, "haivision-srt": HAIVISION},
                    f"{filename} provider revision mismatch")
            require(result["telemetry"]["revision"] == TELEMETRY,
                    f"{filename} telemetry revision mismatch")
        require(len(read_json(archive, "interop.json")["scenarios"]) == 2,
                "both cross-provider directions are required")

        for name, expected_count in (("ctest-robotweax", 47), ("ctest-haivision", 5)):
            member = archive.extractfile(f"{name}/LastTest.log")
            require(member is not None, f"missing {name} CTest log")
            passed = member.read().count(b"Test Passed.")
            require(passed == expected_count, f"{name} CTest count mismatch: {passed}")

        for name, (scenario, direction, gate, hypothesis) in RUNS.items():
            result = read_json(archive, f"{name}/run.json")
            require((result["scenario"], result["direction"]) == (scenario, direction),
                    f"{name} scenario/direction mismatch")
            require(result["passedInfrastructure"] and result["cleanup"]["verified"],
                    f"{name} infrastructure or cleanup failed")
            require(result["captureGate"]["status"] == gate, f"{name} capture gate mismatch")
            require(result["analyzer"]["status"] == "completed"
                    and result["analyzer"]["selectedHypothesis"] == hypothesis,
                    f"{name} analyzer mismatch")
            parameters = result["parameters"]
            require(parameters["robotweax_revision"] == ROBOTWEAX
                    and parameters["haivision_revision"] == HAIVISION
                    and result["telemetryRevision"] == TELEMETRY,
                    f"{name} revision mismatch")
            require(result["udpMaxima"] == {"rmemMax": 4194304, "wmemMax": 4194304},
                    f"{name} UDP maxima mismatch")
            expected = 7400 if scenario == "receiver-pause" else 2400
            payload = result["payload"]
            require(payload["integrityVerified"] and payload["expectedMessages"] == expected
                    and payload["uniqueSentPackets"] == expected
                    and payload["uniqueReceivedPackets"] == expected,
                    f"{name} transfer mismatch")
            for artifact in ("sender.jsonl", "receiver.jsonl", "analysis.json", "samples.csv"):
                require(f"{name}/{artifact}" in members, f"{name} missing {artifact}")

            if scenario == "bandwidth-reduction":
                require(result["faultInjection"] == "kernel-netem-rate", f"{name} fault source mismatch")
                active = [item for item in result["kernelSnapshots"]
                          if item.get("phase") == "bandwidth-active"]
                require(active, f"{name} missing active qdisc readback")
                expected_rate = (3808000 if parameters["active_pps"] == 2800 else 1904000)
                qdiscs = active[0]["namespaces"]["sender"]["qdisc"]
                require(any(item.get("options", {}).get("rate", {}).get("rate") == expected_rate
                            for item in qdiscs), f"{name} active qdisc rate mismatch")
            elif scenario == "receiver-pause":
                require(result["faultInjection"] == "application-pause", f"{name} fault source mismatch")
            else:
                require(result["faultInjection"] == "none", f"{name} baseline fault mismatch")

        drops = [585, 590, 583]
        for name, count in zip(("netem-bandwidth-r2h",
                                "netem-bandwidth-robotweax-to-haivision-2",
                                "netem-bandwidth-robotweax-to-haivision-3"), drops):
            reason = read_json(archive, f"{name}/run.json")["captureGate"]["reason"]
            require(f"sender_drop_packet_delta={count}" in reason,
                    f"{name} sender drop evidence mismatch")

    print("SRT 0.2.6 evidence verified: 16 Linux runs, both provider results, archive SHA-256")


if __name__ == "__main__":
    main()
