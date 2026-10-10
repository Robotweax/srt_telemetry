#!/usr/bin/env python3
"""End-to-end test of the observability path: exporter -> Prometheus,
OTLP exporter -> OpenTelemetry Collector -> Prometheus, alert rules, and the
Grafana dashboards.

It starts the dashboard demo (real SRT streams through impairing relays),
an OpenTelemetry Collector, Prometheus with the alert rules and Grafana with
the provisioned dashboards, all on loopback in a temporary directory, then
checks:
- Prometheus scrapes the demo (job srt) and the collector (job otel);
- the collector receives the OTLP export (srt_* families from job otel,
  with the resource attributes the demo sets), including the connection
  group's paths (the demo runs with --group backup);
- SRTStreamFailing fires for remote-4g and uplink-3, with advice in the
  description, and never for studio-a;
- every dashboard query returns data (tools/check_dashboards.py);
- Grafana loads both dashboards and answers a query through the data source.

Usage:
  observability_e2e.py --demo BUILD/robotweax_srt_telemetry_public_srt_dashboard_demo
      --prometheus PATH --otelcol PATH --grafana-home DIR
      [--screenshots DIR] [--keep]
The demo needs LD_LIBRARY_PATH for its SRT library, as for the tests.
"""

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OBS = os.path.join(ROOT, "observability")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def get(url, timeout=10):
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return response.read()


def get_json(url, data=None):
    request = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"} if data else {})
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.load(response)


def wait(what, check, timeout):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            result = check()
            if result:
                return result
        except Exited:
            raise
        except Exception as error:  # services still starting
            last = error
        time.sleep(1)
    raise RuntimeError(f"timed out waiting for {what}" + (f" ({last})" if last else ""))


class Exited(Exception):
    pass


