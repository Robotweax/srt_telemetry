#!/usr/bin/env python3

import argparse
import csv
import hashlib
import json
import re
import sys
from decimal import Decimal, InvalidOperation
from pathlib import Path


INCIDENT_PATTERN = re.compile(r"^incident-[0-9]{3,}-[a-z0-9-]+$")


def fail(message: str) -> None:
    raise RuntimeError(message)


def require_keys(value: dict, keys: set, name: str) -> None:
    missing = keys - set(value)
    if missing:
        fail(f"{name} is missing keys: {sorted(missing)}")


def parse_number(value: str, name: str) -> Decimal:
    try:
        return Decimal(value)
    except InvalidOperation as error:
        raise RuntimeError(f"invalid number for {name}: {value!r}") from error


def resolve_dataset(manifest_path: Path, relative_name: str) -> Path:
    incident_directory = manifest_path.parent.resolve()
    dataset_path = (incident_directory / relative_name).resolve()
    if dataset_path.parent != incident_directory:
        fail(f"dataset must be directly inside the incident directory: {relative_name}")
    return dataset_path


def load_rows(samples_path: Path) -> tuple:
    with samples_path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        if reader.fieldnames is None:
            fail("sample dataset has no header")
        rows = list(reader)
    required = {
        "role",
        "provider",
        "sample_index",
        "observed_at_unix_nano",
        "sample_monotonic_nano",
        "expected_quality_flags",
        "expected_interval_nano",
    }
    missing = required - set(reader.fieldnames)
    if missing:
        fail(f"sample dataset is missing columns: {sorted(missing)}")
    return reader.fieldnames, rows


def validate_timeline(manifest: dict, rows: list) -> dict:
    dataset = manifest["dataset"]
    declared_roles = dataset["roles"]
    interval = int(dataset["sampleIntervalNano"])
    providers = {item["role"]: item["provider"] for item in manifest["providers"]}
    by_role = {role: [] for role in declared_roles}
    by_key = {}
    for row in rows:
        role = row["role"]
        if role not in by_role:
            fail(f"undeclared role in samples: {role}")
        if row["provider"] != providers.get(role):
            fail(f"provider mismatch for role {role}: {row['provider']}")
        sample_index = int(row["sample_index"])
        key = (role, sample_index)
        if key in by_key:
            fail(f"duplicate sample: {key}")
        by_key[key] = row
        by_role[role].append(row)
    for role, role_rows in by_role.items():
        role_rows.sort(key=lambda row: int(row["sample_index"]))
        indexes = [int(row["sample_index"]) for row in role_rows]
        if indexes != list(range(len(role_rows))):
            fail(f"non-contiguous sample indexes for {role}: {indexes}")
        monotonic = [int(row["sample_monotonic_nano"]) for row in role_rows]
        if any(
            current - previous != interval
            for previous, current in zip(monotonic, monotonic[1:])
        ):
            fail(f"unexpected monotonic interval for {role}")
    lengths = {len(role_rows) for role_rows in by_role.values()}
    if len(lengths) != 1:
        fail(f"roles have different sample counts: {sorted(lengths)}")
    for sample_index in range(next(iter(lengths))):
        observed = {
            int(by_key[(role, sample_index)]["observed_at_unix_nano"])
            for role in declared_roles
        }
        if len(observed) != 1:
            fail(f"synthetic roles are not aligned at sample {sample_index}")
    return by_key


