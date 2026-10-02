#!/usr/bin/env python3
"""Validates OTLP/HTTP JSON metric requests against the OpenTelemetry protobuf.

Parses each request with the official schema (opentelemetry-proto) and no
unknown fields allowed, then checks what the schema cannot express:

- OTLP JSON rules: 64-bit integers and timestamps as strings, enums as
  integers, lowerCamelCase keys (an unknown key fails the parse);
- one metric per name; non-empty units; every sum cumulative and monotonic;
- every point timestamped, start before time, attribute keys unique.

Usage:
  otlp_validate.py FILE...            requests from files ("-" for stdin)
  otlp_validate.py --command CMD...   the stdout of a command
Exit status 0 when every request is valid; 2 when opentelemetry-proto is
missing (install with: pip install opentelemetry-proto).
"""

import json
import subprocess
import sys

try:
    from google.protobuf import json_format
    from opentelemetry.proto.collector.metrics.v1 import metrics_service_pb2
except ImportError:
    print("otlp_validate: needs opentelemetry-proto (pip install opentelemetry-proto)", file=sys.stderr)
    sys.exit(2)

CUMULATIVE = 2


def check_point(errors, where, point):
    keys = [a["key"] for a in point.get("attributes", [])]
    if len(keys) != len(set(keys)):
        errors.append(f"{where}: duplicate attribute keys {keys}")
    time = point.get("timeUnixNano")
    if not isinstance(time, str) or not time.isdigit() or int(time) == 0:
        errors.append(f"{where}: timeUnixNano must be a non-zero integer string, got {time!r}")
        return
    start = point.get("startTimeUnixNano")
    if start is not None:
        if not isinstance(start, str) or not start.isdigit():
            errors.append(f"{where}: startTimeUnixNano must be an integer string")
        elif int(start) > int(time):
            errors.append(f"{where}: start {start} after time {time}")
    if "asInt" in point and not isinstance(point["asInt"], str):
        errors.append(f"{where}: asInt must be a string (64-bit integer)")
    if "asInt" not in point and "asDouble" not in point:
        errors.append(f"{where}: no value")
    for a in point.get("attributes", []):
        value = a.get("value", {})
        if "intValue" in value and not isinstance(value["intValue"], str):
            errors.append(f"{where}: intValue of {a['key']} must be a string")


def validate(data, label):
    errors = []
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        return [f"{label}: not UTF-8: {error}"]
    try:
        request = json.loads(text)
    except json.JSONDecodeError as error:
        return [f"{label}: not JSON: {error}"]
    try:
        json_format.Parse(text, metrics_service_pb2.ExportMetricsServiceRequest(), ignore_unknown_fields=False)
    except json_format.ParseError as error:
        return [f"{label}: rejected by the OTLP schema: {error}"]
    points = 0
    for r, resource_metrics in enumerate(request.get("resourceMetrics", [])):
        keys = [a["key"] for a in resource_metrics.get("resource", {}).get("attributes", [])]
        attributes = set(keys)
        if len(attributes) != len(keys):
            errors.append(f"{label}: resource {r} repeats an attribute key")
        if "service.name" not in attributes:
            errors.append(f"{label}: resource {r} without service.name")
        for scope_metrics in resource_metrics.get("scopeMetrics", []):
            names = set()
            for metric in scope_metrics.get("metrics", []):
                name = metric.get("name", "")
                where = f"{label}: {name}"
                if name in names:
                    errors.append(f"{where}: metric repeated")
                names.add(name)
                if not metric.get("unit"):
                    errors.append(f"{where}: no unit")
                if "sum" in metric:
                    data = metric["sum"]
                    if data.get("aggregationTemporality") != CUMULATIVE or data.get("isMonotonic") is not True:
                        errors.append(f"{where}: sums must be cumulative (2, as an integer) and monotonic")
                elif "gauge" in metric:
                    data = metric["gauge"]
                else:
                    errors.append(f"{where}: neither gauge nor sum")
                    continue
                if not data.get("dataPoints"):
                    errors.append(f"{where}: metric without points")
                for i, point in enumerate(data.get("dataPoints", [])):
                    points += 1
                    check_point(errors, f"{where}[{i}]", point)
    if points == 0:
        errors.append(f"{label}: no data points")
    return errors


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__, file=sys.stderr)
        return 1
    inputs = []
    if args[0] == "--command":
        result = subprocess.run(args[1:], capture_output=True, timeout=120)
        if result.returncode != 0:
            print(f"otlp_validate: command failed: {result.stderr.decode(errors='replace')}", file=sys.stderr)
            return 1
        inputs.append((result.stdout, " ".join(args[1:])))
    else:
        for path in args:
            inputs.append((sys.stdin.buffer.read() if path == "-" else open(path, "rb").read(), path))
    failures = []
    for text, label in inputs:
        failures.extend(validate(text, label))
    for failure in failures:
        print("FAIL " + failure)
    if failures:
        return 1
    print(f"OTLP requests valid: {len(inputs)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
