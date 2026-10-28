"""Conservative automatic scope; qualification is only explicit dispatch."""
import argparse
import os
import subprocess
from pathlib import PurePosixPath

def native_required(paths):
    # Only ordinary prose is exempt. Security policy and unknown inputs fail closed.
    return not paths or any(not (p.endswith(".md") and (p.startswith("docs/") or p in {"README.md", "CONTRIBUTING.md", "CHANGELOG.md"})) for p in paths)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        assert not native_required(["README.md", "docs/guide.md"])
        for paths in ([], ["SECURITY.md"], ["security/policy.md"], ["src/main.cpp"], ["tests/test.py"], ["baselines/pin.json"], [".github/workflows/ci.yml"], ["unknown"]):
            assert native_required(paths), paths
        print("scope cases passed")
        return
    base, head = os.environ.get("CI_BASE", ""), os.environ["GITHUB_SHA"]
    if base and subprocess.run(["git", "cat-file", "-e", base+"^{commit}"], capture_output=True).returncode == 0:
        paths = subprocess.check_output(["git", "diff", "--name-only", "--no-renames", base, head], text=True).splitlines()
    else:
        paths = []
    native = native_required(paths) or os.environ.get("GITHUB_EVENT_NAME") == "workflow_dispatch"
    with open(os.environ["GITHUB_OUTPUT"], "a") as out:
        out.write("native="+str(native).lower()+"\n")
    with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as out:
        out.write(f"Candidate: `{head}`. Native smoke selected: `{native}`. Full qualification is unattempted unless explicitly dispatched; skipped suites are not passing evidence.\n")

if __name__ == "__main__":
    main()
