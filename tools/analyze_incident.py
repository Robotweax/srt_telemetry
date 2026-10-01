#!/usr/bin/env python3
# FROZEN (2026-10-01): reference implementation for SRT Doctor.
# SRT Telemetry's roadmap is the sender/receiver dashboard path (docs/roadmap.md).
# Cross-endpoint diagnosis now belongs to SRT Doctor; only bug fixes that keep
# existing tests and downstream consumers (e.g. Flight Recorder) working land here.

import argparse
import csv
import hashlib
import json
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple


QUALITY_FLAGS = {
    1 << 0: "first-sample",
    1 << 1: "monotonic-clock-regression",
    1 << 2: "provider-changed",
    1 << 3: "counter-reset",
}

DELTA_FIELDS = [
    "sent_packet_delta",
    "received_packet_delta",
    "unique_sent_packet_delta",
    "unique_received_packet_delta",
    "sender_loss_packet_delta",
    "receiver_loss_packet_delta",
    "retransmitted_packet_delta",
    "sender_drop_packet_delta",
    "receiver_drop_packet_delta",
    "receiver_undecryptable_packet_delta",
]

RAW_FIELDS = [
    "rtt_seconds",
    "flight_size_packets",
    "flow_window_packets",
    "congestion_window_packets",
    "available_send_buffer_bytes",
    "available_receive_buffer_bytes",
    "send_buffer_bytes",
    "send_buffer_seconds",
    "receive_buffer_bytes",
    "receive_buffer_seconds",
    "sender_tsbpd_delay_seconds",
    "receiver_tsbpd_delay_seconds",
]


def fail(message: str) -> None:
    raise RuntimeError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_optional_number(value: Optional[str], name: str) -> Optional[float]:
    if value is None or value == "":
        return None
    try:
        if any(marker in value for marker in [".", "e", "E"]):
            return float(value)
        return int(value)
    except ValueError as error:
        raise RuntimeError(f"invalid numeric field {name}: {value!r}") from error


def validate_streams(streams: Dict[str, dict]) -> None:
    if set(streams) != {"sender", "receiver"}:
        fail(
            f"analysis requires exactly sender and receiver streams: {sorted(streams)}"
        )
    indexes_by_role = {}
    for role, stream in streams.items():
        samples = stream["samples"]
        indexes = [sample["sampleIndex"] for sample in samples]
        if indexes != list(range(len(samples))):
            fail(f"{role} sample indexes are not contiguous from zero: {indexes}")
        indexes_by_role[role] = indexes
    if indexes_by_role["sender"] != indexes_by_role["receiver"]:
        fail("sender and receiver sample indexes are not aligned")
    if not indexes_by_role["sender"]:
        fail("analysis requires at least one aligned sample")


def load_manifest(manifest_path: Path) -> dict:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or manifest.get("schemaVersion") != "1alpha1":
        fail("unsupported golden-incident manifest")
    dataset = manifest.get("dataset")
    if (
        not isinstance(dataset, dict)
        or dataset.get("format") != "robotweax-srt-telemetry-csv-v1"
    ):
        fail("manifest does not reference the supported telemetry CSV format")
    samples_path = (manifest_path.parent / dataset["samplesFile"]).resolve()
    if (
        samples_path.parent != manifest_path.parent.resolve()
        or not samples_path.is_file()
    ):
        fail("manifest sample dataset is outside the incident directory or missing")
    if sha256_file(samples_path) != dataset["sha256"]:
        fail("golden-incident sample digest mismatch")
    providers = {provider["role"]: provider for provider in manifest["providers"]}
    streams = {
        role: {
            "header": {
                "streamId": f"{manifest['incidentId']}-{role}",
                "flowId": manifest["incidentId"],
                "role": role,
                "configuredProvider": providers[role]["provider"],
                "providerVersion": providers[role]["version"],
                "providerRevision": providers[role]["revision"],
                "buildProfile": providers[role]["buildProfile"],
                "recordCount": dataset["rows"] // len(dataset["roles"]),
                "capacity": dataset["rows"] // len(dataset["roles"]),
                "overwrittenCount": 0,
            },
            "samples": [],
        }
        for role in dataset["roles"]
    }
    with samples_path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        for row_number, row in enumerate(reader, start=2):
            role = row.get("role")
            if role not in streams:
                fail(f"undeclared role at CSV row {row_number}: {role!r}")
            if row.get("provider") != providers[role]["provider"]:
                fail(f"provider mismatch at CSV row {row_number}")
            raw = {
                "provider": row["provider"],
                "observed_at_unix_nano": int(row["observed_at_unix_nano"]),
                "sample_monotonic_nano": int(row["sample_monotonic_nano"]),
            }
            for field in RAW_FIELDS:
                raw[field] = parse_optional_number(
                    row.get(field), f"row {row_number}.{field}"
                )
            delta = {
                "quality_flags": int(row["expected_quality_flags"]),
                "interval_nano": int(row["expected_interval_nano"]),
            }
            for field in DELTA_FIELDS:
                delta[field] = parse_optional_number(
                    row.get(f"expected_{field}"), f"row {row_number}.expected_{field}"
                )
            streams[role]["samples"].append(
                {
                    "sampleIndex": int(row["sample_index"]),
                    "raw": raw,
                    "delta": delta,
                }
            )
    if sum(len(stream["samples"]) for stream in streams.values()) != dataset["rows"]:
        fail("manifest row count does not match the CSV dataset")
    validate_streams(streams)
    return {
        "format": "robotweax-srt-telemetry-csv-v1",
        "incidentId": manifest["incidentId"],
        "failureClass": manifest["failureClass"],
        "flowId": manifest["incidentId"],
        "artifacts": [
            {"path": manifest_path.name, "sha256": sha256_file(manifest_path)},
            {"path": samples_path.name, "sha256": dataset["sha256"]},
        ],
        "streams": streams,
        "nonClaims": list(manifest.get("contradictions", [])),
    }