def validate_expectations(manifest: dict, fieldnames: list, by_key: dict) -> None:
    identifiers = set()
    for expectation in manifest["machineExpectations"]:
        identifier = expectation["id"]
        if identifier in identifiers:
            fail(f"duplicate machine expectation id: {identifier}")
        identifiers.add(identifier)
        role = expectation["role"]
        source = expectation.get("source", "delta")
        field = (
            expectation["field"]
            if source == "raw"
            else f"expected_{expectation['field']}"
        )
        if field not in fieldnames:
            fail(f"{identifier} references missing field {field}")
        first = int(expectation["sampleRange"]["first"])
        last = int(expectation["sampleRange"]["last"])
        if first > last:
            fail(f"{identifier} has an inverted sample range")
        for sample_index in range(first, last + 1):
            row = by_key.get((role, sample_index))
            if row is None:
                fail(f"{identifier} references missing sample {role}/{sample_index}")
            actual_text = row[field]
            operator = expectation["operator"]
            if operator == "unavailable":
                if actual_text != "":
                    fail(
                        f"{identifier} expected {field} to be unavailable at "
                        f"{sample_index}"
                    )
                continue
            if actual_text == "":
                fail(f"{identifier} expected {field} to be available at {sample_index}")
            actual = parse_number(actual_text, field)
            expected = Decimal(str(expectation["value"]))
            if operator == "equals" and actual != expected:
                fail(
                    f"{identifier} expected {expected}, got {actual} at {sample_index}"
                )
            if operator == "greater-than":
                if actual <= expected:
                    fail(
                        f"{identifier} expected greater than {expected}, got "
                        f"{actual} at {sample_index}"
                    )
                continue
            if operator == "less-than":
                if actual >= expected:
                    fail(
                        f"{identifier} expected less than {expected}, got {actual} "
                        f"at {sample_index}"
                    )
                continue
            if operator == "equals-field-plus":
                comparison = f"expected_{expectation['comparisonField']}"
                if comparison not in fieldnames or row[comparison] == "":
                    fail(f"{identifier} comparison field is unavailable: {comparison}")
                comparison_value = parse_number(row[comparison], comparison)
                if actual != comparison_value + expected:
                    fail(
                        f"{identifier} expected {comparison} + {expected}, got "
                        f"{actual} at {sample_index}"
                    )
                continue
            if operator != "equals":
                fail(f"{identifier} uses unsupported operator: {operator}")


