#!/usr/bin/env python3

import argparse
import csv
import hashlib
import io
import json
import sys
from pathlib import Path


INCIDENT_ID = "incident-001-constant-loss"
INTERVAL_NANO = 1_000_000_000
FIRST_ACTIVE_SAMPLE = 3
LAST_ACTIVE_SAMPLE = 6
LAST_SAMPLE = 8
PACKET_WIRE_BYTES = 1_360

RAW_COUNTERS = [
    "sent_packet_total",
    "received_packet_total",
    "unique_sent_packet_total",
    "unique_received_packet_total",
    "sender_loss_packet_total",
    "receiver_loss_packet_total",
    "retransmitted_packet_total",
    "sender_drop_packet_total",
    "receiver_drop_packet_total",
    "receiver_undecryptable_packet_total",
    "sent_wire_byte_total",
    "received_wire_byte_total",
    "unique_sent_wire_byte_total",
    "unique_received_wire_byte_total",
    "retransmitted_wire_byte_total",
]

RATE_FIELDS = [
    "sent_wire_bits_per_second",
    "received_wire_bits_per_second",
    "unique_sent_wire_bits_per_second",
    "unique_received_wire_bits_per_second",
]

CSV_COLUMNS = [
    "role",
    "provider",
    "sample_index",
    "observed_at_unix_nano",
    "sample_monotonic_nano",
    *RAW_COUNTERS,
    "rtt_seconds",
    "expected_quality_flags",
    "expected_interval_nano",
    *[f"expected_{field.replace('_total', '_delta')}" for field in RAW_COUNTERS],
    *[f"expected_{field}" for field in RATE_FIELDS],
]


def empty_row(role: str, provider: str, sample_index: int) -> dict:
    monotonic_nano = (sample_index + 1) * INTERVAL_NANO
    row = {column: "" for column in CSV_COLUMNS}
    row.update(
        {
            "role": role,
            "provider": provider,
            "sample_index": sample_index,
            "observed_at_unix_nano": 1_800_000_000_000_000_000 + monotonic_nano,
            "sample_monotonic_nano": monotonic_nano,
            "expected_quality_flags": 1 if sample_index == 0 else 0,
            "expected_interval_nano": 0 if sample_index == 0 else INTERVAL_NANO,
        }
    )
    return row


def expected(row: dict, field: str, value: int) -> None:
    row[f"expected_{field.replace('_total', '_delta')}"] = value


