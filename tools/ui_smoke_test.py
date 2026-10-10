#!/usr/bin/env python3
"""Browser smoke test of the built-in web UI.

Starts the dashboard demo (real SRT streams through impairing relays), loads
the UI in headless Chrome or Chromium and checks the rendered DOM:

- the fleet view lists every endpoint from the live feed, worst first;
- the remote-4g stream (RTT above its latency) shows latency advice;
- a source view shows the verdict, the advice, the tiles and four charts;
- a connection group (demo --group backup) is one endpoint: its paths follow
  its row, it shows how many paths are usable, and its view lists the paths
  (skipped when the SRT library has no groups);
- with a token, the UI asks for it, and opens with #token=...

Usage: ui_smoke_test.py --demo PATH --browser PATH
"""

import argparse
import json
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.request


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def dump_dom(browser, url, budget_ms=6000):
    result = subprocess.run(
        [browser, "--headless=new", "--no-sandbox", "--disable-gpu", "--no-first-run",
         "--disable-dev-shm-usage", f"--virtual-time-budget={budget_ms}", "--dump-dom", url],
        capture_output=True, text=True, timeout=90)
    return result.stdout


def snapshot(port, token=None):
    request = urllib.request.Request(f"http://127.0.0.1:{port}/api/v1/snapshot")
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request, timeout=5) as response:
        return json.load(response)


failures = []


def check(condition, message):
    print(("ok   " if condition else "FAIL ") + message)
    if not condition:
        failures.append(message)