def validate_capture(manifest: dict, by_key: dict) -> None:
    provenance = manifest["provenance"]
    capture = manifest.get("capture")
    if provenance["kind"] == "synthetic-contract-fixture":
        if capture is not None:
            fail("synthetic fixture cannot contain captured-provider metadata")
        return
    if provenance["kind"] != "captured-provider-run" or not isinstance(capture, dict):
        fail("captured provider run requires capture metadata")
    artifacts = capture.get("artifacts")
    if not isinstance(artifacts, list):
        fail("capture artifacts are missing")
    artifact_roles = {artifact.get("role") for artifact in artifacts}
    if artifact_roles != set(manifest["dataset"]["roles"]):
        fail(f"capture artifact roles do not match dataset roles: {artifact_roles}")
    fault_proxy = capture.get("faultProxy")
    delay_proxy = capture.get("delayProxy")
    application_pause = capture.get("applicationPause")
    bandwidth_proxy = capture.get("bandwidthProxy")
    if sum(
        candidate is not None
        for candidate in [
            fault_proxy,
            delay_proxy,
            application_pause,
            bandwidth_proxy,
        ]
    ) != 1:
        fail(
            "capture must contain exactly one fault proxy, delay proxy, "
            "application pause, or bandwidth proxy"
        )
    first_active = manifest["faultPlan"]["firstActiveSample"]
    last_active = manifest["faultPlan"]["lastActiveSample"]
    if application_pause is not None:
        if not isinstance(application_pause, dict):
            fail("capture application-pause observations are invalid")
        if capture.get("environment", {}).get("transport") != "ipv4-loopback-direct":
            fail("application-pause capture must use direct IPv4 loopback")
        samples = application_pause.get("samples")
        if not isinstance(samples, list):
            fail("capture application-pause samples are missing")
        indexes = [sample.get("sampleIndex") for sample in samples]
        expected_indexes = list(range(first_active, last_active + 1))
        if indexes != expected_indexes:
            fail(f"capture application-pause samples are not contiguous: {indexes}")
        elapsed = [sample.get("elapsedMonotonicNano") for sample in samples]
        if any(not isinstance(value, int) or value <= 0 for value in elapsed):
            fail("capture application-pause elapsed times are invalid")
        if any(current <= previous for previous, current in zip(elapsed, elapsed[1:])):
            fail("capture application-pause elapsed times are not increasing")
        measured_pause = application_pause.get("measuredPauseNano")
        if not isinstance(measured_pause, int) or measured_pause < elapsed[-1]:
            fail("capture application-pause duration is inconsistent")
        return
    proxy = (
        fault_proxy
        if fault_proxy is not None
        else delay_proxy
        if delay_proxy is not None
        else bandwidth_proxy
    )
    if not isinstance(proxy, dict) or not isinstance(proxy.get("stages"), list):
        fail("capture proxy observations are missing")
    stages = proxy["stages"]
    indexes = [stage.get("sampleIndex") for stage in stages]
    expected_indexes = list(range(1, max(index for _, index in by_key) + 1))
    if indexes != expected_indexes:
        fail(f"capture fault stages are not contiguous: {indexes}")
    if bandwidth_proxy is not None:
        if capture.get("environment", {}).get("transport") != (
            "ipv4-loopback-udp-proxy"
        ):
            fail("bandwidth capture must use the IPv4 loopback UDP proxy")
        active_rate = proxy.get("activeDataPacketsPerSecond")
        recovery_rate = proxy.get("recoveryDataPacketsPerSecond")
        active_messages = proxy.get("activeTransferMessages")
        steady_messages = proxy.get("steadyMessagesPerInterval")
        if not isinstance(active_rate, int) or active_rate <= 0:
            fail("bandwidth proxy active data-packet rate is invalid")
        if not isinstance(recovery_rate, int) or recovery_rate <= active_rate:
            fail("bandwidth proxy recovery rate must exceed its active rate")
        if not isinstance(active_messages, int) or active_messages <= 0:
            fail("bandwidth proxy active transfer size is invalid")
        if not isinstance(steady_messages, int) or steady_messages <= 0:
            fail("bandwidth proxy steady transfer size is invalid")
        for stage in stages:
            index = stage["sampleIndex"]
            active = first_active <= index <= last_active
            if stage.get("active") != active:
                fail(f"bandwidth proxy active state mismatch at sample {index}")
            for field in [
                "queueAtStart",
                "downstreamDataPackets",
                "queuedDataPackets",
                "forwardedDataPackets",
                "maximumQueuedPackets",
                "queueAtEnd",
            ]:
                value = stage.get(field)
                if not isinstance(value, int) or value < 0:
                    fail(
                        f"bandwidth proxy {field} is invalid at sample {index}: "
                        f"{value}"
                    )
        active_stages = [stage for stage in stages if stage["active"]]
        if not active_stages:
            fail("bandwidth proxy has no active stages")
        if active_stages[0]["downstreamDataPackets"] < active_messages:
            fail("bandwidth proxy did not observe the offered active transfer")
        if active_stages[0]["queueAtEnd"] <= 0:
            fail("bandwidth proxy queue did not grow in the first active stage")
        if active_stages[-1]["queueAtEnd"] <= 0:
            fail("bandwidth proxy queue drained before the active restriction ended")
        if max(stage["maximumQueuedPackets"] for stage in active_stages) <= 0:
            fail("bandwidth proxy did not record queued data packets")
        recovery_stages = [
            stage for stage in stages if stage["sampleIndex"] > last_active
        ]
        if not recovery_stages or recovery_stages[0]["queueAtStart"] <= 0:
            fail("bandwidth proxy recovery did not begin with a retained queue")
        if recovery_stages[0]["queueAtEnd"] != 0:
            fail("bandwidth proxy queue did not drain in the first recovery stage")
        return
    if delay_proxy is not None:
        messages = proxy.get("messagesPerInterval")
        delay_millis = proxy.get("oneWayDelayMillis")
        if not isinstance(messages, int) or messages <= 0:
            fail("delay proxy message count is invalid")
        if not isinstance(delay_millis, int) or delay_millis <= 0:
            fail("delay proxy one-way delay is invalid")
        for stage in stages:
            index = stage["sampleIndex"]
            active = first_active <= index <= last_active
            if stage.get("active") != active:
                fail(f"delay proxy active state mismatch at sample {index}")
            if stage.get("downstreamPackets", 0) < messages:
                fail(
                    f"delay proxy downstream packet count is invalid at sample {index}"
                )
            delayed = stage.get("delayedDownstreamPackets", 0) + stage.get(
                "delayedReversePackets", 0
            )
            if active and delayed <= 0:
                fail(f"delay proxy did not delay packets at sample {index}")
            if not active and delayed != 0:
                fail(f"delay proxy delayed packets outside the fault at sample {index}")
        return

    expected_drops = proxy.get("expectedDropsPerActiveInterval")
    messages = proxy.get("messagesPerInterval")
    if not isinstance(messages, int) or messages <= 0:
        fail("fault proxy message count is invalid")
    if not isinstance(expected_drops, int) or expected_drops <= 0:
        fail("fault proxy expected drop count is invalid")
    pattern = proxy.get("pattern")
    if pattern is None:
        pattern = "constant-every-nth"
    if pattern == "constant-every-nth":
        drop_every = proxy.get("dropEveryOriginalDataPacket")
        if not isinstance(drop_every, int) or drop_every <= 1:
            fail("constant fault proxy drop interval is invalid")
        if any(
            key in proxy
            for key in ["burstStartOriginalDataPacket", "burstLengthPackets"]
        ):
            fail("constant fault proxy contains burst configuration")
        calculated_drops = messages // drop_every
    elif pattern == "contiguous-burst":
        burst_start = proxy.get("burstStartOriginalDataPacket")
        burst_length = proxy.get("burstLengthPackets")
        if not isinstance(burst_start, int) or burst_start <= 0:
            fail("burst fault proxy start is invalid")
        if not isinstance(burst_length, int) or burst_length <= 0:
            fail("burst fault proxy length is invalid")
        if "dropEveryOriginalDataPacket" in proxy:
            fail("burst fault proxy contains constant-loss configuration")
        if burst_start + burst_length - 1 > messages:
            fail("burst fault proxy range exceeds the interval")
        calculated_drops = burst_length
    else:
        fail(f"unsupported fault proxy pattern: {pattern}")
    if expected_drops != calculated_drops:
        fail(
            f"fault proxy expected drop count mismatch: "
            f"declared {expected_drops}, calculated {calculated_drops}"
        )
    for stage in stages:
        index = stage["sampleIndex"]
        active = first_active <= index <= last_active
        if stage.get("active") != active:
            fail(f"fault proxy active state mismatch at sample {index}")
        if stage.get("originalDataPackets") != messages:
            fail(f"fault proxy original packet count mismatch at sample {index}")
        drops = stage.get("droppedOriginalDataPackets")
        if drops != (expected_drops if active else 0):
            fail(f"fault proxy drop count mismatch at sample {index}: {drops}")
        if active and stage.get("repeatedDataPackets", 0) < expected_drops:
            fail(
                f"fault proxy did not observe retransmission attempts at sample {index}"
            )


