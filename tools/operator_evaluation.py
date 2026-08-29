#!/usr/bin/env python3

"""Prepare, run, score, and summarize a bounded operator comparison study."""

import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import statistics
import sys
import tempfile
import time
from pathlib import Path
from typing import Iterable, List, Optional, Tuple


SCHEMA_VERSION = "1alpha1"
STUDY_KIND = "robotweax-srt-operator-evaluation-study"
RAW_VIEW_KIND = "robotweax-srt-raw-statistics-task"
REPORT_VIEW_KIND = "robotweax-srt-incident-report-task"
REFERENCE_KIND = "robotweax-srt-operator-evaluation-reference"
RESPONSE_KIND = "robotweax-srt-operator-evaluation-response"
SESSION_KIND = "robotweax-srt-operator-evaluation-session"
SUMMARY_KIND = "robotweax-srt-operator-evaluation-summary"

CONDITIONS = {"raw-statistics", "incident-report"}
EVIDENCE_CLASSES = {"synthetic-harness", "internal-pilot", "design-partner"}
FAILURE_DOMAINS = {
    "transmission-path",
    "sender-endpoint",
    "receiver-endpoint",
    "exact-network-hop",
    "insufficient-evidence",
}
HYPOTHESIS_OPTIONS = {
    "transmission-path-loss-with-recovery",
    "sender-side-pressure",
    "receiver-side-pressure",
}
MISSING_EVIDENCE_OPTIONS = {
    "packet-sequence-adjacency",
    "exact-failing-hop",
    "sender-buffer-occupancy",
    "sender-flight-size",
    "receiver-buffer-occupancy",
    "paired-loss-alignment",
    "sender-retransmission-deltas",
}

DOMAIN_BY_ANALYSIS = {
    "sender-to-receiver transmission path": "transmission-path",
    "sender endpoint": "sender-endpoint",
    "receiver endpoint": "receiver-endpoint",
}

MISSING_EVIDENCE_BY_TEXT = {
    "Packet sequence adjacency and the exact failing network hop are unavailable.": {
        "packet-sequence-adjacency",
        "exact-failing-hop",
    },
    "Sender buffer occupancy is unavailable.": {"sender-buffer-occupancy"},
    "Sender flight-size evidence is unavailable.": {"sender-flight-size"},
    "Receiver buffer occupancy is unavailable.": {"receiver-buffer-occupancy"},
    "Paired sender and receiver loss deltas are not both available.": {
        "paired-loss-alignment"
    },
    "Positive sender and receiver loss do not overlap in aligned samples.": {
        "paired-loss-alignment"
    },
    "Sender retransmission deltas are unavailable.": {"sender-retransmission-deltas"},
}

PARTICIPANT_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")
STUDY_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")


def fail(message: str) -> None:
    raise RuntimeError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path: Path) -> dict:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read JSON document {path}: {error}") from error
    if not isinstance(document, dict):
        fail(f"JSON document is not an object: {path}")
    return document


def encode_json(document: dict) -> str:
    return json.dumps(document, indent=2, sort_keys=True) + "\n"


