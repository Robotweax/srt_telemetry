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
        if any(current - previous != interval for previous, current in zip(monotonic, monotonic[1:])):
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
        field = f"expected_{expectation['field']}"
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
                    fail(f"{identifier} expected {field} to be unavailable at {sample_index}")
                continue
            if actual_text == "":
                fail(f"{identifier} expected {field} to be available at {sample_index}")
            actual = parse_number(actual_text, field)
            expected = Decimal(str(expectation["value"]))
            if operator == "equals" and actual != expected:
                fail(f"{identifier} expected {expected}, got {actual} at {sample_index}")
            if operator == "equals-field-plus":
                comparison = f"expected_{expectation['comparisonField']}"
                if comparison not in fieldnames or row[comparison] == "":
                    fail(f"{identifier} comparison field is unavailable: {comparison}")
                comparison_value = parse_number(row[comparison], comparison)
                if actual != comparison_value + expected:
                    fail(
                        f"{identifier} expected {comparison} + {expected}, got {actual} at {sample_index}"
                    )
                continue
            if operator != "equals":
                fail(f"{identifier} uses unsupported operator: {operator}")


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
    if provenance["kind"] == "synthetic-contract-fixture" and provenance["capturedFromProvider"]:
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