def validate_recordings(manifest_path: Path, manifest: dict) -> None:
    recordings = manifest.get("recordings")
    if recordings is None:
        return
    if not isinstance(recordings, dict):
        fail("recordings metadata is not an object")
    require_keys(
        recordings,
        {"schemaVersion", "format", "flowId", "sender", "receiver", "analysis"},
        "recordings",
    )
    if recordings["schemaVersion"] != "1alpha1" or recordings["format"] != (
        "robotweax-srt-telemetry-jsonl-v1"
    ):
        fail("unsupported retained recording format")
    if recordings["flowId"] != manifest["incidentId"]:
        fail("retained recording flow id does not match the incident id")
    providers = {
        provider["role"]: provider["provider"] for provider in manifest["providers"]
    }
    expected_records = manifest["dataset"]["rows"] // len(manifest["dataset"]["roles"])
    artifact_hashes = {}
    for role in ["sender", "receiver"]:
        metadata = recordings.get(role)
        if not isinstance(metadata, dict):
            fail(f"retained {role} recording metadata is missing")
        path = resolve_dataset(manifest_path, metadata["file"])
        contents = path.read_bytes()
        digest = hashlib.sha256(contents).hexdigest()
        if digest != metadata["sha256"]:
            fail(f"retained {role} recording digest mismatch")
        lines = contents.decode("utf-8").splitlines()
        if (
            len(lines) != expected_records + 1
            or metadata["records"] != expected_records
        ):
            fail(f"retained {role} recording count mismatch")
        header = json.loads(lines[0])
        expected_header = {
            "kind": "robotweax-srt-telemetry-recording",
            "schemaVersion": "1alpha1",
            "flowId": manifest["incidentId"],
            "role": role,
            "configuredProvider": providers[role],
            "recordCount": expected_records,
            "overwrittenCount": 0,
        }
        for field, expected in expected_header.items():
            if header.get(field) != expected:
                fail(f"retained {role} recording header {field} mismatch")
        artifact_hashes[metadata["file"]] = digest

    analysis_metadata = recordings.get("analysis")
    if not isinstance(analysis_metadata, dict):
        fail("retained recording analysis metadata is missing")
    analysis_path = resolve_dataset(manifest_path, analysis_metadata["file"])
    analysis_digest = hashlib.sha256(analysis_path.read_bytes()).hexdigest()
    if analysis_digest != analysis_metadata["sha256"]:
        fail("retained recording analysis digest mismatch")
    analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
    if analysis.get("schemaVersion") != analysis_metadata["schemaVersion"]:
        fail("retained recording analysis schema mismatch")
    source = analysis.get("source", {})
    if source.get("format") != recordings["format"] or source.get("flowId") != (
        manifest["incidentId"]
    ):
        fail("retained recording analysis source mismatch")
    analysis_artifacts = {
        artifact.get("path"): artifact.get("sha256")
        for artifact in source.get("artifacts", [])
    }
    if analysis_artifacts != artifact_hashes:
        fail("retained recording analysis artifact hashes do not match the JSONL files")