def load_jsonl_file(path: Path) -> Tuple[dict, List[dict]]:
    documents = []
    with path.open(encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            if not line.endswith("\n"):
                fail(f"JSONL line has no newline terminator: {path}:{line_number}")
            try:
                document = json.loads(line)
            except json.JSONDecodeError as error:
                raise RuntimeError(
                    f"invalid JSONL at {path}:{line_number}: {error}"
                ) from error
            if (
                not isinstance(document, dict)
                or document.get("schemaVersion") != "1alpha1"
            ):
                fail(f"unsupported JSONL document at {path}:{line_number}")
            documents.append(document)
    if not documents:
        fail(f"empty JSONL recording: {path}")
    header = documents[0]
    if header.get("kind") != "robotweax-srt-telemetry-recording":
        fail(f"first JSONL line is not a recording header: {path}")
    required_header = {
        "streamId",
        "flowId",
        "role",
        "configuredProvider",
        "providerVersion",
        "providerRevision",
        "buildProfile",
        "telemetryVersion",
        "recordCount",
        "capacity",
        "overwrittenCount",
    }
    missing_header = required_header - set(header)
    if missing_header:
        fail(f"JSONL header is missing fields {sorted(missing_header)}: {path}")
    if header["telemetryVersion"] != "0.1.0-alpha.0":
        fail(f"unsupported telemetry JSONL version: {header['telemetryVersion']}")
    if header["configuredProvider"] not in {"robotweax-srt", "haivision-srt"}:
        fail(f"unsupported configured provider: {path}")
    for field in [
        "streamId",
        "flowId",
        "providerVersion",
        "providerRevision",
        "buildProfile",
    ]:
        if not isinstance(header[field], str) or not header[field]:
            fail(f"empty JSONL header field {field}: {path}")
    samples = documents[1:]
    for index, sample in enumerate(samples):
        if sample.get("kind") != "robotweax-srt-telemetry-sample":
            fail(f"unexpected JSONL document kind at sample {index}: {path}")
        if sample.get("streamId") != header.get("streamId"):
            fail(f"JSONL stream identity changed at sample {index}: {path}")
        if sample.get("sampleIndex") != index:
            fail(f"JSONL sample indexes are not contiguous: {path}")
        if not isinstance(sample.get("raw"), dict) or not isinstance(
            sample.get("delta"), dict
        ):
            fail(f"JSONL sample is missing raw or delta data: {path}:{index + 2}")
        if sample["raw"].get("provider") not in {
            "unknown",
            "robotweax-srt",
            "haivision-srt",
        }:
            fail(f"JSONL sample contains an invalid provider: {path}:{index + 2}")
        if not isinstance(sample["delta"].get("quality_flags"), int):
            fail(f"JSONL sample contains invalid quality flags: {path}:{index + 2}")
    if header.get("recordCount") != len(samples):
        fail(f"JSONL record count mismatch: {path}")
    if (
        not isinstance(header["capacity"], int)
        or header["capacity"] <= 0
        or header["capacity"] < len(samples)
    ):
        fail(f"JSONL capacity is invalid: {path}")
    return header, samples


def load_jsonl(paths: List[Path]) -> dict:
    if len(paths) != 2:
        fail("JSONL analysis requires exactly two recording files")
    streams = {}
    artifacts = []
    non_claims = []
    flow_id = None
    for path in paths:
        header, samples = load_jsonl_file(path)
        role = header.get("role")
        if role not in {"sender", "receiver"} or role in streams:
            fail(f"invalid or duplicate JSONL role: {role!r}")
        if flow_id is None:
            flow_id = header.get("flowId")
        elif header.get("flowId") != flow_id:
            fail("JSONL recordings do not share one flowId")
        overwritten = header.get("overwrittenCount")
        if not isinstance(overwritten, int) or overwritten < 0:
            fail(f"invalid JSONL overwrite accounting: {path}")
        if overwritten > 0:
            non_claims.append(
                f"{role} recorder overwrote {overwritten} pre-trigger samples "
                "before export."
            )
        streams[role] = {"header": header, "samples": samples}
        artifacts.append({"path": path.name, "sha256": sha256_file(path)})
    if not isinstance(flow_id, str) or not flow_id:
        fail("JSONL recordings have no shared flowId")
    validate_streams(streams)
    return {
        "format": "robotweax-srt-telemetry-jsonl-v1",
        "incidentId": None,
        "failureClass": None,
        "flowId": flow_id,
        "artifacts": artifacts,
        "streams": streams,
        "nonClaims": non_claims,
    }


def numeric(value: object) -> Optional[float]:
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return value
    return None


def positive(value: object) -> bool:
    number = numeric(value)
    return number is not None and number > 0


def sum_available(samples: List[dict], field: str) -> Tuple[float, int]:
    values = [numeric(sample["delta"].get(field)) for sample in samples]
    available = [value for value in values if value is not None]
    return sum(available), len(available)


def rtt_baseline(samples: List[dict]) -> Optional[float]:
    warm_values = [
        numeric(sample["raw"].get("rtt_seconds"))
        for sample in samples
        if sample["sampleIndex"] in {1, 2}
    ]
    available = [value for value in warm_values if value is not None]
    return min(available) if available else None


def rtt_step_threshold(baseline: Optional[float]) -> Optional[float]:
    if baseline is None:
        return None
    return max(baseline * 2.0, baseline + 0.020)


def warm_max_baseline(samples: List[dict], field: str) -> Optional[float]:
    warm_values = [
        numeric(sample["raw"].get(field))
        for sample in samples
        if sample["sampleIndex"] in {1, 2}
    ]
    available = [value for value in warm_values if value is not None]
    return max(available) if available else None


def pressure_step_threshold(
    baseline: Optional[float], absolute_step: float
) -> Optional[float]:
    if baseline is None:
        return None
    return max(baseline * 2.0, baseline + absolute_step)


def at_or_above(value: object, threshold: Optional[float]) -> bool:
    number = numeric(value)
    return number is not None and threshold is not None and number >= threshold


def evidence(id_value: str, summary: str, samples: List[int]) -> dict:
    return {"id": id_value, "summary": summary, "samples": samples}


def build_analysis(source: dict) -> dict:
    streams = source["streams"]
    sender_samples = streams["sender"]["samples"]
    receiver_samples = streams["receiver"]["samples"]
    sender_rtt_baseline = rtt_baseline(sender_samples)
    receiver_rtt_baseline = rtt_baseline(receiver_samples)
    sender_rtt_threshold = rtt_step_threshold(sender_rtt_baseline)
    receiver_rtt_threshold = rtt_step_threshold(receiver_rtt_baseline)
    sender_buffer_baseline = warm_max_baseline(sender_samples, "send_buffer_bytes")
    sender_flight_baseline = warm_max_baseline(sender_samples, "flight_size_packets")
    receiver_buffer_baseline = warm_max_baseline(
        receiver_samples, "receive_buffer_bytes"
    )
    sender_buffer_threshold = pressure_step_threshold(sender_buffer_baseline, 65_536)
    sender_flight_threshold = pressure_step_threshold(sender_flight_baseline, 8)
    receiver_buffer_threshold = pressure_step_threshold(
        receiver_buffer_baseline, 1_048_576
    )
    observations = []
    deviation_indexes = []
    quality_boundaries = []
    for sender, receiver in zip(sender_samples, receiver_samples):
        index = sender["sampleIndex"]
        sender_delta = sender["delta"]
        receiver_delta = receiver["delta"]
        signals = {
            "senderLossPackets": numeric(sender_delta.get("sender_loss_packet_delta")),
            "receiverLossPackets": numeric(
                receiver_delta.get("receiver_loss_packet_delta")
            ),
            "retransmittedPackets": numeric(
                sender_delta.get("retransmitted_packet_delta")
            ),
            "senderDropPackets": numeric(sender_delta.get("sender_drop_packet_delta")),
            "receiverDropPackets": numeric(
                receiver_delta.get("receiver_drop_packet_delta")
            ),
            "receiverUndecryptablePackets": numeric(
                receiver_delta.get("receiver_undecryptable_packet_delta")
            ),
            "physicalSentPackets": numeric(sender_delta.get("sent_packet_delta")),
            "uniqueSentPackets": numeric(sender_delta.get("unique_sent_packet_delta")),
            "uniqueReceivedPackets": numeric(
                receiver_delta.get("unique_received_packet_delta")
            ),
            "senderRttSeconds": numeric(sender["raw"].get("rtt_seconds")),
            "receiverRttSeconds": numeric(receiver["raw"].get("rtt_seconds")),
            "senderFlightSizePackets": numeric(
                sender["raw"].get("flight_size_packets")
            ),
            "senderSendBufferBytes": numeric(
                sender["raw"].get("send_buffer_bytes")
            ),
            "senderSendBufferSeconds": numeric(
                sender["raw"].get("send_buffer_seconds")
            ),
            "receiverReceiveBufferBytes": numeric(
                receiver["raw"].get("receive_buffer_bytes")
            ),
            "receiverReceiveBufferSeconds": numeric(
                receiver["raw"].get("receive_buffer_seconds")
            ),
        }
        counter_deviating = any(
            positive(signals[field])
            for field in [
                "senderLossPackets",
                "receiverLossPackets",
                "retransmittedPackets",
                "senderDropPackets",
                "receiverDropPackets",
                "receiverUndecryptablePackets",
            ]
        )
        rtt_deviating = index >= 3 and (
            at_or_above(signals["senderRttSeconds"], sender_rtt_threshold)
            or at_or_above(signals["receiverRttSeconds"], receiver_rtt_threshold)
        )
        pressure_deviating = index >= 3 and (
            at_or_above(
                signals["senderSendBufferBytes"], sender_buffer_threshold
            )
            or at_or_above(
                signals["senderFlightSizePackets"], sender_flight_threshold
            )
            or at_or_above(
                signals["receiverReceiveBufferBytes"], receiver_buffer_threshold
            )
        )
        deviating = counter_deviating or rtt_deviating or pressure_deviating
        if deviating:
            deviation_indexes.append(index)
        boundaries = []
        for role, sample in [("sender", sender), ("receiver", receiver)]:
            flags = sample["delta"].get("quality_flags", 0)
            if not isinstance(flags, int) or flags < 0:
                fail(f"invalid quality flags for {role} sample {index}")
            names = [name for flag, name in QUALITY_FLAGS.items() if flags & flag]
            if names:
                boundaries.append({"role": role, "flags": names})
                quality_boundaries.append(
                    {"sampleIndex": index, "role": role, "flags": names}
                )
        observations.append(
            {
                "sampleIndex": index,
                "observedAtUnixNano": {
                    "sender": sender["raw"].get("observed_at_unix_nano"),
                    "receiver": receiver["raw"].get("observed_at_unix_nano"),
                },
                "deviating": deviating,
                "signals": signals,
                "qualityBoundaries": boundaries,
            }
        )

    first_deviation = deviation_indexes[0] if deviation_indexes else None
    last_deviation = deviation_indexes[-1] if deviation_indexes else None
    recovery_sample = None
    if last_deviation is not None:
        for observation in observations[last_deviation + 1 :]:
            if not observation["deviating"]:
                recovery_sample = observation["sampleIndex"]
                break

    sender_loss_total, sender_loss_available = sum_available(
        sender_samples, "sender_loss_packet_delta"
    )
    receiver_loss_total, receiver_loss_available = sum_available(
        receiver_samples, "receiver_loss_packet_delta"
    )
    retransmission_total, retransmission_available = sum_available(
        sender_samples, "retransmitted_packet_delta"
    )
    sender_drop_total, sender_drop_available = sum_available(
        sender_samples, "sender_drop_packet_delta"
    )
    receiver_drop_total, receiver_drop_available = sum_available(
        receiver_samples, "receiver_drop_packet_delta"
    )
    undecryptable_total, undecryptable_available = sum_available(
        receiver_samples, "receiver_undecryptable_packet_delta"
    )
    physical_total, _ = sum_available(sender_samples, "sent_packet_delta")
    unique_sent_total, _ = sum_available(sender_samples, "unique_sent_packet_delta")
    unique_received_total, _ = sum_available(
        receiver_samples, "unique_received_packet_delta"
    )

    path_support = []
    path_contradictions = []
    path_missing = []
    paired_loss_indexes = [
        sender["sampleIndex"]
        for sender, receiver in zip(sender_samples, receiver_samples)
        if positive(sender["delta"].get("sender_loss_packet_delta"))
        and positive(receiver["delta"].get("receiver_loss_packet_delta"))
    ]
    if paired_loss_indexes:
        path_support.append(
            evidence(
                "paired-loss",
                (
                    "Sender-learned and receiver-observed loss are positive in "
                    "aligned samples."
                ),
                paired_loss_indexes,
            )
        )
    if retransmission_total > 0 and physical_total > unique_sent_total:
        path_support.append(
            evidence(
                "recovery-traffic",
                "Retransmissions explain physical sends above unique sends.",
                deviation_indexes,
            )
        )
    if recovery_sample is not None:
        path_support.append(
            evidence(
                "recovery-boundary",
                "Loss and retransmission return to zero after the deviation window.",
                [recovery_sample],
            )
        )
    if sender_drop_total > 0:
        path_contradictions.append(
            "Local sender-buffer removal counters are positive during the incident; "
            "they do not by themselves establish network or application payload loss."
        )
    if receiver_drop_total > 0:
        path_contradictions.append(
            "Receiver drop counters are positive during the incident."
        )
    if undecryptable_total > 0:
        path_contradictions.append("Receiver undecryptable counters are positive.")
    if sender_loss_available == 0 or receiver_loss_available == 0:
        path_missing.append(
            "Paired sender and receiver loss deltas are not both available."
        )
    elif sender_loss_total > 0 and receiver_loss_total > 0 and not paired_loss_indexes:
        path_missing.append(
            "Positive sender and receiver loss do not overlap in aligned samples."
        )
    if retransmission_available == 0:
        path_missing.append("Sender retransmission deltas are unavailable.")
    path_missing.append(
        "Packet sequence adjacency and the exact failing network hop are unavailable."
    )
    path_supported = (
        len(path_support) >= 2
        and bool(paired_loss_indexes)
        and retransmission_total > 0
        and not path_contradictions
    )

    paired_rtt_indexes = [
        sender["sampleIndex"]
        for sender, receiver in zip(sender_samples, receiver_samples)
        if sender["sampleIndex"] >= 3
        and at_or_above(sender["raw"].get("rtt_seconds"), sender_rtt_threshold)
        and at_or_above(receiver["raw"].get("rtt_seconds"), receiver_rtt_threshold)
    ]
    rtt_support = []
    rtt_contradictions = []
    rtt_missing = []
    if paired_rtt_indexes:
        rtt_support.append(
            evidence(
                "paired-rtt-step",
                (
                    "Sender and receiver RTT exceed their warm candidate baselines "
                    "by both the relative and absolute step thresholds."
                ),
                paired_rtt_indexes,
            )
        )
    if recovery_sample is not None and paired_rtt_indexes:
        rtt_support.append(
            evidence(
                "rtt-recovery-boundary",
                "Paired RTT returns below the candidate step thresholds.",
                [recovery_sample],
            )
        )
    if sender_loss_total > 0 or receiver_loss_total > 0:
        rtt_contradictions.append(
            "Sender-learned or receiver-observed loss counters are positive."
        )
    if retransmission_total > 0 and sender_loss_total == 0 and receiver_loss_total == 0:
        rtt_support.append(
            evidence(
                "retransmission-without-observed-loss",
                (
                    "Sender retransmissions occur without paired sender-learned or "
                    "receiver-observed loss."
                ),
                [
                    sample["sampleIndex"]
                    for sample in sender_samples
                    if positive(sample["delta"].get("retransmitted_packet_delta"))
                ],
            )
        )
    if sender_drop_total > 0 or receiver_drop_total > 0 or undecryptable_total > 0:
        rtt_contradictions.append(
            "Endpoint drop or undecryptable counters are positive during the incident."
        )
    if sender_rtt_baseline is None or receiver_rtt_baseline is None:
        rtt_missing.append(
            "Paired warm sender and receiver RTT baselines are unavailable."
        )
    elif not paired_rtt_indexes:
        rtt_missing.append("Paired RTT does not cross the candidate step thresholds.")
    rtt_missing.append(
        "One-way delay and the exact failing network hop are unavailable."
    )
    rtt_supported = (
        len(paired_rtt_indexes) >= 2
        and recovery_sample is not None
        and not rtt_contradictions
    )

    sender_support = []
    sender_contradictions = []
    sender_missing = []
    if sender_drop_total > 0:
        sender_support.append(
            evidence(
                "sender-drop",
                "Sender counters record local send/retransmission-buffer removals "
                "due to message TTL or TLPKTDROP; already submitted UDP datagrams "
                "may still arrive at the receiver.",
                [
                    sample["sampleIndex"]
                    for sample in sender_samples
                    if positive(sample["delta"].get("sender_drop_packet_delta"))
                ],
            )
        )
    if sender_drop_available == 0:
        sender_missing.append("Sender drop deltas are unavailable.")
    if all(sample["raw"].get("send_buffer_bytes") is None for sample in sender_samples):
        sender_missing.append("Sender buffer occupancy is unavailable.")
    if all(
        sample["raw"].get("flight_size_packets") is None for sample in sender_samples
    ):
        sender_missing.append("Sender flight-size evidence is unavailable.")
    sender_buffer_indexes = [
        sample["sampleIndex"]
        for sample in sender_samples
        if sample["sampleIndex"] >= 3
        and at_or_above(
            sample["raw"].get("send_buffer_bytes"), sender_buffer_threshold
        )
    ]
    sender_flight_indexes = [
        sample["sampleIndex"]
        for sample in sender_samples
        if sample["sampleIndex"] >= 3
        and at_or_above(
            sample["raw"].get("flight_size_packets"), sender_flight_threshold
        )
    ]
    sender_overlap_indexes = sorted(
        set(sender_buffer_indexes) & set(sender_flight_indexes)
    )
    receiver_normal_indexes = [
        sample["sampleIndex"]
        for sample in receiver_samples
        if sample["sampleIndex"] in sender_overlap_indexes
        and numeric(sample["raw"].get("receive_buffer_bytes")) is not None
        and receiver_buffer_threshold is not None
        and numeric(sample["raw"].get("receive_buffer_bytes"))
        < receiver_buffer_threshold
    ]
    sender_pressure_recovery_sample = None
    if sender_buffer_indexes:
        for sender, receiver in zip(
            sender_samples[sender_buffer_indexes[-1] + 1 :],
            receiver_samples[sender_buffer_indexes[-1] + 1 :],
        ):
            sender_buffer = numeric(sender["raw"].get("send_buffer_bytes"))
            sender_flight = numeric(sender["raw"].get("flight_size_packets"))
            receiver_buffer = numeric(receiver["raw"].get("receive_buffer_bytes"))
            if (
                sender_buffer is not None
                and sender_buffer_threshold is not None
                and sender_buffer < sender_buffer_threshold
                and sender_flight is not None
                and sender_flight_threshold is not None
                and sender_flight < sender_flight_threshold
                and receiver_buffer is not None
                and receiver_buffer_threshold is not None
                and receiver_buffer < receiver_buffer_threshold
            ):
                sender_pressure_recovery_sample = sender["sampleIndex"]
                break
        sender_support.append(
            evidence(
                "sender-buffer-growth",
                "Sender buffer occupancy exceeds its warm candidate threshold.",
                sender_buffer_indexes,
            )
        )
    if sender_overlap_indexes:
        sender_support.append(
            evidence(
                "sender-flight-growth",
                "Sender flight size rises together with sender buffer occupancy.",
                sender_overlap_indexes,
            )
        )
    if receiver_normal_indexes:
        sender_support.append(
            evidence(
                "receiver-buffer-remains-below-pressure-threshold",
                (
                    "Receiver buffer occupancy remains below its conservative "
                    "pressure threshold during the sender buffer/flight overlap."
                ),
                receiver_normal_indexes,
            )
        )
    if sender_pressure_recovery_sample is not None:
        sender_support.append(
            evidence(
                "sender-pressure-recovery",
                "Sender buffer and flight size return below their thresholds.",
                [sender_pressure_recovery_sample],
            )
        )
    if sender_buffer_baseline is not None and not sender_buffer_indexes:
        sender_missing.append(
            "Sender buffer occupancy does not cross the candidate threshold."
        )
    if sender_flight_baseline is not None and not sender_flight_indexes:
        sender_missing.append(
            "Sender flight size does not cross the candidate threshold."
        )
    if sender_buffer_indexes and sender_pressure_recovery_sample is None:
        sender_missing.append("Sender pressure recovery is not observed.")
    sender_missing.append("Sender application scheduling is unavailable.")
    sender_missing.append(
        "Offered application rate and the exact egress capacity restriction are unavailable."
    )
    sender_pressure_supported = (
        len(sender_buffer_indexes) >= 2
        and bool(sender_overlap_indexes)
        and receiver_normal_indexes == sender_overlap_indexes
        and sender_pressure_recovery_sample is not None
    )
    sender_hypothesis_supported = sender_drop_total > 0 or sender_pressure_supported

    receiver_support = []
    receiver_contradictions = []
    receiver_missing = []
    if receiver_drop_total > 0:
        receiver_support.append(
            evidence(
                "receiver-drop",
                "Receiver drop counters are positive.",
                [
                    sample["sampleIndex"]
                    for sample in receiver_samples
                    if positive(sample["delta"].get("receiver_drop_packet_delta"))
                ],
            )
        )
    if receiver_drop_available == 0:
        receiver_missing.append("Receiver drop deltas are unavailable.")
    receiver_buffer_indexes = [
        sample["sampleIndex"]
        for sample in receiver_samples
        if sample["sampleIndex"] >= 3
        and at_or_above(
            sample["raw"].get("receive_buffer_bytes"), receiver_buffer_threshold
        )
    ]
    upstream_pressure_indexes = sorted(
        {
            sample["sampleIndex"]
            for sample in sender_samples
            if sample["sampleIndex"] >= 3
            and (
                at_or_above(
                    sample["raw"].get("send_buffer_bytes"), sender_buffer_threshold
                )
                or at_or_above(
                    sample["raw"].get("flight_size_packets"), sender_flight_threshold
                )
            )
        }
    )
    pressure_recovery_sample = None
    if receiver_buffer_indexes:
        for sample in receiver_samples[receiver_buffer_indexes[-1] + 1 :]:
            value = numeric(sample["raw"].get("receive_buffer_bytes"))
            if (
                value is not None
                and receiver_buffer_threshold is not None
                and value < receiver_buffer_threshold
            ):
                pressure_recovery_sample = sample["sampleIndex"]
                break
        receiver_support.append(
            evidence(
                "receiver-buffer-growth",
                (
                    "Receiver buffer occupancy exceeds its warm candidate "
                    "baseline threshold."
                ),
                receiver_buffer_indexes,
            )
        )
    overlap_indexes = sorted(
        set(receiver_buffer_indexes) & set(upstream_pressure_indexes)
    )
    if overlap_indexes:
        receiver_support.append(
            evidence(
                "upstream-backpressure",
                (
                    "Sender buffer or flight growth overlaps elevated receiver "
                    "buffer occupancy."
                ),
                overlap_indexes,
            )
        )
    if pressure_recovery_sample is not None:
        receiver_support.append(
            evidence(
                "receiver-buffer-recovery",
                "Receiver buffer occupancy returns below its candidate threshold.",
                [pressure_recovery_sample],
            )
        )
    if receiver_buffer_baseline is None:
        receiver_missing.append("Receiver buffer occupancy is unavailable.")
    elif not receiver_buffer_indexes:
        receiver_missing.append(
            "Receiver buffer occupancy does not cross the candidate threshold."
        )
    elif pressure_recovery_sample is None:
        receiver_missing.append("Receiver buffer recovery is not observed.")
    if undecryptable_available == 0:
        receiver_missing.append("Receiver undecryptable deltas are unavailable.")
    receiver_missing.append(
        "Receiver application-consumption timing is unavailable from public SRT statistics."
    )
    receiver_pressure_supported = (
        len(receiver_buffer_indexes) >= 2
        and bool(overlap_indexes)
        and pressure_recovery_sample is not None
    )
    receiver_hypothesis_supported = receiver_pressure_supported
    if receiver_pressure_supported:
        sender_contradictions.append(
            "Receiver buffer pressure provides a competing endpoint explanation."
        )
        sender_hypothesis_supported = sender_drop_total > 0
        rtt_contradictions.append(
            "Receiver buffer pressure provides a competing endpoint explanation."
        )
        rtt_supported = False
    if sender_pressure_supported:
        rtt_contradictions.append(
            "Sender egress pressure provides a competing endpoint explanation."
        )
        rtt_supported = False

    receiver_drop_indexes = [
        sample["sampleIndex"]
        for sample in receiver_samples
        if positive(sample["delta"].get("receiver_drop_packet_delta"))
    ]
    receiver_tsbpd_indexes = [
        sample["sampleIndex"]
        for sample in receiver_samples
        if positive(sample["raw"].get("receiver_tsbpd_delay_seconds"))
    ]
    receiver_tsbpd_values = sorted(
        {
            numeric(sample["raw"].get("receiver_tsbpd_delay_seconds"))
            for sample in receiver_samples
            if positive(sample["raw"].get("receiver_tsbpd_delay_seconds"))
        }
    )
    unique_delivery_gap = max(0, unique_sent_total - unique_received_total)
    receiver_drop_accounts_for_gap = (
        receiver_drop_total > 0
        and unique_sent_total >= unique_received_total
        and abs(unique_delivery_gap - receiver_drop_total) < 1e-9
    )
    loss_precedes_receiver_drop = (
        bool(paired_loss_indexes)
        and bool(receiver_drop_indexes)
        and paired_loss_indexes[0] <= receiver_drop_indexes[0]
        and receiver_drop_indexes[0] <= paired_loss_indexes[-1] + 1
    )
    recovery_margin_support = []
    recovery_margin_contradictions = []
    recovery_margin_missing = [
        (
            "Packet belated-arrival counters are unavailable under the "
            "non-destructive public statistics contract."
        ),
        "The counterfactual outcome under a larger configured latency is unavailable.",
        "The exact delay distribution and failing network hop are unavailable.",
    ]
    if loss_precedes_receiver_drop and retransmission_total > 0:
        recovery_margin_support.append(
            evidence(
                "loss-retransmission-before-receiver-drop",
                (
                    "Paired sender/receiver loss and sender retransmission precede "
                    "or coincide with receiver drops."
                ),
                sorted(set(paired_loss_indexes + receiver_drop_indexes)),
            )
        )
    if receiver_drop_accounts_for_gap:
        recovery_margin_support.append(
            evidence(
                "receiver-drop-delivery-gap",
                (
                    f"Receiver drops ({receiver_drop_total:g}) exactly account for "
                    f"the unique packet delivery gap ({unique_delivery_gap:g})."
                ),
                receiver_drop_indexes,
            )
        )
    if receiver_tsbpd_values:
        recovery_margin_support.append(
            evidence(
                "configured-receiver-tsbpd-delay",
                (
                    "A positive configured receiver TSBPD delay is available in "
                    "the public statistics."
                ),
                receiver_tsbpd_indexes,
            )
        )
    if recovery_sample is not None and receiver_drop_indexes:
        recovery_margin_support.append(
            evidence(
                "recovery-margin-recovery",
                (
                    "Loss, retransmission, and receiver-drop deltas return to zero "
                    "after the deviation."
                ),
                [recovery_sample],
            )
        )
    if undecryptable_total > 0:
        recovery_margin_contradictions.append(
            "Receiver undecryptable counters provide a competing security explanation."
        )
    if sender_pressure_supported:
        recovery_margin_contradictions.append(
            "Sustained sender egress pressure provides a competing endpoint explanation."
        )
    if receiver_pressure_supported:
        recovery_margin_contradictions.append(
            "Sustained receiver buffer pressure provides a competing endpoint explanation."
        )
    recovery_margin_supported = (
        loss_precedes_receiver_drop
        and retransmission_total > 0
        and receiver_drop_accounts_for_gap
        and bool(receiver_tsbpd_values)
        and recovery_sample is not None
        and not recovery_margin_contradictions
    )
    if recovery_margin_supported:
        rtt_contradictions.append(
            (
                "Receiver drops and an accounted delivery gap provide a competing "
                "recovery-margin explanation."
            )
        )
        rtt_supported = False

    hypotheses = [
        {
            "id": "transmission-path-loss-with-recovery",
            "failureDomain": "sender-to-receiver transmission path",
            "assessment": "supported" if path_supported else "insufficient-evidence",
            "summary": (
                "Paired loss and retransmission evidence localizes the deviation "
                "to the transmission path and shows recovery, but not the exact "
                "failing hop."
            ),
            "supportingEvidence": path_support,
            "contradictoryEvidence": path_contradictions,
            "missingEvidence": path_missing,
        },
        {
            "id": "transmission-path-rtt-step-with-recovery",
            "failureDomain": "sender-to-receiver transmission path",
            "assessment": "supported" if rtt_supported else "insufficient-evidence",
            "summary": (
                "Paired RTT step and recovery evidence localizes added round-trip "
                "delay to the transmission path, but not to one direction or hop."
            ),
            "supportingEvidence": rtt_support,
            "contradictoryEvidence": rtt_contradictions,
            "missingEvidence": rtt_missing,
        },
        {
            "id": "sender-side-pressure",
            "failureDomain": "sender endpoint",
            "assessment": (
                "supported"
                if sender_hypothesis_supported
                else "insufficient-evidence"
            ),
            "summary": (
                "Sender pressure requires drop, queue, or flight-window evidence."
            ),
            "supportingEvidence": sender_support,
            "contradictoryEvidence": sender_contradictions,
            "missingEvidence": sender_missing,
        },
        {
            "id": "receiver-side-pressure",
            "failureDomain": "receiver endpoint",
            "assessment": (
                "supported"
                if receiver_hypothesis_supported
                else "insufficient-evidence"
            ),
            "summary": (
                "Receiver pressure requires drop, buffer, or backpressure evidence."
            ),
            "supportingEvidence": receiver_support,
            "contradictoryEvidence": receiver_contradictions,
            "missingEvidence": receiver_missing,
        },
        {
            "id": "receiver-recovery-margin-exhausted",
            "failureDomain": "receiver playout/recovery boundary",
            "assessment": (
                "supported"
                if recovery_margin_supported
                else "insufficient-evidence"
            ),
            "summary": (
                "Paired loss and retransmission followed by receiver drops and an "
                "accounted delivery gap indicate that the configured receiver "
                "recovery/playout margin was exhausted; they do not prove which "
                "latency setting would be sufficient."
            ),
            "supportingEvidence": recovery_margin_support,
            "contradictoryEvidence": recovery_margin_contradictions,
            "missingEvidence": recovery_margin_missing,
        },
    ]
    if (
        path_supported
        and not sender_hypothesis_supported
        and not receiver_hypothesis_supported
        and not recovery_margin_supported
    ):
        selected = {
            "id": "transmission-path-loss-with-recovery",
            "assessment": "supported",
            "rationale": (
                "Aligned sender loss, receiver loss, and retransmission deltas "
                "identify recoverable path loss without sufficient "
                "endpoint-pressure evidence."
            ),
        }
    elif (
        rtt_supported
        and not sender_hypothesis_supported
        and not receiver_hypothesis_supported
        and not recovery_margin_supported
    ):
        selected = {
            "id": "transmission-path-rtt-step-with-recovery",
            "assessment": "supported",
            "rationale": (
                "Paired sender and receiver RTT cross conservative warm-baseline "
                "thresholds and recover without loss or endpoint-pressure evidence."
            ),
        }
    elif (
        sender_pressure_supported
        and not path_supported
        and not receiver_hypothesis_supported
        and not recovery_margin_supported
    ):
        selected = {
            "id": "sender-side-pressure",
            "assessment": "supported",
            "rationale": (
                "Sender buffer growth overlaps elevated flight size while the "
                "receiver buffer remains below its pressure threshold, then "
                "recovers without supported path-loss evidence."
            ),
        }
    elif (
        receiver_pressure_supported
        and not path_supported
        and not sender_hypothesis_supported
        and not recovery_margin_supported
    ):
        selected = {
            "id": "receiver-side-pressure",
            "assessment": "supported",
            "rationale": (
                "Receiver buffer growth overlaps upstream sender pressure and "
                "recovers after the deviation without a supported path-loss "
                "explanation."
            ),
        }
    elif (
        recovery_margin_supported
        and not sender_pressure_supported
        and not receiver_pressure_supported
    ):
        selected = {
            "id": "receiver-recovery-margin-exhausted",
            "assessment": "supported",
            "rationale": (
                "Paired loss and retransmission are followed by receiver drops "
                "that exactly account for the unique delivery gap under a "
                "configured TSBPD delay, then recover without security evidence."
            ),
        }
    else:
        selected = {
            "id": None,
            "assessment": "insufficient-evidence",
            "rationale": (
                "The available signals do not isolate one failure domain without a "
                "supported competing explanation."
            ),
        }

    source_summary = {
        "format": source["format"],
        "incidentId": source["incidentId"],
        "declaredFailureClass": source["failureClass"],
        "flowId": source["flowId"],
        "artifacts": source["artifacts"],
        "streams": [
            {
                "role": role,
                "streamId": streams[role]["header"]["streamId"],
                "provider": streams[role]["header"]["configuredProvider"],
                "providerVersion": streams[role]["header"]["providerVersion"],
                "providerRevision": streams[role]["header"]["providerRevision"],
                "buildProfile": streams[role]["header"]["buildProfile"],
                "recordCount": len(streams[role]["samples"]),
                "overwrittenCount": streams[role]["header"]["overwrittenCount"],
            }
            for role in ["sender", "receiver"]
        ],
    }
    non_claims = list(source["nonClaims"])
    if sender_drop_total > 0:
        non_claims.extend([
            "Sender-drop counters do not by themselves prove network loss or "
            "application payload loss; they count local send/retransmission-buffer removals.",
            "The combined sender-drop counter does not distinguish message TTL "
            "from TLPKTDROP or establish the effective sender deadline.",
        ])
    non_claims.extend(
        [
            "The analysis does not identify the exact failing network hop.",
            "RTT evidence does not isolate one-way delay or its direction.",
            (
                "Public SRT buffer gauges do not expose exact application "
                "scheduling or consumption timing."
            ),
            (
                "Public SRT statistics do not expose the offered application "
                "rate or identify an exact egress capacity restriction."
            ),
            (
                "The analysis does not infer unavailable endpoint-pressure metrics "
                "as zero."
            ),
            (
                "Receiver drops and configured TSBPD delay do not by themselves "
                "prove that increasing latency is the correct remediation."
            ),
        ]
    )
    return {
        "schemaVersion": "1alpha1",
        "kind": "robotweax-srt-local-incident-analysis",
        "status": "pass",
        "source": source_summary,
        "timeline": {
            "sampleCount": len(observations),
            "firstDeviationSample": first_deviation,
            "lastDeviationSample": last_deviation,
            "recoverySample": recovery_sample,
            "qualityBoundaries": quality_boundaries,
            "observations": observations,
        },
        "baselines": {
            "rttSeconds": {
                "sender": sender_rtt_baseline,
                "receiver": receiver_rtt_baseline,
                "senderStepThreshold": sender_rtt_threshold,
                "receiverStepThreshold": receiver_rtt_threshold,
                "rule": (
                    "minimum warm sample 1/2 baseline; "
                    "threshold=max(2x, baseline+0.020s)"
                ),
            },
            "pressure": {
                "senderSendBufferBytes": sender_buffer_baseline,
                "senderSendBufferStepThreshold": sender_buffer_threshold,
                "senderFlightSizePackets": sender_flight_baseline,
                "senderFlightStepThreshold": sender_flight_threshold,
                "receiverReceiveBufferBytes": receiver_buffer_baseline,
                "receiverReceiveBufferStepThreshold": receiver_buffer_threshold,
                "rule": (
                    "maximum warm sample 1/2 baseline; sender byte "
                    "threshold=max(2x, baseline+65536); receiver byte "
                    "threshold=max(2x, baseline+1048576); packet "
                    "threshold=max(2x, baseline+8)"
                ),
            },
        },
        "totals": {
            "senderLossPackets": sender_loss_total,
            "receiverLossPackets": receiver_loss_total,
            "retransmittedPackets": retransmission_total,
            "physicalSentPackets": physical_total,
            "uniqueSentPackets": unique_sent_total,
            "uniqueReceivedPackets": unique_received_total,
        },
        "hypotheses": hypotheses,
        "selectedHypothesis": selected,
        "nonClaims": list(dict.fromkeys(non_claims)),
    }


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Correlate paired SRT telemetry and produce an explainable incident "
            "analysis"
        )
    )
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--manifest", type=Path)
    inputs.add_argument("--jsonl", type=Path, nargs=2, metavar=("SENDER", "RECEIVER"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compact", action="store_true")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if arguments.manifest is not None:
        if not arguments.manifest.is_file():
            fail(f"manifest does not exist: {arguments.manifest}")
        source = load_manifest(arguments.manifest.resolve())
    else:
        for path in arguments.jsonl:
            if not path.is_file():
                fail(f"JSONL recording does not exist: {path}")
        source = load_jsonl([path.resolve() for path in arguments.jsonl])
    result = build_analysis(source)
    if arguments.compact:
        encoded = json.dumps(result, sort_keys=True, separators=(",", ":")) + "\n"
    else:
        encoded = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if arguments.output is None:
        sys.stdout.write(encoded)
    else:
        if arguments.output.exists():
            fail(f"refusing to overwrite analysis result: {arguments.output}")
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(encoded, encoding="utf-8")
        print(f"incident analysis written: {arguments.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"incident analysis failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