def write_new_json(path: Path, document: dict) -> None:
    if path.exists():
        fail(f"refusing to overwrite output: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(encode_json(document), encoding="utf-8")


def load_analyzer(path: Path):
    specification = importlib.util.spec_from_file_location(
        "robotweax_operator_evaluation_analyzer", path
    )
    if specification is None or specification.loader is None:
        fail(f"cannot load incident analyzer: {path}")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def contained_file(directory: Path, relative: object, label: str) -> Path:
    if not isinstance(relative, str) or not relative:
        fail(f"{label} has no file name")
    candidate = (directory / relative).resolve()
    if candidate.parent != directory.resolve() or not candidate.is_file():
        fail(f"{label} is outside the incident directory or missing: {relative!r}")
    return candidate


def validate_digest(path: Path, expected: object, label: str) -> None:
    if not isinstance(expected, str) or sha256_file(path) != expected:
        fail(f"{label} digest mismatch: {path}")


def load_incident(manifest_path: Path, analyzer) -> dict:
    manifest_path = manifest_path.resolve()
    manifest = read_json(manifest_path)
    if manifest.get("schemaVersion") != SCHEMA_VERSION:
        fail(f"unsupported incident manifest: {manifest_path}")
    incident_id = manifest.get("incidentId")
    if not isinstance(incident_id, str) or not re.fullmatch(
        r"incident-[0-9]{3,}-[a-z0-9-]+", incident_id
    ):
        fail(f"invalid incident identity: {manifest_path}")
    recordings = manifest.get("recordings")
    if (
        not isinstance(recordings, dict)
        or recordings.get("schemaVersion") != SCHEMA_VERSION
        or recordings.get("format") != "robotweax-srt-telemetry-jsonl-v1"
    ):
        fail(f"incident has no supported retained JSONL recordings: {manifest_path}")

    recording_paths = []
    for role in ["sender", "receiver"]:
        record = recordings.get(role)
        if not isinstance(record, dict):
            fail(f"incident recording metadata is missing {role}: {manifest_path}")
        recording = contained_file(manifest_path.parent, record.get("file"), role)
        validate_digest(recording, record.get("sha256"), role)
        recording_paths.append(recording)

    analysis_record = recordings.get("analysis")
    if not isinstance(analysis_record, dict):
        fail(f"incident has no retained analysis: {manifest_path}")
    analysis_path = contained_file(
        manifest_path.parent, analysis_record.get("file"), "analysis"
    )
    validate_digest(analysis_path, analysis_record.get("sha256"), "analysis")
    retained_analysis = read_json(analysis_path)
    reproduced = analyzer.build_analysis(analyzer.load_jsonl(recording_paths))
    if reproduced != retained_analysis:
        fail(f"retained incident analysis is not reproducible: {manifest_path}")
    if retained_analysis.get("selectedHypothesis", {}).get("assessment") != "supported":
        message = "evaluation scenario has no supported reference hypothesis: "
        fail(message + str(manifest_path))

    return {
        "manifestPath": manifest_path,
        "manifest": manifest,
        "manifestSha256": sha256_file(manifest_path),
        "recordingPaths": recording_paths,
        "source": analyzer.load_jsonl(recording_paths),
        "analysis": retained_analysis,
    }


def build_raw_view(study_id: str, incident: dict) -> dict:
    source = incident["source"]
    streams = source["streams"]
    rows = []
    for role in ["sender", "receiver"]:
        stream = streams[role]
        for sample in stream["samples"]:
            rows.append(
                {
                    "sampleIndex": sample["sampleIndex"],
                    "role": role,
                    "raw": sample["raw"],
                    "delta": sample["delta"],
                }
            )
    rows.sort(key=lambda item: (item["sampleIndex"], item["role"]))
    headers = []
    for role in ["sender", "receiver"]:
        header = streams[role]["header"]
        headers.append(
            {
                "role": role,
                "provider": header["configuredProvider"],
                "providerVersion": header["providerVersion"],
                "providerRevision": header["providerRevision"],
                "buildProfile": header["buildProfile"],
                "recordCount": header["recordCount"],
                "overwrittenCount": header["overwrittenCount"],
            }
        )
    return {
        "schemaVersion": SCHEMA_VERSION,
        "kind": RAW_VIEW_KIND,
        "studyId": study_id,
        "scenarioId": incident["manifest"]["incidentId"],
        "condition": "raw-statistics",
        "instructions": (
            "Use only the paired normalized public SRT statistics below. Identify "
            "the first deviating sample, a useful failure domain, supported "
            "hypotheses, and missing evidence. No correlation or diagnostic "
            "inference has been added to this view."
        ),
        "nullSemantics": "null means unavailable; an observed zero is encoded as 0",
        "streams": headers,
        "rows": rows,
    }


def build_report_view(study_id: str, incident: dict) -> dict:
    return {
        "schemaVersion": SCHEMA_VERSION,
        "kind": REPORT_VIEW_KIND,
        "studyId": study_id,
        "scenarioId": incident["manifest"]["incidentId"],
        "condition": "incident-report",
        "instructions": (
            "Use the deterministic incident report below. Identify the first "
            "deviating sample, a useful failure domain, supported hypotheses, and "
            "missing evidence. Treat non-claims as explicit limits."
        ),
        "report": incident["analysis"],
    }


def missing_evidence_ids(analysis: dict) -> List[str]:
    identifiers = set()
    unknown = []
    for hypothesis in analysis["hypotheses"]:
        for statement in hypothesis["missingEvidence"]:
            mapped = MISSING_EVIDENCE_BY_TEXT.get(statement)
            if mapped is None:
                unknown.append(statement)
            else:
                identifiers.update(mapped)
    if unknown:
        fail(
            "operator evaluation has no stable missing-evidence identifier for: "
            + "; ".join(sorted(set(unknown)))
        )
    return sorted(identifiers)


def build_reference(study_id: str, incident: dict) -> dict:
    analysis = incident["analysis"]
    selected_id = analysis["selectedHypothesis"]["id"]
    selected = next(
        hypothesis
        for hypothesis in analysis["hypotheses"]
        if hypothesis["id"] == selected_id
    )
    domain = DOMAIN_BY_ANALYSIS.get(selected["failureDomain"])
    if domain is None:
        fail(f"unsupported reference failure domain: {selected['failureDomain']}")
    return {
        "schemaVersion": SCHEMA_VERSION,
        "kind": REFERENCE_KIND,
        "studyId": study_id,
        "scenarioId": incident["manifest"]["incidentId"],
        "failureClass": incident["manifest"]["failureClass"],
        "firstDeviationSample": analysis["timeline"]["firstDeviationSample"],
        "lastDeviationSample": analysis["timeline"]["lastDeviationSample"],
        "recoverySample": analysis["timeline"]["recoverySample"],
        "failureDomain": domain,
        "supportedHypotheses": sorted(
            hypothesis["id"]
            for hypothesis in analysis["hypotheses"]
            if hypothesis["assessment"] == "supported"
        ),
        "missingEvidence": missing_evidence_ids(analysis),
        "nonClaims": analysis["nonClaims"],
    }


def artifact(path: Path, base: Path) -> dict:
    return {"file": str(path.relative_to(base)), "sha256": sha256_file(path)}


def prepare_study(
    study_id: str, manifest_paths: List[Path], analyzer_path: Path, output_dir: Path
) -> Path:
    if not STUDY_PATTERN.fullmatch(study_id):
        fail("study id must use lowercase letters, digits, and hyphens")
    if len(manifest_paths) != 2:
        fail("the crossover candidate requires exactly two incident manifests")
    if output_dir.exists():
        fail(f"refusing to overwrite study packet: {output_dir}")
    analyzer = load_analyzer(analyzer_path.resolve())
    incidents = [load_incident(path, analyzer) for path in manifest_paths]
    scenario_ids = [incident["manifest"]["incidentId"] for incident in incidents]
    if len(set(scenario_ids)) != len(scenario_ids):
        fail("study scenarios must have distinct incident identities")

    output_dir = output_dir.resolve()
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(
        tempfile.mkdtemp(
            prefix=f".{output_dir.name}.partial-", dir=str(output_dir.parent)
        )
    )
    try:
        scenarios = []
        for incident in incidents:
            scenario_id = incident["manifest"]["incidentId"]
            scenario_dir = staging / "scenarios" / scenario_id
            raw_path = scenario_dir / "raw-statistics.json"
            report_path = scenario_dir / "incident-report.json"
            reference_path = scenario_dir / "reference.json"
            write_new_json(raw_path, build_raw_view(study_id, incident))
            write_new_json(report_path, build_report_view(study_id, incident))
            write_new_json(reference_path, build_reference(study_id, incident))
            scenarios.append(
                {
                    "scenarioId": scenario_id,
                    "failureClass": incident["manifest"]["failureClass"],
                    "sourceManifest": {
                        "file": str(incident["manifestPath"]),
                        "sha256": incident["manifestSha256"],
                    },
                    "views": {
                        "raw-statistics": artifact(raw_path, staging),
                        "incident-report": artifact(report_path, staging),
                    },
                    "reference": artifact(reference_path, staging),
                }
            )

        first, second = scenario_ids
        study = {
            "schemaVersion": SCHEMA_VERSION,
            "kind": STUDY_KIND,
            "studyId": study_id,
            "status": "candidate",
            "design": {
                "type": "two-scenario-counterbalanced-crossover",
                "allocation": (
                    "SHA-256(studyId:participantId) first-byte parity; even selects "
                    "arm-a and odd selects arm-b"
                ),
                "rationale": (
                    "Each participant sees each incident once, preventing direct "
                    "answer recall while balancing scenario and condition across arms."
                ),
            },
            "arms": {
                "arm-a": [
                    {"scenarioId": first, "condition": "raw-statistics"},
                    {"scenarioId": second, "condition": "incident-report"},
                ],
                "arm-b": [
                    {"scenarioId": first, "condition": "incident-report"},
                    {"scenarioId": second, "condition": "raw-statistics"},
                ],
            },
            "questions": {
                "failureDomains": sorted(FAILURE_DOMAINS),
                "hypotheses": sorted(HYPOTHESIS_OPTIONS),
                "missingEvidence": sorted(MISSING_EVIDENCE_OPTIONS),
                "confidenceScale": {"minimum": 1, "maximum": 5},
            },
            "metrics": [
                "time-to-first-deviation",
                "time-to-failure-domain",
                "first-deviation-accuracy",
                "failure-domain-accuracy",
                "unsupported-hypothesis-count",
                "missing-evidence-precision-recall",
                "operator-confidence",
            ],
            "gateLimits": {
                "minimumDistinctFailureClasses": 5,
                "automaticProductClaim": False,
                "note": (
                    "The strategy requires at least five reproducible failure classes "
                    "and faster operator understanding. This two-scenario packet "
                    "validates the method but cannot close that gate alone."
                ),
            },
            "privacy": {
                "participantId": "pseudonymous identifier only",
                "forbidden": [
                    "customer telemetry",
                    "names or email addresses",
                    "private network inventory",
                    "stream identifiers",
                    "media payloads",
                ],
            },
            "scenarios": scenarios,
        }
        write_new_json(staging / "study.json", study)
        os.replace(str(staging), str(output_dir))
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    return output_dir / "study.json"


def validate_study(study_path: Path) -> Tuple[dict, Path]:
    study_path = study_path.resolve()
    study = read_json(study_path)
    if (
        study.get("schemaVersion") != SCHEMA_VERSION
        or study.get("kind") != STUDY_KIND
        or set(study.get("arms", {})) != {"arm-a", "arm-b"}
    ):
        fail(f"unsupported operator evaluation study: {study_path}")
    base = study_path.parent
    scenario_ids = set()
    for scenario in study.get("scenarios", []):
        scenario_id = scenario.get("scenarioId")
        if not isinstance(scenario_id, str) or scenario_id in scenario_ids:
            fail("study has invalid or duplicate scenarios")
        scenario_ids.add(scenario_id)
        entries = list(scenario.get("views", {}).values()) + [scenario.get("reference")]
        for entry in entries:
            if not isinstance(entry, dict):
                fail(f"study scenario artifact metadata is invalid: {scenario_id}")
            path = (base / entry.get("file", "")).resolve()
            try:
                path.relative_to(base)
            except ValueError as error:
                raise RuntimeError(f"study artifact escapes packet: {path}") from error
            if not path.is_file() or sha256_file(path) != entry.get("sha256"):
                fail(f"study artifact is missing or changed: {path}")
    if len(scenario_ids) != 2:
        fail("study must contain exactly two scenarios")
    for arm, assignments in study["arms"].items():
        if len(assignments) != 2:
            fail(f"{arm} must contain two tasks")
        if {item.get("scenarioId") for item in assignments} != scenario_ids:
            fail(f"{arm} does not cover each scenario once")
        if {item.get("condition") for item in assignments} != CONDITIONS:
            fail(f"{arm} does not cover each condition once")
    return study, base


def choose_arm(study_id: str, participant_id: str) -> str:
    digest = hashlib.sha256(f"{study_id}:{participant_id}".encode("utf-8")).digest()
    return "arm-a" if digest[0] % 2 == 0 else "arm-b"


def validate_participant(participant_id: object) -> str:
    if not isinstance(participant_id, str) or not PARTICIPANT_PATTERN.fullmatch(
        participant_id
    ):
        fail(
            "participant id must be a pseudonymous 1-64 character identifier using "
            "letters, digits, underscore, or hyphen"
        )
    return participant_id


def scenario_by_id(study: dict, scenario_id: str) -> dict:
    try:
        return next(
            scenario
            for scenario in study["scenarios"]
            if scenario["scenarioId"] == scenario_id
        )
    except StopIteration as error:
        raise RuntimeError(f"study has no scenario {scenario_id}") from error


def load_study_artifact(base: Path, entry: dict) -> dict:
    path = (base / entry["file"]).resolve()
    if not path.is_file() or sha256_file(path) != entry["sha256"]:
        fail(f"study artifact is missing or changed: {path}")
    return read_json(path)


def validate_string_list(value: object, allowed: set, label: str) -> List[str]:
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        fail(f"{label} must be a list of identifiers")
    if len(set(value)) != len(value):
        fail(f"{label} contains duplicate identifiers")
    unsupported = set(value) - allowed
    if unsupported:
        fail(f"{label} contains unsupported identifiers: {sorted(unsupported)}")
    return sorted(value)


def ratio(numerator: int, denominator: int) -> float:
    return 1.0 if denominator == 0 and numerator == 0 else numerator / denominator


def score_task(answer: dict, reference: dict) -> dict:
    first = answer.get("firstDeviationSample")
    if first is not None and (
        not isinstance(first, int) or isinstance(first, bool) or first < 0
    ):
        fail("firstDeviationSample must be a non-negative integer or null")
    domain = answer.get("failureDomain")
    if domain not in FAILURE_DOMAINS:
        fail(f"unsupported failure domain response: {domain!r}")
    hypotheses = validate_string_list(
        answer.get("supportedHypotheses"), HYPOTHESIS_OPTIONS, "supportedHypotheses"
    )
    missing = validate_string_list(
        answer.get("missingEvidence"), MISSING_EVIDENCE_OPTIONS, "missingEvidence"
    )
    confidence = answer.get("confidence")
    if (
        not isinstance(confidence, int)
        or isinstance(confidence, bool)
        or not 1 <= confidence <= 5
    ):
        fail("confidence must be an integer from 1 through 5")
    notes = answer.get("notes", "")
    if not isinstance(notes, str) or len(notes) > 2000:
        fail("notes must be a string no longer than 2000 characters")

    expected_hypotheses = set(reference["supportedHypotheses"])
    selected_hypotheses = set(hypotheses)
    expected_missing = set(reference["missingEvidence"])
    selected_missing = set(missing)
    true_missing = len(expected_missing & selected_missing)
    return {
        "firstDeviationCorrect": first == reference["firstDeviationSample"],
        "failureDomainCorrect": domain == reference["failureDomain"],
        "unsupportedHypotheses": sorted(selected_hypotheses - expected_hypotheses),
        "missedSupportedHypotheses": sorted(expected_hypotheses - selected_hypotheses),
        "missingEvidencePrecision": ratio(true_missing, len(selected_missing)),
        "missingEvidenceRecall": ratio(true_missing, len(expected_missing)),
    }


def validate_timing(timing: object) -> dict:
    if not isinstance(timing, dict):
        fail("task timing must be an object")
    required = [
        "timeToFirstDeviationMillis",
        "timeToFailureDomainMillis",
        "completionMillis",
    ]
    values = []
    for field in required:
        value = timing.get(field)
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            fail(f"{field} must be a non-negative integer")
        values.append(value)
    if values != sorted(values):
        fail("task timing milestones are not monotonic")
    return {field: timing[field] for field in required}


def score_response(study_path: Path, response: dict) -> dict:
    study, base = validate_study(study_path)
    if (
        response.get("schemaVersion") != SCHEMA_VERSION
        or response.get("kind") != RESPONSE_KIND
        or response.get("studyId") != study["studyId"]
    ):
        fail("response does not match the study")
    participant = validate_participant(response.get("participantId"))
    evidence_class = response.get("evidenceClass")
    if evidence_class not in EVIDENCE_CLASSES:
        fail(f"unsupported evidence class: {evidence_class!r}")
    arm = response.get("arm")
    if arm not in study["arms"]:
        fail(f"unsupported study arm: {arm!r}")
    collection_mode = response.get("collectionMode", "imported-response")
    if not isinstance(collection_mode, str) or not collection_mode:
        fail("collectionMode must be a non-empty string")
    expected_assignments = study["arms"][arm]
    tasks = response.get("tasks")
    if not isinstance(tasks, list) or len(tasks) != len(expected_assignments):
        fail("response task count does not match its study arm")

    scored_tasks = []
    for position, (task, expected) in enumerate(
        zip(tasks, expected_assignments), start=1
    ):
        if not isinstance(task, dict):
            fail(f"response task {position} is not an object")
        if (
            task.get("scenarioId") != expected["scenarioId"]
            or task.get("condition") != expected["condition"]
        ):
            fail(f"response task {position} does not match the assigned task")
        scenario = scenario_by_id(study, expected["scenarioId"])
        reference = load_study_artifact(base, scenario["reference"])
        timing = validate_timing(task.get("timing"))
        answer = task.get("answer")
        if not isinstance(answer, dict):
            fail(f"response task {position} has no answer")
        scores = score_task(answer, reference)
        scored_tasks.append(
            {
                "position": position,
                "scenarioId": expected["scenarioId"],
                "failureClass": reference["failureClass"],
                "condition": expected["condition"],
                "timing": timing,
                "answer": {
                    "firstDeviationSample": answer.get("firstDeviationSample"),
                    "failureDomain": answer.get("failureDomain"),
                    "supportedHypotheses": sorted(answer.get("supportedHypotheses")),
                    "missingEvidence": sorted(answer.get("missingEvidence")),
                    "confidence": answer.get("confidence"),
                    "notes": answer.get("notes", ""),
                },
                "score": scores,
                "reference": {
                    "file": scenario["reference"]["file"],
                    "sha256": scenario["reference"]["sha256"],
                },
            }
        )
    return {
        "schemaVersion": SCHEMA_VERSION,
        "kind": SESSION_KIND,
        "status": "complete",
        "studyId": study["studyId"],
        "study": {"file": str(study_path.resolve()), "sha256": sha256_file(study_path)},
        "participantId": participant,
        "evidenceClass": evidence_class,
        "arm": arm,
        "collectionMode": collection_mode,
        "tasks": scored_tasks,
    }


def milliseconds_since(start_ns: int) -> int:
    return int((time.monotonic_ns() - start_ns) / 1_000_000)


def render_number(value: object) -> str:
    return "—" if value is None else str(value)


def render_raw_view(view: dict) -> str:
    lines = [
        view["instructions"],
        "",
        "null/— = unavailable; 0 = observed zero",
        "",
        "sample role sent recv unique_sent unique_recv loss_s loss_r retrans "
        "drop_s drop_r rtt_s flight send_buf recv_buf",
    ]
    fields = [
        ("delta", "sent_packet_delta"),
        ("delta", "received_packet_delta"),
        ("delta", "unique_sent_packet_delta"),
        ("delta", "unique_received_packet_delta"),
        ("delta", "sender_loss_packet_delta"),
        ("delta", "receiver_loss_packet_delta"),
        ("delta", "retransmitted_packet_delta"),
        ("delta", "sender_drop_packet_delta"),
        ("delta", "receiver_drop_packet_delta"),
        ("raw", "rtt_seconds"),
        ("raw", "flight_size_packets"),
        ("raw", "send_buffer_bytes"),
        ("raw", "receive_buffer_bytes"),
    ]
    for row in view["rows"]:
        values = [render_number(row[source].get(field)) for source, field in fields]
        lines.append(f"{row['sampleIndex']:>6} {row['role']:<8} " + " ".join(values))
    return "\n".join(lines)


def render_report_view(view: dict) -> str:
    report = view["report"]
    timeline = report["timeline"]
    selected = report["selectedHypothesis"]
    lines = [
        view["instructions"],
        "",
        f"Samples: {timeline['sampleCount']}",
        f"First deviation: {render_number(timeline['firstDeviationSample'])}",
        f"Last deviation: {render_number(timeline['lastDeviationSample'])}",
        f"Recovery: {render_number(timeline['recoverySample'])}",
        "Selected hypothesis: "
        f"{render_number(selected['id'])} ({selected['assessment']})",
        f"Rationale: {selected['rationale']}",
        "",
        "Hypotheses:",
    ]
    for hypothesis in report["hypotheses"]:
        lines.append(
            f"- {hypothesis['id']} | {hypothesis['failureDomain']} | "
            f"{hypothesis['assessment']}"
        )
        for evidence in hypothesis["supportingEvidence"]:
            lines.append(f"  evidence: {evidence['summary']}")
        for contradiction in hypothesis["contradictoryEvidence"]:
            lines.append(f"  contradiction: {contradiction}")
        for missing in hypothesis["missingEvidence"]:
            lines.append(f"  missing: {missing}")
    lines.append("")
    lines.append("Non-claims:")
    lines.extend(f"- {statement}" for statement in report["nonClaims"])
    return "\n".join(lines)


def parse_first_deviation(value: str) -> Optional[int]:
    normalized = value.strip().lower()
    if normalized in {"unknown", "unbekannt", "none", "null", ""}:
        return None
    try:
        parsed = int(normalized)
    except ValueError as error:
        raise RuntimeError(
            "first deviation must be a sample number or 'unknown'"
        ) from error
    if parsed < 0:
        fail("first deviation cannot be negative")
    return parsed


def prompt_choice(prompt: str, allowed: set) -> str:
    while True:
        value = input(prompt).strip()
        if value in allowed:
            return value
        print("Choose one of: " + ", ".join(sorted(allowed)), file=sys.stderr)


def prompt_list(prompt: str, allowed: set) -> List[str]:
    while True:
        value = input(prompt).strip()
        if value.lower() in {"", "none", "keine"}:
            return []
        selected = [item.strip() for item in value.split(",") if item.strip()]
        if len(set(selected)) == len(selected) and not (set(selected) - allowed):
            return sorted(selected)
        print(
            "Enter comma-separated values from: " + ", ".join(sorted(allowed)),
            file=sys.stderr,
        )


def collect_interactive_response(
    study_path: Path,
    participant_id: str,
    evidence_class: str,
    selected_arm: Optional[str],
) -> dict:
    study, base = validate_study(study_path)
    participant_id = validate_participant(participant_id)
    if evidence_class not in EVIDENCE_CLASSES:
        fail(f"unsupported evidence class: {evidence_class!r}")
    arm = selected_arm or choose_arm(study["studyId"], participant_id)
    if arm not in study["arms"]:
        fail(f"unsupported study arm: {arm!r}")
    tasks = []
    print(
        f"Study {study['studyId']} | participant {participant_id} | {arm} | "
        f"{evidence_class}"
    )
    print("Do not enter names, email addresses, customer data, or private identifiers.")
    for position, assignment in enumerate(study["arms"][arm], start=1):
        scenario = scenario_by_id(study, assignment["scenarioId"])
        view = load_study_artifact(base, scenario["views"][assignment["condition"]])
        input(f"\nPress Enter to reveal task {position} of 2.")
        start = time.monotonic_ns()
        print(
            "\n"
            + (
                render_raw_view(view)
                if assignment["condition"] == "raw-statistics"
                else render_report_view(view)
            )
        )
        while True:
            try:
                first = parse_first_deviation(
                    input("\nFirst deviating sample (or unknown): ")
                )
                break
            except RuntimeError as error:
                print(str(error), file=sys.stderr)
        first_ms = milliseconds_since(start)
        print("Failure domains: " + ", ".join(sorted(FAILURE_DOMAINS)))
        domain = prompt_choice("Useful failure domain: ", FAILURE_DOMAINS)
        domain_ms = milliseconds_since(start)
        print("Hypotheses: " + ", ".join(sorted(HYPOTHESIS_OPTIONS)))
        hypotheses = prompt_list(
            "Supported hypotheses (comma-separated or none): ", HYPOTHESIS_OPTIONS
        )
        print("Missing evidence: " + ", ".join(sorted(MISSING_EVIDENCE_OPTIONS)))
        missing = prompt_list(
            "Missing evidence (comma-separated or none): ", MISSING_EVIDENCE_OPTIONS
        )
        while True:
            confidence_text = input("Confidence (1-5): ").strip()
            if confidence_text in {"1", "2", "3", "4", "5"}:
                confidence = int(confidence_text)
                break
            print("Confidence must be 1, 2, 3, 4, or 5.", file=sys.stderr)
        notes = input("Optional short notes (no identifying data): ").strip()
        completion_ms = milliseconds_since(start)
        tasks.append(
            {
                "scenarioId": assignment["scenarioId"],
                "condition": assignment["condition"],
                "timing": {
                    "timeToFirstDeviationMillis": first_ms,
                    "timeToFailureDomainMillis": domain_ms,
                    "completionMillis": completion_ms,
                },
                "answer": {
                    "firstDeviationSample": first,
                    "failureDomain": domain,
                    "supportedHypotheses": hypotheses,
                    "missingEvidence": missing,
                    "confidence": confidence,
                    "notes": notes,
                },
            }
        )
    return {
        "schemaVersion": SCHEMA_VERSION,
        "kind": RESPONSE_KIND,
        "studyId": study["studyId"],
        "participantId": participant_id,
        "evidenceClass": evidence_class,
        "arm": arm,
        "collectionMode": "interactive",
        "tasks": tasks,
    }


def mean(values: Iterable[float]) -> float:
    numbers = list(values)
    return sum(numbers) / len(numbers)


def condition_statistics(tasks: List[dict]) -> dict:
    return {
        "taskCount": len(tasks),
        "medianTimeToFirstDeviationMillis": statistics.median(
            task["timing"]["timeToFirstDeviationMillis"] for task in tasks
        ),
        "medianTimeToFailureDomainMillis": statistics.median(
            task["timing"]["timeToFailureDomainMillis"] for task in tasks
        ),
        "firstDeviationAccuracy": mean(
            float(task["score"]["firstDeviationCorrect"]) for task in tasks
        ),
        "failureDomainAccuracy": mean(
            float(task["score"]["failureDomainCorrect"]) for task in tasks
        ),
        "meanUnsupportedHypothesisCount": mean(
            len(task["score"]["unsupportedHypotheses"]) for task in tasks
        ),
        "meanMissingEvidencePrecision": mean(
            task["score"]["missingEvidencePrecision"] for task in tasks
        ),
        "meanMissingEvidenceRecall": mean(
            task["score"]["missingEvidenceRecall"] for task in tasks
        ),
        "meanConfidence": mean(task["answer"]["confidence"] for task in tasks),
    }


def validate_completed_session(
    study: dict, base: Path, study_path: Path, session: dict
) -> None:
    study_record = session.get("study")
    if not isinstance(study_record, dict) or study_record.get("sha256") != sha256_file(
        study_path
    ):
        fail(
            f"session study digest is missing or stale: {session.get('participantId')}"
        )
    arm = session.get("arm")
    if arm not in study["arms"]:
        fail(f"session has unsupported arm: {arm!r}")
    collection_mode = session.get("collectionMode")
    if not isinstance(collection_mode, str) or not collection_mode:
        fail("session collectionMode must be a non-empty string")
    expected_assignments = study["arms"][arm]
    tasks = session.get("tasks")
    if not isinstance(tasks, list) or len(tasks) != len(expected_assignments):
        fail(
            f"session task count does not match its arm: {session.get('participantId')}"
        )
    for position, (task, assignment) in enumerate(
        zip(tasks, expected_assignments), start=1
    ):
        if not isinstance(task, dict) or task.get("position") != position:
            fail(
                f"session has an invalid task position: {session.get('participantId')}"
            )
        if (
            task.get("scenarioId") != assignment["scenarioId"]
            or task.get("condition") != assignment["condition"]
        ):
            fail(f"session task does not match its arm: {session.get('participantId')}")
        scenario = scenario_by_id(study, assignment["scenarioId"])
        reference = load_study_artifact(base, scenario["reference"])
        if task.get("failureClass") != reference["failureClass"]:
            fail(f"session failure class is stale: {session.get('participantId')}")
        if task.get("reference") != scenario["reference"]:
            fail(f"session reference digest is stale: {session.get('participantId')}")
        validate_timing(task.get("timing"))
        answer = task.get("answer")
        if not isinstance(answer, dict) or task.get("score") != score_task(
            answer, reference
        ):
            fail(f"session score is invalid or stale: {session.get('participantId')}")


def summarize_sessions(study_path: Path, session_paths: List[Path]) -> dict:
    study, base = validate_study(study_path)
    if not session_paths:
        fail("summary requires at least one completed session")
    sessions = []
    participants = set()
    for path in session_paths:
        session = read_json(path.resolve())
        if (
            session.get("schemaVersion") != SCHEMA_VERSION
            or session.get("kind") != SESSION_KIND
            or session.get("status") != "complete"
            or session.get("studyId") != study["studyId"]
        ):
            fail(f"unsupported or mismatched session: {path}")
        participant = validate_participant(session.get("participantId"))
        if participant in participants:
            fail(f"duplicate participant session: {participant}")
        validate_completed_session(study, base, study_path.resolve(), session)
        participants.add(participant)
        sessions.append(session)

    by_condition = {condition: [] for condition in CONDITIONS}
    failure_classes = set()
    evidence_counts = {classification: 0 for classification in sorted(EVIDENCE_CLASSES)}
    paired = []
    for session in sessions:
        current_participant = session["participantId"]
        evidence_class = session.get("evidenceClass")
        if evidence_class not in EVIDENCE_CLASSES:
            fail(f"session has unsupported evidence class: {evidence_class!r}")
        evidence_counts[evidence_class] += 1
        participant_tasks = {}
        for task in session.get("tasks", []):
            condition = task.get("condition")
            if condition not in CONDITIONS or condition in participant_tasks:
                fail(
                    "session does not contain one task per condition: "
                    + current_participant
                )
            by_condition[condition].append(task)
            participant_tasks[condition] = task
            failure_classes.add(task["failureClass"])
        if set(participant_tasks) != CONDITIONS:
            fail(f"session does not cover both conditions: {session['participantId']}")
        raw = participant_tasks["raw-statistics"]
        report = participant_tasks["incident-report"]
        paired.append(
            {
                "participantId": session["participantId"],
                "rawMinusReportTimeToFirstDeviationMillis": (
                    raw["timing"]["timeToFirstDeviationMillis"]
                    - report["timing"]["timeToFirstDeviationMillis"]
                ),
                "rawMinusReportTimeToFailureDomainMillis": (
                    raw["timing"]["timeToFailureDomainMillis"]
                    - report["timing"]["timeToFailureDomainMillis"]
                ),
                "reportMinusRawFailureDomainCorrect": (
                    int(report["score"]["failureDomainCorrect"])
                    - int(raw["score"]["failureDomainCorrect"])
                ),
                "rawMinusReportUnsupportedHypothesisCount": (
                    len(raw["score"]["unsupportedHypotheses"])
                    - len(report["score"]["unsupportedHypotheses"])
                ),
                "reportMinusRawConfidence": (
                    report["answer"]["confidence"] - raw["answer"]["confidence"]
                ),
            }
        )

    statistics_by_condition = {
        condition: condition_statistics(by_condition[condition])
        for condition in sorted(CONDITIONS)
    }
    comparison = {
        "pairedParticipantCount": len(paired),
        "medianRawMinusReportTimeToFirstDeviationMillis": statistics.median(
            item["rawMinusReportTimeToFirstDeviationMillis"] for item in paired
        ),
        "medianRawMinusReportTimeToFailureDomainMillis": statistics.median(
            item["rawMinusReportTimeToFailureDomainMillis"] for item in paired
        ),
        "meanReportMinusRawFailureDomainCorrect": mean(
            item["reportMinusRawFailureDomainCorrect"] for item in paired
        ),
        "meanRawMinusReportUnsupportedHypothesisCount": mean(
            item["rawMinusReportUnsupportedHypothesisCount"] for item in paired
        ),
        "meanReportMinusRawConfidence": mean(
            item["reportMinusRawConfidence"] for item in paired
        ),
    }
    has_design_partner = evidence_counts["design-partner"] > 0
    enough_classes = (
        len(failure_classes) >= study["gateLimits"]["minimumDistinctFailureClasses"]
    )
    report_faster = comparison["medianRawMinusReportTimeToFailureDomainMillis"] > 0
    accuracy_not_worse = comparison["meanReportMinusRawFailureDomainCorrect"] >= 0
    unsupported_not_higher = (
        comparison["meanRawMinusReportUnsupportedHypothesisCount"] >= 0
    )
    eligible = all(
        [
            has_design_partner,
            enough_classes,
            report_faster,
            accuracy_not_worse,
            unsupported_not_higher,
        ]
    )
    if not has_design_partner:
        claim_status = (
            "harness-only"
            if evidence_counts["internal-pilot"] == 0
            else "internal-pilot-only"
        )
    elif not enough_classes:
        claim_status = "design-partner-evidence-incomplete"
    else:
        claim_status = (
            "eligible-for-human-gate-review" if eligible else "gate-criteria-not-met"
        )
    return {
        "schemaVersion": SCHEMA_VERSION,
        "kind": SUMMARY_KIND,
        "status": "pass",
        "studyId": study["studyId"],
        "study": {"file": str(study_path.resolve()), "sha256": sha256_file(study_path)},
        "participantCount": len(sessions),
        "evidenceClassCounts": evidence_counts,
        "distinctFailureClasses": sorted(failure_classes),
        "statistics": statistics_by_condition,
        "comparison": comparison,
        "claimAssessment": {
            "status": claim_status,
            "eligibleForHumanGateReview": eligible,
            "checks": {
                "hasDesignPartnerEvidence": has_design_partner,
                "minimumFiveFailureClasses": enough_classes,
                "incidentReportMedianFailureDomainTimeFaster": report_faster,
                "incidentReportFailureDomainAccuracyNotWorse": accuracy_not_worse,
                "incidentReportUnsupportedHypothesesNotHigher": unsupported_not_higher,
            },
            "nonClaim": (
                "Synthetic and internal sessions validate the harness only. Even an "
                "eligible result requires human review of participant selection, "
                "study conduct, sample size, and operational representativeness."
            ),
        },
        "pairedResults": paired,
    }


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run a counterbalanced raw-SRT-statistics versus incident-report "
            "evaluation"
        )
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    prepare = subparsers.add_parser("prepare", help="create an immutable study packet")
    prepare.add_argument("--study-id", required=True)
    prepare.add_argument("--manifest", type=Path, action="append", required=True)
    prepare.add_argument(
        "--analyzer",
        type=Path,
        default=Path(__file__).resolve().with_name("analyze_incident.py"),
    )
    prepare.add_argument("--output-dir", type=Path, required=True)

    run = subparsers.add_parser("run", help="collect and score one interactive session")
    run.add_argument("--study", type=Path, required=True)
    run.add_argument("--participant-id", required=True)
    run.add_argument(
        "--evidence-class", choices=sorted(EVIDENCE_CLASSES), required=True
    )
    run.add_argument("--arm", choices=["arm-a", "arm-b"])
    run.add_argument("--output", type=Path, required=True)

    score = subparsers.add_parser("score", help="score one imported response")
    score.add_argument("--study", type=Path, required=True)
    score.add_argument("--response", type=Path, required=True)
    score.add_argument("--output", type=Path, required=True)

    summarize = subparsers.add_parser("summarize", help="aggregate completed sessions")
    summarize.add_argument("--study", type=Path, required=True)
    summarize.add_argument("--session", type=Path, action="append", required=True)
    summarize.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if arguments.command == "prepare":
        result = prepare_study(
            arguments.study_id,
            arguments.manifest,
            arguments.analyzer,
            arguments.output_dir,
        )
        print(f"operator evaluation study written: {result}")
        return 0
    if arguments.command == "run":
        if arguments.output.exists():
            fail(f"refusing to overwrite session: {arguments.output}")
        response = collect_interactive_response(
            arguments.study,
            arguments.participant_id,
            arguments.evidence_class,
            arguments.arm,
        )
        session = score_response(arguments.study, response)
        write_new_json(arguments.output, session)
        print(f"operator evaluation session written: {arguments.output}")
        return 0
    if arguments.command == "score":
        response = read_json(arguments.response)
        session = score_response(arguments.study, response)
        write_new_json(arguments.output, session)
        print(f"operator evaluation session written: {arguments.output}")
        return 0
    summary = summarize_sessions(arguments.study, arguments.session)
    write_new_json(arguments.output, summary)
    print(f"operator evaluation summary written: {arguments.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"operator evaluation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
