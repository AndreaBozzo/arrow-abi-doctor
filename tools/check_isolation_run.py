#!/usr/bin/env python3
"""Check a coordinator run report against what the run was set up to do.

Issue #1's acceptance condition is checked in `coordinator/tests/isolation.rs`
against the library. This checks the *shipped binary*, over the real corpus, by
reading the artifact it wrote rather than the output it printed: a CI step that
greps its own echoed log is testing the echo.

Usage:
    check_isolation_run.py <run.json> --cases N --crash-at K

`--crash-at` is the 1-based case index the faulty worker was told to fault on,
so the expected number of surviving result lines is K - 1. That subtraction is
the whole point of the per-case flush, and stating it here means a regression in
it fails the run rather than quietly reducing the count.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


def fail(problems: list[str]) -> int:
    for problem in problems:
        print(f"MISMATCH: {problem}")
    return 1


def check(report: dict[str, Any], cases: int, crash_at: int) -> list[str]:
    problems: list[str] = []
    workers = {w["name"]: w for w in report["workers"]}

    for name in ("null", "faulty"):
        if name not in workers:
            problems.append(f"no worker named {name!r} in the report")
    if problems:
        return problems

    # The surviving consumer. Every case reported, cleanly, or the run proved
    # nothing about isolation: a crash that takes nobody else down is only
    # interesting if somebody else was there.
    null = workers["null"]
    if null["termination"] != {"kind": "exited", "code": 0}:
        problems.append(f"null worker did not exit cleanly: {null['termination']}")
    if null["cases_reported"] != cases:
        problems.append(f"null reported {null['cases_reported']} of {cases} cases")
    if null["accepted"] != cases:
        problems.append(f"null accepted {null['accepted']} of {cases} cases")
    if null["protocol_errors"]:
        problems.append(f"null broke the worker protocol: {null['protocol_errors']}")

    faulty = workers["faulty"]
    termination = faulty["termination"]
    # POSIX reports the fault as a signal; Windows as the exception code for an
    # access violation. Both are the same event.
    segfault_shapes = (
        {"kind": "signal", "signal": 11},
        {"kind": "exception", "code": 0xC0000005},
    )
    if termination not in segfault_shapes:
        problems.append(f"faulty worker did not segfault: {termination}")
    if faulty["cases_reported"] != crash_at - 1:
        problems.append(
            f"faulty reported {faulty['cases_reported']} cases, expected {crash_at - 1} "
            "to survive the fault -- a result line is not being flushed per case"
        )
    if not faulty["suspect"]:
        problems.append("the crashing case was not attributed to a suspect")
    if faulty["protocol_errors"]:
        problems.append(f"a crash was recorded as a protocol error: {faulty['protocol_errors']}")

    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="path to run.json")
    parser.add_argument("--cases", type=int, required=True)
    parser.add_argument("--crash-at", type=int, required=True)
    args = parser.parse_args()

    report = json.loads(args.report.read_text(encoding="utf-8"))
    problems = check(report, args.cases, args.crash_at)
    if problems:
        return fail(problems)

    print(
        f"ok  worker isolation: faulty crashed on case {args.crash_at} and was recorded, "
        f"null still reported {args.cases}/{args.cases}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