def generate_rows() -> list:
    sender = {
        "sent_packet_total": 1_000,
        "unique_sent_packet_total": 1_000,
        "sender_loss_packet_total": 0,
        "retransmitted_packet_total": 0,
        "sent_wire_byte_total": 1_000 * PACKET_WIRE_BYTES,
        "unique_sent_wire_byte_total": 1_000 * PACKET_WIRE_BYTES,
        "retransmitted_wire_byte_total": 0,
    }
    receiver = {
        "received_packet_total": 1_000,
        "unique_received_packet_total": 1_000,
        "receiver_loss_packet_total": 0,
        "received_wire_byte_total": 1_000 * PACKET_WIRE_BYTES,
        "unique_received_wire_byte_total": 1_000 * PACKET_WIRE_BYTES,
    }
    rows = []
    for sample_index in range(LAST_SAMPLE + 1):
        active = FIRST_ACTIVE_SAMPLE <= sample_index <= LAST_ACTIVE_SAMPLE
        retransmitted = 5 if active else 0
        if sample_index > 0:
            sender["unique_sent_packet_total"] += 100
            sender["sent_packet_total"] += 100 + retransmitted
            sender["sender_loss_packet_total"] += retransmitted
            sender["retransmitted_packet_total"] += retransmitted
            sender["unique_sent_wire_byte_total"] += 100 * PACKET_WIRE_BYTES
            sender["sent_wire_byte_total"] += (100 + retransmitted) * PACKET_WIRE_BYTES
            sender["retransmitted_wire_byte_total"] += retransmitted * PACKET_WIRE_BYTES

            receiver["received_packet_total"] += 100
            receiver["unique_received_packet_total"] += 100
            receiver["receiver_loss_packet_total"] += retransmitted
            receiver["received_wire_byte_total"] += 100 * PACKET_WIRE_BYTES
            receiver["unique_received_wire_byte_total"] += 100 * PACKET_WIRE_BYTES

        sender_row = empty_row("sender", "robotweax-srt", sample_index)
        sender_row.update(sender)
        sender_row["rtt_seconds"] = "0.030" if active else "0.020"
        receiver_row = empty_row("receiver", "haivision-srt", sample_index)
        receiver_row.update(receiver)
        receiver_row["rtt_seconds"] = "0.030" if active else "0.020"

        if sample_index > 0:
            expected(sender_row, "sent_packet_total", 100 + retransmitted)
            expected(sender_row, "unique_sent_packet_total", 100)
            expected(sender_row, "sender_loss_packet_total", retransmitted)
            expected(sender_row, "retransmitted_packet_total", retransmitted)
            expected(sender_row, "sent_wire_byte_total", (100 + retransmitted) * PACKET_WIRE_BYTES)
            expected(sender_row, "unique_sent_wire_byte_total", 100 * PACKET_WIRE_BYTES)
            expected(sender_row, "retransmitted_wire_byte_total", retransmitted * PACKET_WIRE_BYTES)
            sender_row["expected_sent_wire_bits_per_second"] = (
                (100 + retransmitted) * PACKET_WIRE_BYTES * 8
            )
            sender_row["expected_unique_sent_wire_bits_per_second"] = (
                100 * PACKET_WIRE_BYTES * 8
            )

            expected(receiver_row, "received_packet_total", 100)
            expected(receiver_row, "unique_received_packet_total", 100)
            expected(receiver_row, "receiver_loss_packet_total", retransmitted)
            expected(receiver_row, "received_wire_byte_total", 100 * PACKET_WIRE_BYTES)
            expected(receiver_row, "unique_received_wire_byte_total", 100 * PACKET_WIRE_BYTES)
            receiver_row["expected_received_wire_bits_per_second"] = (
                100 * PACKET_WIRE_BYTES * 8
            )
            receiver_row["expected_unique_received_wire_bits_per_second"] = (
                100 * PACKET_WIRE_BYTES * 8
            )

        rows.extend([sender_row, receiver_row])
    return rows


def csv_bytes(rows: list) -> bytes:
    output = io.StringIO(newline="")
    writer = csv.DictWriter(output, fieldnames=CSV_COLUMNS, lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)
    return output.getvalue().encode("utf-8")


