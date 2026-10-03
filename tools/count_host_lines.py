#!/usr/bin/env python3
"""Counts the host lines of a reference integration: every non-blank line a
patch adds to the host application (code and build files alike, comments
included; a changed line counts as added). The Phase 3 gate is 30 or fewer.

Usage: count_host_lines.py PATCH [--max 30]
"""

import argparse
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("patch")
    parser.add_argument("--max", type=int, default=30)
    args = parser.parse_args()
    per_file = {}
    current = None
    for line in open(args.patch, encoding="utf-8"):
        if line.startswith("+++ "):
            current = line[4:].strip().removeprefix("b/")
            per_file.setdefault(current, 0)
        elif line.startswith("+") and current is not None and line[1:].strip():
            per_file[current] += 1
    total = sum(per_file.values())
    for name, count in per_file.items():
        print(f"{count:4d}  {name}")
    print(f"{total:4d}  host lines (gate: {args.max} or fewer)")
    if not per_file:
        print("no added lines found: not a unified diff?")
        return 1
    return 0 if total <= args.max else 1


if __name__ == "__main__":
    sys.exit(main())
