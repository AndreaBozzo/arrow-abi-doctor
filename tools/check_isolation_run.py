#!/usr/bin/env python3
"""Check a coordinator run report against what the run was set up to do.

Issue #1's acceptance condition is checked in `coordinator/tests/isolation.rs`
against the library. This checks the *shipped binary*, over the real corpus, by
reading the artifact it wrote rather than the output it printed: a CI step that
greps its own echoed log is testing the echo.

Three independent checks, selected by flag:

    --cases N --crash-at K      isolation: the crash was recorded and the other
                                consumer still finished
    --expect-sanitizers a,b     the header reports the sanitizers the build
                                actually enabled, one name per array element
    --expect-sent-only          the worker digests what it was handed and
                                reports no `received` half

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

# ABI_DIGEST_HEX_SIZE - 1: 128 bits as lowercase hex (libabi/include/abi/digest.h).
DIGEST_HEX_LEN = 32


def check_isolation(report: dict[str, Any], cases: int, crash_at: int) -> list[str]:
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


def check_sanitizers(report: dict[str, Any], worker: str, expected: list[str]) -> list[str]:
    """The header's `sanitizers` is one name per element, not one joined string.

    Only a build with sanitizers on can tell the two apart: on an ordinary build
    the list is empty and every shape looks alike. So this is pointed at a
    sanitizer build, where `["address,undefined"]` and `["address",
    "undefined"]` are visibly different things.
    """
    problems: list[str] = []
    workers = {w["name"]: w for w in report["workers"]}
    if worker not in workers:
        return [f"no worker named {worker!r} in the report"]

    header = workers[worker]["header"]
    if header is None:
        return [f"{worker} wrote no header line"]

    found = header.get("sanitizers")
    if found != expected:
        problems.append(
            f"{worker} header reports sanitizers {found!r}, expected {expected!r} "
            "-- one name per element, not one comma-joined string"
        )
    return problems


def check_sent_only(report: dict[str, Any], worker: str) -> list[str]:
    """The worker digests what it was handed and claims nothing came back.

    The null consumer reads no buffer and returns nothing, so every accepted
    line must carry a computed `sent` and no `received` at all -- not a copy of
    `sent`, which would claim a round trip that never happened
    (docs/worker-protocol.md, the `digest` object).
    """
    workers = {w["name"]: w for w in report["workers"]}
    if worker not in workers:
        return [f"no worker named {worker!r} in the report"]
    problems: list[str] = []
    path = Path(workers[worker]["results_path"])
    lines = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    accepted = [line for line in lines[1:] if line.get("status") == "accepted"]
    if not accepted:
        return [f"{worker} accepted no case, so nothing about its digests was checked"]
    for line in accepted:
        digest = line.get("digest")
        where = f"{worker} {line['case']}"
        if not isinstance(digest, dict) or not isinstance(digest.get("ver"), int):
            problems.append(f"{where}: no versioned digest object")
            continue
        sent = digest.get("sent")
        # A schema-only case (the EOF and early-release lifecycles) has no data
        # to digest, and says so with exactly this reason; nothing else excuses
        # a missing `sent`.
        schema_only = sent is None and digest.get("sent_error") == "no array to digest"
        if not schema_only and (
            not isinstance(sent, dict)
            or not all(
                isinstance(sent.get(k), str) and len(sent[k]) == DIGEST_HEX_LEN
                for k in ("physical", "logical")
            )
        ):
            problems.append(f"{where}: `sent` is not two 32-hex digests: {digest}")
        if "received" in digest or "received_error" in digest:
            problems.append(f"{where}: claims something came back: {digest}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="path to run.json")
    parser.add_argument("--cases", type=int)
    parser.add_argument("--crash-at", type=int)
    parser.add_argument(
        "--worker", default="null", help="worker for --expect-sanitizers / --expect-sent-only"
    )
    parser.add_argument(
        "--expect-sanitizers",
        help="comma-separated names the header must report, e.g. address,undefined",
    )
    parser.add_argument(
        "--expect-sent-only",
        action="store_true",
        help="every accepted line of --worker carries `sent` and no `received`",
    )
    args = parser.parse_args()

    isolation = args.cases is not None and args.crash_at is not None
    if not isolation and args.expect_sanitizers is None and not args.expect_sent_only:
        parser.error(
            "nothing to check: pass --cases with --crash-at, --expect-sanitizers, "
            "or --expect-sent-only"
        )

    report = json.loads(args.report.read_text(encoding="utf-8"))
    problems: list[str] = []
    done: list[str] = []

    if isolation:
        problems += check_isolation(report, args.cases, args.crash_at)
        done.append(
            f"isolation: faulty crashed on case {args.crash_at} and was recorded, "
            f"null still reported {args.cases}/{args.cases}"
        )
    if args.expect_sanitizers is not None:
        expected = [name for name in args.expect_sanitizers.split(",") if name]
        problems += check_sanitizers(report, args.worker, expected)
        done.append(f"header: {args.worker} reports sanitizers {expected}")
    if args.expect_sent_only:
        problems += check_sent_only(report, args.worker)
        done.append(f"digest: {args.worker} reports `sent` on every accepted case, no `received`")

    if problems:
        for problem in problems:
            print(f"MISMATCH: {problem}")
        return 1

    for line in done:
        print(f"ok  {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