class Stack:
    def __init__(self, work):
        self.work = work
        self.processes = []

    def start(self, name, command, cwd=None, env=None):
        log = open(os.path.join(self.work, name + ".log"), "w")
        process = subprocess.Popen(command, cwd=cwd or self.work, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL, start_new_session=True)
        self.processes.append((name, process))
        return process

    def check_running(self):
        for name, process in self.processes:
            if process.poll() is not None:
                tail = open(os.path.join(self.work, name + ".log")).read()[-2000:]
                raise Exited(f"{name} exited with {process.returncode}:\n{tail}")

    def stop(self):
        for _, process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
        for _, process in self.processes:
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--demo", required=True)
    parser.add_argument("--prometheus", required=True)
    parser.add_argument("--otelcol", required=True)
    parser.add_argument("--grafana-home", required=True)
    parser.add_argument("--screenshots")
    parser.add_argument("--keep", action="store_true", help="keep the work directory")
    args = parser.parse_args()
    # The services run in the work directory: resolve the paths first.
    for name in ("demo", "prometheus", "otelcol", "grafana_home"):
        setattr(args, name, os.path.abspath(getattr(args, name)))

    work = tempfile.mkdtemp(prefix="srt-observability-e2e-")
    ports = {name: free_port() for name in ("demo", "otlp", "otel_prom", "prometheus", "grafana")}
    prometheus = f"http://127.0.0.1:{ports['prometheus']}"
    grafana = f"http://127.0.0.1:{ports['grafana']}"
    stack = Stack(work)
    failures = []

    def check(condition, message):
        print(("ok   " if condition else "FAIL ") + message)
        if not condition:
            failures.append(message)

    try:
        # OpenTelemetry Collector: the example configuration on other ports.
        collector = open(os.path.join(OBS, "otel-collector", "config.yaml"), encoding="utf-8").read()
        collector = collector.replace("127.0.0.1:4318", f"127.0.0.1:{ports['otlp']}")
        collector = collector.replace("127.0.0.1:8889", f"127.0.0.1:{ports['otel_prom']}")
        # The collector's own metrics would bind port 8888.
        collector = collector.replace("service:\n", "service:\n  telemetry:\n    metrics:\n      level: none\n")
        write(os.path.join(work, "otelcol.yaml"), collector)
        stack.start("otelcol", [args.otelcol, "--config", os.path.join(work, "otelcol.yaml")])

        # Prometheus with the alert rules.
        shutil.copy(os.path.join(OBS, "prometheus", "srt-telemetry-alerts.yml"), work)
        write(os.path.join(work, "prometheus.yml"), f"""global:
  scrape_interval: 2s
  evaluation_interval: 2s
rule_files: [srt-telemetry-alerts.yml]
scrape_configs:
  - job_name: srt
    static_configs: [{{targets: ["127.0.0.1:{ports['demo']}"]}}]
  - job_name: otel
    static_configs: [{{targets: ["127.0.0.1:{ports['otel_prom']}"]}}]
""")
        stack.start("prometheus", [args.prometheus, "--config.file=prometheus.yml", "--storage.tsdb.path=data",
                                   f"--web.listen-address=127.0.0.1:{ports['prometheus']}"])

        # Grafana with the provisioned dashboards and data source.
        graf = os.path.join(work, "grafana")
        write(os.path.join(graf, "grafana.ini"), f"""[paths]
data = {graf}/data
logs = {graf}/logs
plugins = {graf}/plugins
provisioning = {graf}/provisioning
[server]
http_addr = 127.0.0.1
http_port = {ports['grafana']}
[auth.anonymous]
enabled = true
org_role = Admin
[analytics]
reporting_enabled = false
check_for_updates = false
check_for_plugin_updates = false
[news]
news_feed_enabled = false
""")
        provider = open(os.path.join(OBS, "grafana", "provisioning", "dashboards", "srt-telemetry.yaml")).read()
        provider = provider.replace("/var/lib/grafana/dashboards/srt-telemetry",
                                    os.path.join(OBS, "grafana", "dashboards"))
        write(os.path.join(graf, "provisioning", "dashboards", "srt.yaml"), provider)
        source = open(os.path.join(OBS, "grafana", "provisioning", "datasources", "prometheus.yaml")).read()
        source = source.replace("http://localhost:9090", prometheus) + "    uid: e2e-prometheus\n"
        write(os.path.join(graf, "provisioning", "datasources", "prometheus.yaml"), source)
        stack.start("grafana", [os.path.join(args.grafana_home, "bin", "grafana"), "server",
                                "--homepath", args.grafana_home, "--config", os.path.join(graf, "grafana.ini")],
                    env=dict(os.environ, LANG="en_US.UTF-8"))

        # The demo, scraped and exporting OTLP every 2 s.
        stack.start("demo", [args.demo, "--port", str(ports["demo"]), "--otlp",
                             f"http://127.0.0.1:{ports['otlp']}/v1/metrics", "--otlp-interval", "2000",
                             "--group", "backup"])

        def targets_up():
            stack.check_running()
            active = get_json(f"{prometheus}/api/v1/targets")["data"]["activeTargets"]
            return {t["labels"]["job"] for t in active if t["health"] == "up"} >= {"srt", "otel"}

        wait("Prometheus targets srt and otel up", targets_up, 90)
        check(True, "Prometheus scrapes the demo and the collector")

        def query(expr):
            url = f"{prometheus}/api/v1/query?" + urllib.parse.urlencode({"query": expr})
            return get_json(url)["data"]["result"]

        # The first scrapes can catch the collector between two exports.
        otlp = wait("OTLP metrics in the collector",
                    lambda: (lambda r: r if len(r) >= 30 else None)(
                        query('count by (__name__) ({job="otel",__name__=~"srt_.+"})')), 60)
        check(True, f"collector receives the OTLP export ({len(otlp)} srt_* families)")
        resource = wait("OTLP verdicts", lambda: query(
            'srt_health_verdict{job="otel",service_name="srt-dashboard-demo",service_instance_id="demo-1",'
            'otel_scope_name="robotweax.srt_telemetry"}'), 60)
        check(len(resource) >= 10, f"OTLP verdicts carry the resource attributes ({len(resource)} series)")
        paths = wait("OTLP connection-group paths", lambda: query(
            'srt_group_usable_paths{job="otel",stream="bonded-1"}'), 60)
        check(len(paths) == 2, f"OTLP carries the group's usable paths ({len(paths)} groups)")

        expected = {(stream, role) for stream in ("remote-4g", "uplink-3") for role in ("sender", "receiver")}

        def failing():
            stack.check_running()
            alerts = get_json(f"{prometheus}/api/v1/alerts")["data"]["alerts"]
            firing = {(a["labels"].get("stream"), a["labels"].get("role")): a for a in alerts
                      if a["labels"]["alertname"] == "SRTStreamFailing" and a["state"] == "firing"
                      and a["labels"].get("job") == "srt"}
            return firing if expected <= set(firing) else None

        firing = wait("SRTStreamFailing for remote-4g and uplink-3", failing, 150)
        for key in sorted(expected):
            description = firing[key]["annotations"].get("description", "")
            check("Advice: " in description, f"SRTStreamFailing {key[0]} {key[1]}: {description}")
        studio = query('max_over_time(ALERTS{alertname=~"SRTStreamFailing|SRTStreamAtRisk",stream="studio-a"}[1h])')
        check(not studio, "no Failing or At risk alert for studio-a")

        result = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "check_dashboards.py"),
                                 "--prometheus", prometheus, "--groups"], capture_output=True, text=True)
        print(result.stdout, end="")
        check(result.returncode == 0, "every dashboard query returns data")

        wait("Grafana", lambda: get_json(f"{grafana}/api/health").get("database") == "ok", 90)
        found = wait("provisioned dashboards",
                     lambda: {d["uid"] for d in get_json(f"{grafana}/api/search?type=dash-db")}
                     >= {"srt-fleet", "srt-endpoint"}, 60)
        check(bool(found), "Grafana provisions srt-fleet and srt-endpoint")
        for uid in ("srt-fleet", "srt-endpoint"):
            dashboard = get_json(f"{grafana}/api/dashboards/uid/{uid}")["dashboard"]
            check(len(dashboard.get("panels", [])) >= 8, f"Grafana loads {uid} ({len(dashboard['panels'])} panels)")
        now = int(time.time() * 1000)
        body = json.dumps({"from": str(now - 300000), "to": str(now), "queries": [{
            "refId": "A", "datasource": {"type": "prometheus", "uid": "e2e-prometheus"},
            "expr": 'count(srt_health_verdict{job="srt"})', "instant": True}]}).encode()
        frames = get_json(f"{grafana}/api/ds/query", body)["results"]["A"].get("frames", [])
        check(bool(frames), "Grafana queries Prometheus through the provisioned data source")

        if args.screenshots:
            shots(args.screenshots, grafana, ports["demo"])
    except Exception as error:
        failures.append(str(error))
        print(f"FAIL {error}")
    finally:
        stack.stop()
        if args.keep or failures:
            print(f"work directory (logs): {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)

    if failures:
        print(f"{len(failures)} observability checks failed")
        return 1
    print("all observability checks passed")
    return 0


def shots(directory, grafana, demo_port):
    from playwright.sync_api import sync_playwright
    os.makedirs(directory, exist_ok=True)
    pages = {
        "grafana-fleet": f"{grafana}/d/srt-fleet/srt-fleet?orgId=1&from=now-15m&to=now&kiosk",
        "grafana-endpoint": f"{grafana}/d/srt-endpoint/srt-endpoint?orgId=1&from=now-15m&to=now"
                            f"&var-job=srt&var-instance=127.0.0.1:{demo_port}&kiosk",
    }
    with sync_playwright() as p:
        browser = p.chromium.launch()
        context = browser.new_context(viewport={"width": 1600, "height": 1300}, locale="en-US", color_scheme="dark")
        page = context.new_page()
        for name, url in pages.items():
            page.goto(url)
            page.wait_for_timeout(8000)
            page.screenshot(path=os.path.join(directory, name + ".png"), full_page=True)
        browser.close()


if __name__ == "__main__":
    sys.exit(main())
