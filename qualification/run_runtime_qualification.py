#!/usr/bin/env python3

import argparse
import datetime as dt
import hashlib
import json
import math
import platform
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_mode(peer: Path, argument: str, prefix: str, timeout_seconds: float) -> dict:
    started = time.monotonic_ns()
    try:
        completed = subprocess.run(
            [str(peer), argument],
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            f"{peer.name} {argument} exceeded {timeout_seconds} seconds"
        ) from error
    wall_clock_nano = time.monotonic_ns() - started
    if completed.returncode != 0:
        raise RuntimeError(
            json.dumps(
                {
                    "command": [str(peer), argument],
                    "returnCode": completed.returncode,
                    "stdout": completed.stdout,
                    "stderr": completed.stderr,
                },
                sort_keys=True,
            )
        )
    marker = f"{prefix} "
    matching = [line for line in completed.stdout.splitlines() if line.startswith(marker)]
    if len(matching) != 1:
        raise RuntimeError(f"expected one {prefix} line, got {completed.stdout!r}")
    observations = json.loads(matching[0][len(marker) :])
    if not isinstance(observations, dict):
        raise RuntimeError(f"{prefix} payload is not an object")
    return {
        "status": "pass",
        "wallClockNano": wall_clock_nano,
        "observations": observations,
    }


def require_number(value: object, name: str, minimum: Optional[float] = None) -> float:
    if not isinstance(value, (int, float)) or not math.isfinite(value):
        raise RuntimeError(f"{name} is not finite: {value!r}")
    numeric = float(value)
    if minimum is not None and numeric < minimum:
        raise RuntimeError(f"{name} is below {minimum}: {value!r}")
    return numeric


def validate_summary(summary: object, name: str, allow_negative: bool) -> None:
    if not isinstance(summary, dict):
        raise RuntimeError(f"{name} is not an object")
    minimum = None if allow_negative else 0.0
    for field in [
        "meanNanoseconds",
        "minimumNanoseconds",
        "p50Nanoseconds",
        "p95Nanoseconds",
        "maximumNanoseconds",
    ]:
        require_number(summary.get(field), f"{name}.{field}", minimum)


def validate_benchmark(result: dict, provider: str) -> None:
    observations = result["observations"]
    if observations.get("provider") != provider:
        raise RuntimeError(f"benchmark provider mismatch: {observations}")
    for pair_name in ["providerCall", "syntheticSdk"]:
        pair = observations.get(pair_name)
        if not isinstance(pair, dict):
            raise RuntimeError(f"missing benchmark pair {pair_name}")
        validate_summary(pair.get("direct"), f"{pair_name}.direct", False)
        validate_summary(pair.get("adapter"), f"{pair_name}.adapter", False)
        validate_summary(pair.get("incremental"), f"{pair_name}.incremental", True)
        require_number(pair.get("adapterToDirectRatio"), f"{pair_name}.ratio", 0.0)
    synthetic_incremental = observations["syntheticSdk"]["incremental"]
    require_number(
        synthetic_incremental.get("p50Nanoseconds"),
        "syntheticSdk.incremental.p50Nanoseconds",
        0.0,
    )


def validate_lifecycle(result: dict, provider: str) -> None:
    observations = result["observations"]
    expected = {
        "provider": provider,
        "sessionDestroyedBeforeClose": True,
        "staleHandleProbeUnsupported": True,
        "closeNativeResult": 0,
        "postCloseError": 4,
        "postCloseNativeResult": -1,
        "postCloseAvailableFields": 0,
    }
    for key, value in expected.items():
        if observations.get(key) != value:
            raise RuntimeError(f"unexpected lifecycle {key}: {observations}")
    require_number(observations.get("closeElapsedNano"), "closeElapsedNano", 0.0)
    require_number(observations.get("postCloseElapsedNano"), "postCloseElapsedNano", 0.0)


def validate_close_race(result: dict, provider: str) -> None:
    observations = result["observations"]
    expected = {
        "provider": provider,
        "orderlySessionBeforeClose": True,
        "raceFailureNativeResult": -1,
        "closeNativeResult": 0,
        "postCloseError": 4,
        "postCloseNativeResult": -1,
        "postCloseAvailableFields": 0,
    }
    for key, value in expected.items():
        if observations.get(key) != value:
            raise RuntimeError(f"unexpected close race {key}: {observations}")
    require_number(observations.get("successfulRaceSamples"), "successfulRaceSamples", 1.0)
    require_number(
        observations.get("providerFailuresAfterClose"), "providerFailuresAfterClose", 1.0
    )
    require_number(observations.get("elapsedNano"), "closeRace.elapsedNano", 1.0)


def qualify_provider(
    provider: str,
    version: str,
    revision: str,
    build_profile: str,
    peer: Path,
    library: Path,
) -> dict:
    benchmark = run_mode(peer, "--benchmark", "BENCHMARK", 60.0)
    lifecycle = run_mode(peer, "--lifecycle", "LIFECYCLE", 15.0)
    close_race = run_mode(peer, "--close-race", "CLOSE_RACE", 10.0)
    validate_benchmark(benchmark, provider)
    validate_lifecycle(lifecycle, provider)
    validate_close_race(close_race, provider)
    return {
        "provider": provider,
        "version": version,
        "revision": revision,
        "buildProfile": build_profile,
        "artifacts": {
            "peerExecutableSha256": sha256_file(peer),
            "providerLibrarySha256": sha256_file(library),
        },
        "benchmark": benchmark,
        "lifecycle": lifecycle,
        "closeSampleRace": close_race,
    }


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Measure public SRT sampling and qualify close/sample lifecycle behavior"
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

    providers = [
        qualify_provider(
            "robotweax-srt",
            arguments.robotweax_version,
            arguments.robotweax_revision,
            arguments.robotweax_build_profile,
            arguments.robotweax_peer,
            arguments.robotweax_library,
        ),
        qualify_provider(
            "haivision-srt",
            arguments.haivision_version,
            arguments.haivision_revision,
            arguments.haivision_build_profile,
            arguments.haivision_peer,
            arguments.haivision_library,
        ),
    ]
    manifest = {
        "schemaVersion": "1alpha1",
        "kind": "public-srt-runtime-qualification",
        "status": "pass",
        "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "telemetry": {
            "revision": arguments.telemetry_revision,
            "adapterVersion": "0.1.0-alpha.0",
        },
        "environment": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "pythonVersion": platform.python_version(),
            "transport": "ipv4-loopback",
            "buildType": "Release",
            "processesSequential": True,
            "schedulerIsolation": False,
            "powerManagementControlled": False,
            "timingClock": "steady_clock",
        },
        "providers": providers,
        "interpretation": {
            "benchmarkIsReleaseGate": False,
            "closeSampleRaceIsSupportedHostUsage": False,
            "supportedLifecycle": "destroy telemetry session before closing its SRT socket",
        },
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"runtime qualification passed: {arguments.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"runtime qualification failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