class Demo:
    def __init__(self, path, *args):
        self.port = free_port()
        self.process = subprocess.Popen(
            [path, "--port", str(self.port), "--streams", "3", "--duration", "120", *args],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        line = self.process.stdout.readline()
        if "dashboard:" not in line:
            raise RuntimeError(f"demo did not start: {line}{self.process.stdout.read()}")
        self.group = None
        if "--group" in args:
            self.group = self.process.stdout.readline().strip().removeprefix("group: ")
        # Keep reading the demo's output, so SRT logs never block it.
        self.drain = threading.Thread(target=lambda: [None for _ in self.process.stdout], daemon=True)
        self.drain.start()

    def stop(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.process.kill()


def wait_for_advice(port, token=None, timeout=45):
    deadline = time.time() + timeout
    while time.time() < deadline:
        data = snapshot(port, token)
        for source in data["sources"]:
            health = source.get("health") or {}
            if source["labels"].get("stream") == "remote-4g" and health.get("knob") == "latency":
                return data
        time.sleep(1)
    return snapshot(port, token)


def fleet_rows(dom):
    """(is_path, endpoint name, verdict, role) of every fleet row, in order."""
    rows = []
    for match in re.finditer(r'<tr tabindex="0"([^>]*)>(.*?)</tr>', dom, re.S):
        attrs, body = match.groups()
        verdict = re.search(r'<td class="status v-([a-z_]+)"', body)
        name = re.search(r'<div class="endpoint-name">(.*?)</div>', body, re.S)
        role = re.search(r'<span class="role">([a-z]+)</span>', body)
        rows.append(("path-row" in attrs, re.sub(r"<[^>]+>", "", name.group(1)) if name else "",
                     verdict.group(1) if verdict else "", role.group(1) if role else ""))
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--demo", required=True)
    parser.add_argument("--browser", required=True)
    args = parser.parse_args()

    demo = Demo(args.demo, "--group", "backup")
    try:
        data = wait_for_advice(demo.port)
        grouped = demo.group == "backup"
        if not grouped:
            print(f"skip group checks: group {demo.group}")
        # Six plain endpoints; with the group two more (sender and receiver
        # groups) and their four paths as rows.
        endpoints = 8 if grouped else 6
        total = 12 if grouped else 6
        base = f"http://127.0.0.1:{demo.port}/"
        for attempt in range(3):
            dom = dump_dom(args.browser, base)
            rows = fleet_rows(dom)
            if 'data-state="live"' in dom and len(rows) == total:
                break
        check(len(rows) == len(data["sources"]) == total, f"fleet lists all {total} rows (found {len(rows)})")
        check('data-state="live"' in dom, "fleet follows the live feed")
        check(">null<" not in dom and ">undefined<" not in dom and "NaN" not in dom,
              "no null, undefined or NaN in the page")
        rank = {"failing": 0, "at_risk": 1, "watch": 2, "unknown": 3, "ok": 4}
        shown = [rank.get(v, -1) for is_path, _, v, _ in rows if not is_path]
        check(len(shown) == endpoints and shown == sorted(shown), f"worst endpoint first (ranks {shown})")
        check("aise the latency to" in dom, "remote-4g shows latency advice (as a change, or as a hint at Watch)")
        check(re.search(rf"<h1>(\d+ of {endpoints} endpoints needs? attention|{endpoints} endpoints: no action needed)</h1>",
                        dom) is not None, f"headline counts {endpoints} endpoints, paths not on their own")
        if grouped:
            paths = [i for i, row in enumerate(rows) if row[0]]
            check(len(paths) == 4, f"four path rows (found {len(paths)})")
            # Each group row is followed by exactly its own paths: lte, sat,
            # with the group's role.
            under = {}
            owner = None
            for is_path, name, _, role in rows:
                if not is_path:
                    owner = (name, role)
                    continue
                under.setdefault(owner, []).append((name, role))
            expected_under = {("bonded-1", r): [("↳ lte", r), ("↳ sat", r)] for r in ("sender", "receiver")}
            check(under == expected_under, f"each group's own paths follow its row ({under})")
            check(len(re.findall(r'class="chip paths( short)?"[^>]*>[12]/2 paths<', dom)) == 2,
                  "group rows show their usable paths")
            group = next(s for s in data["sources"] if s["labels"].get("stream") == "bonded-1"
                         and s["role"] == "sender" and "path" not in s["labels"])
            gdom = dump_dom(args.browser, f"{base}#/source/{group['id']}")
            names = re.findall(r'<td class="path-name">([^<]*)</td>', gdom)
            check(sorted(names) == ["lte", "sat"], f"group view lists its paths ({names})")
            check("<h2>Paths</h2>" in gdom and "Own grade" in gdom, "group view: paths section with own grades")
            path = next(s for s in data["sources"] if s["labels"].get("path") == "sat" and s["role"] == "sender")
            pdom = dump_dom(args.browser, f"{base}#/source/{path['id']}")
            check(f'href="#/source/{group["id"]}"' in pdom and "connection group" in pdom
                  and "bonded-1 · path sat" in pdom, "path view links back to its group")

        bad = next(s for s in data["sources"] if s["labels"]["stream"] == "remote-4g" and s["role"] == "receiver")
        dom = dump_dom(args.browser, f"{base}#/source/{bad['id']}")
        check('class="card hero v-' in dom and "aise the latency to" in dom, "source view: verdict and advice")
        check(dom.count('class="card chart"') == 4, "source view: four charts")
        check(re.search(r'<path class="line s1" d="M', dom) is not None, "source view: charts drawn")
        check(dom.count('class="card tile"') >= 6, "source view: current values")
        check("Verdict over time" in dom and 'class="card indicator' in dom, "source view: timeline and indicators")
    finally:
        demo.stop()

    demo = Demo(args.demo, "--token", "smoke-token")
    try:
        wait_for_advice(demo.port, "smoke-token", timeout=5)
        base = f"http://127.0.0.1:{demo.port}/"
        dom = dump_dom(args.browser, base, 3000)
        check("Access token required" in dom and "endpoint-name" not in dom, "token: the UI asks for it")
        # A slow runner may need a second look before the feed is up.
        for attempt in range(3):
            dom = dump_dom(args.browser, f"{base}#token=smoke-token", 8000)
            state = re.search(r'data-state="([a-z]+)"', dom)
            rows = len(re.findall(r'<tr tabindex="0"', dom))
            if state and state.group(1) == "live" and rows == 6:
                break
        check(state is not None and state.group(1) == "live" and rows == 6,
              f"token: #token= opens the dashboard and the feed (link {state.group(1) if state else None}, {rows} rows)")
        check("smoke-token" not in dom, "token: not left in the page")
    finally:
        demo.stop()

    if failures:
        print(f"{len(failures)} UI smoke checks failed")
        return 1
    print("all UI smoke checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