def manifest_bytes(samples: bytes, row_count: int) -> bytes:
    manifest = {
        "schemaVersion": "1alpha1",
        "incidentId": INCIDENT_ID,
        "failureClass": "constant-packet-loss",
        "topology": "one sender connected directly to one receiver",
        "direction": "sender-to-receiver",
        "providers": [
            {
                "role": "sender",
                "provider": "robotweax-srt",
                "version": "0.2.0-4-g271f832",
                "revision": "271f832ef4bb96fab13777a313a0af9d7e20af7e",
                "buildProfile": "clean-release-shared-openssl",
            },
            {
                "role": "receiver",
                "provider": "haivision-srt",
                "version": "1.5.5",
                "revision": "b6b4ae990daa8193625a4ddeaeaed03023b23125",
                "buildProfile": "release-shared-openssl-generated-install",
            },
        ],
        "transportProfile": {
            "mode": "live",
            "api": "message",
            "addressFamily": "ipv4",
            "encryption": "none",
            "fec": "none",
            "group": "none",
        },
        "faultPlan": {
            "seed": 104729,
            "description": "Five original DATA packets per second are lost and recovered by retransmission during samples 3 through 6.",
            "firstActiveSample": FIRST_ACTIVE_SAMPLE,
            "lastActiveSample": LAST_ACTIVE_SAMPLE,
        },
        "provenance": {
            "kind": "synthetic-contract-fixture",
            "capturedFromProvider": False,
            "generator": "generate.py",
            "telemetryVersion": "0.1.0-alpha.0",
            "description": "Deterministic semantic fixture; it is not provider-runtime qualification evidence.",
        },
        "dataset": {
            "format": "robotweax-srt-telemetry-csv-v1",
            "samplesFile": "samples.csv",
            "sha256": hashlib.sha256(samples).hexdigest(),
            "rows": row_count,
            "roles": ["sender", "receiver"],
            "sampleIntervalNano": INTERVAL_NANO,
        },
        "expectedEvidence": [
            "receiver loss is observed only at the receiver and retains its observation side",
            "sender loss and retransmission rise together during the active fault window",
            "physical sent packets equal unique sent packets plus retransmissions during loss",
            "drop and undecrypt counters remain unavailable rather than synthetic zero",
            "loss and retransmission deltas return to zero after the fault window",
        ],
        "machineExpectations": [
            {
                "id": "receiver-loss-active",
                "role": "receiver",
                "field": "receiver_loss_packet_delta",
                "operator": "equals",
                "value": 5,
                "sampleRange": {"first": FIRST_ACTIVE_SAMPLE, "last": LAST_ACTIVE_SAMPLE},
            },
            {
                "id": "sender-loss-active",
                "role": "sender",
                "field": "sender_loss_packet_delta",
                "operator": "equals",
                "value": 5,
                "sampleRange": {"first": FIRST_ACTIVE_SAMPLE, "last": LAST_ACTIVE_SAMPLE},
            },
            {
                "id": "retransmission-active",
                "role": "sender",
                "field": "retransmitted_packet_delta",
                "operator": "equals",
                "value": 5,
                "sampleRange": {"first": FIRST_ACTIVE_SAMPLE, "last": LAST_ACTIVE_SAMPLE},
            },
            {
                "id": "physical-includes-retransmission",
                "role": "sender",
                "field": "sent_packet_delta",
                "operator": "equals-field-plus",
                "comparisonField": "unique_sent_packet_delta",
                "value": 5,
                "sampleRange": {"first": FIRST_ACTIVE_SAMPLE, "last": LAST_ACTIVE_SAMPLE},
            },
            {
                "id": "receiver-loss-recovered",
                "role": "receiver",
                "field": "receiver_loss_packet_delta",
                "operator": "equals",
                "value": 0,
                "sampleRange": {"first": LAST_ACTIVE_SAMPLE + 1, "last": LAST_SAMPLE},
            },
            {
                "id": "receiver-drop-unavailable",
                "role": "receiver",
                "field": "receiver_drop_packet_delta",
                "operator": "unavailable",
                "sampleRange": {"first": 1, "last": LAST_SAMPLE},
            },
            {
                "id": "undecrypt-unavailable",
                "role": "receiver",
                "field": "receiver_undecryptable_packet_delta",
                "operator": "unavailable",
                "sampleRange": {"first": 1, "last": LAST_SAMPLE},
            },
        ],
        "contradictions": [
            "The fixture does not establish the location or cause of real network loss.",
            "Unavailable drop and undecrypt counters cannot support a zero-drop or no-decrypt-failure claim.",
            "Synthetic wall-clock alignment does not qualify real endpoint clock synchronization.",
        ],
    }
    return (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=f"Generate or check {INCIDENT_ID}")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    rows = generate_rows()
    samples = csv_bytes(rows)
    manifest = manifest_bytes(samples, len(rows))
    expected_files = {"samples.csv": samples, "manifest.json": manifest}
    if arguments.check:
        for name, expected_contents in expected_files.items():
            path = arguments.output / name
            if not path.is_file() or path.read_bytes() != expected_contents:
                raise RuntimeError(f"generated artifact differs: {path}")
        print(f"{INCIDENT_ID} is reproducible")
        return 0
    arguments.output.mkdir(parents=True, exist_ok=True)
    for name, contents in expected_files.items():
        (arguments.output / name).write_bytes(contents)
    print(f"generated {INCIDENT_ID} in {arguments.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"generation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