def validate_manifest(manifest_path: Path) -> None:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict):
        fail("manifest root is not an object")
    require_keys(
        manifest,
        {
            "schemaVersion",
            "incidentId",
            "failureClass",
            "topology",
            "direction",
            "providers",
            "transportProfile",
            "faultPlan",
            "provenance",
            "dataset",
            "expectedEvidence",
            "machineExpectations",
        },
        "manifest",
    )
    if manifest["schemaVersion"] != "1alpha1":
        fail(f"unsupported schema version: {manifest['schemaVersion']}")
    if not INCIDENT_PATTERN.fullmatch(manifest["incidentId"]):
        fail(f"invalid incident id: {manifest['incidentId']}")
    provenance = manifest["provenance"]
    if (
        provenance["kind"] == "synthetic-contract-fixture"
        and provenance["capturedFromProvider"]
    ):
        fail("synthetic fixture cannot claim provider capture")
    dataset = manifest["dataset"]
    if dataset["format"] != "robotweax-srt-telemetry-csv-v1":
        fail(f"unsupported dataset format: {dataset['format']}")
    samples_path = resolve_dataset(manifest_path, dataset["samplesFile"])
    contents = samples_path.read_bytes()
    digest = hashlib.sha256(contents).hexdigest()
    if digest != dataset["sha256"]:
        fail(f"sample digest mismatch: expected {dataset['sha256']}, got {digest}")
    fieldnames, rows = load_rows(samples_path)
    if len(rows) != int(dataset["rows"]):
        fail(f"sample row count mismatch: expected {dataset['rows']}, got {len(rows)}")
    by_key = validate_timeline(manifest, rows)
    first_active = int(manifest["faultPlan"]["firstActiveSample"])
    last_active = int(manifest["faultPlan"]["lastActiveSample"])
    if first_active > last_active:
        fail("fault plan has an inverted active sample range")
    for role in dataset["roles"]:
        if (role, first_active) not in by_key or (role, last_active) not in by_key:
            fail(f"fault plan is outside the dataset for role {role}")
    validate_expectations(manifest, fieldnames, by_key)
    validate_capture(manifest, by_key)
    validate_recordings(manifest_path, manifest)
    print(f"golden incident valid: {manifest['incidentId']} ({len(rows)} rows)")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate a Robotweax golden incident")
    parser.add_argument("manifest", type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if not arguments.manifest.is_file():
        fail(f"manifest does not exist: {arguments.manifest}")
    validate_manifest(arguments.manifest.resolve())
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"golden incident validation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
