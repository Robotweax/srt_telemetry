#!/usr/bin/env python3
"""Checks the Grafana dashboards and the alert rules against the metric names.

Static checks (always):
- every srt_* metric a query uses is a family in docs/architecture/metric-names.md;
- every window metric selects one statistic (stat label): summing across
  statistics would add unrelated values;
- the dashboards are what build_dashboards.py generates (no hand edits).

Live checks (with --prometheus URL): every dashboard query, with the
dashboard variables filled in, returns data from a Prometheus that scrapes
an exporter (the dashboard demo in CI). The endpoint dashboard is evaluated
for a sender and a receiver; each of its queries must return data for at
least one of them. Its connection-group queries are evaluated for a group
sender and receiver: with --groups the exporter must have them (the demo
with --group), otherwise those queries are skipped when it has none.

Usage: check_dashboards.py [--prometheus http://127.0.0.1:9090] [--job srt] [--groups]
"""

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DASHBOARDS = os.path.join(ROOT, "observability", "grafana", "dashboards")
RULES = os.path.join(ROOT, "observability", "prometheus", "srt-telemetry-alerts.yml")
NAMES = os.path.join(ROOT, "docs", "architecture", "metric-names.md")

failures = []


def fail(message):
    failures.append(message)
    print("FAIL " + message)


def families():
    """Window families (need stat) and all other families from the names doc."""
    text = open(NAMES, encoding="utf-8").read()
    window = set()
    other = set()
    for line in text.splitlines():
        if not line.startswith("| `"):
            continue
        names = re.findall(r"`(srt_[a-z0-9_]+)", line)
        if not names:
            continue
        cells = [c.strip() for c in line.split("|")]
        if len(cells) > 5 and cells[4] in ("rate", "gauge", "ratio"):
            window.add(names[0])
        else:
            other.update(names)
    other.add("srt_telemetry_schema_info")
    other.add("srt_telemetry:target")  # recording rule in the alert rules
    return window, other


def queries(dashboard):
    for panel in dashboard.get("panels", []):
        for target in panel.get("targets", []):
            if target.get("expr"):
                yield panel.get("title", "?"), target["expr"]
    for variable in dashboard.get("templating", {}).get("list", []):
        query = variable.get("query")
        if isinstance(query, dict):
            query = query.get("query")
        if isinstance(query, str) and "srt_" in query:
            yield f"variable {variable['name']}", query


def check_names(where, expr, window, other):
    # Metric names, not label values (inside quotes).
    stripped = re.sub(r'"[^"]*"', '""', expr)
    for match in re.finditer(r"\b(srt_[a-z0-9_:]+)(\{[^}]*\})?", stripped):
        name = match.group(1)
        if name not in window and name not in other:
            fail(f"{where}: unknown metric {name}")
            continue
        if name in window:
            selector = expr[match.start():].split("}", 1)[0]
            if "stat=" not in selector:
                fail(f"{where}: {name} without a stat selector")


def rule_exprs():
    """The expr of every rule, including folded (>-) multi-line ones."""
    exprs = []
    lines = open(RULES, encoding="utf-8").read().splitlines()
    for i, line in enumerate(lines):
        match = re.match(r"^(\s*)expr:\s*(.*)$", line)
        if not match:
            continue
        indent, text = len(match.group(1)), match.group(2)
        if text in (">-", ">", "|", "|-"):
            body = []
            for following in lines[i + 1:]:
                if following.strip() and len(following) - len(following.lstrip()) <= indent:
                    break
                body.append(following.strip())
            text = " ".join(body)
        exprs.append(text)
    if len(exprs) < 6:
        fail(f"alert rules: found {len(exprs)} expressions")
    return exprs


def fill(expr, job, source=None, name_label="stream", time_range="10m"):
    expr = expr.replace('job=~"$job"', f'job=~"{job}"').replace('instance=~"$instance"', 'instance=~".+"')
    expr = expr.replace('source_id=~"$source"', f'source_id="{source}"' if source else 'source_id=~".+"')
    return expr.replace("$name_label", name_label).replace("$__range", time_range)


def query(prometheus, expr):
    url = f"{prometheus}/api/v1/query?" + urllib.parse.urlencode({"query": expr})
    with urllib.request.urlopen(url, timeout=10) as response:
        body = json.load(response)
    if body.get("status") != "success":
        raise RuntimeError(body.get("error", "query failed"))
    return body["data"]["result"]


def source_ids(prometheus, job, family="srt_health_verdict", extra=',path="",group=""'):
    """A source per role: by default a plain endpoint (no connection group or
    path, whose panels differ); with family srt_group_usable_paths a group."""
    ids = {}
    for role in ("sender", "receiver"):
        result = query(prometheus, f'{family}{{job="{job}",role="{role}"{extra}}}')
        if result:
            # The worst one exercises the most panels.
            result.sort(key=lambda r: -float(r["value"][1]))
            ids[role] = result[0]["metric"]["source_id"]
    return ids


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--prometheus")
    parser.add_argument("--job", default="srt")
    parser.add_argument("--groups", action="store_true", help="require connection groups in the job")
    args = parser.parse_args()
    window, other = families()
    if len(window) < 30 or not other:
        fail(f"metric names doc parse: {len(window)} window and {len(other)} other families")

    generator = os.path.join(ROOT, "observability", "grafana", "build_dashboards.py")
    before = {n: open(os.path.join(DASHBOARDS, n), encoding="utf-8").read() for n in os.listdir(DASHBOARDS)}
    subprocess.run([sys.executable, generator], check=True, capture_output=True)
    for name, text in before.items():
        if open(os.path.join(DASHBOARDS, name), encoding="utf-8").read() != text:
            fail(f"{name} differs from build_dashboards.py output (edit the generator, then run it)")

    dashboards = {}
    for name in sorted(os.listdir(DASHBOARDS)):
        dashboards[name] = json.load(open(os.path.join(DASHBOARDS, name), encoding="utf-8"))
        for where, expr in queries(dashboards[name]):
            check_names(f"{name}: {where}", expr, window, other)
    for expr in rule_exprs():
        check_names("alert rules", expr, window, other)
    count = sum(1 for d in dashboards.values() for _ in queries(d))
    print(f"static: {count} queries in {len(dashboards)} dashboards checked")

    if args.prometheus:
        ids = source_ids(args.prometheus, args.job)
        if set(ids) != {"sender", "receiver"}:
            fail(f"live: need a sender and a receiver in job {args.job}, found {ids}")
        groups = source_ids(args.prometheus, args.job, "srt_group_usable_paths", "")
        if args.groups and set(groups) != {"sender", "receiver"}:
            fail(f"live: need a group sender and receiver in job {args.job}, found {groups}")
        live = 0
        skipped = 0
        for name, dashboard in dashboards.items():
            for where, expr in queries(dashboard):
                if where.startswith("variable"):
                    continue
                group_query = "srt_group_" in expr or "srt_health_path_verdict" in expr
                if group_query and not groups:
                    skipped += 1
                    continue
                if "$source" in expr:
                    chosen = groups if group_query else ids
                    results = [query(args.prometheus, fill(expr, args.job, chosen.get(role))) for role in chosen]
                    ok = any(results)
                else:
                    ok = bool(query(args.prometheus, fill(expr, args.job)))
                live += 1
                if not ok:
                    fail(f"live: {name}: {where}: no data for {fill(expr, args.job)}")
        print(f"live: {live} queries returned data"
              + (f", {skipped} connection-group queries skipped (no group)" if skipped else ""))

    if failures:
        print(f"{len(failures)} dashboard checks failed")
        return 1
    print("all dashboard checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
