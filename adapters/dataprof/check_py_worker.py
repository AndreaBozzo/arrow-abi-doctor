#!/usr/bin/env python3
"""Check the Python worker under the coordinator: a real pair, a panic, a crash.

`abi_worker.py` makes pyarrow and dataprof a differential pair (issue #17). This
runs the shipped coordinator binary against it and reads the artifacts it wrote,
in three runs:

1. **The pair.** Both consumers over Corpus A: every case reported, no harness
   errors, no protocol errors; pyarrow hands back a `received` digest on every
   accepted line and dataprof never does. Verdicts are printed, not asserted --
   a consumer's result is a finding, and a check that pins it would fail the day
   the consumer is fixed.
2. **A panic.** The worker is told to raise, on case 3, a BaseException that is
   not an Exception -- what a pyo3 PanicException is. It has to become a
   `rejected` line marked `panicked`, and the worker has to finish the run. A
   worker catching `Exception` instead dies here.
3. **A crash.** The worker is told to die of a real SIGSEGV on case 3. The
   coordinator has to record the abnormal termination, the two lines before it,
   and case 3 as the suspect. A worker that buffers its output loses those lines.

Needs pyarrow, dataprof, the `_abicase` extension, corpus/a, and a built
coordinator (`cargo build -p abi-coordinator`, or ABI_COORDINATOR=<path>).
"""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
MANIFEST_ROWS = [
    line.split("\t")
    for line in (ROOT / "corpus" / "a" / "manifest.tsv").read_text(encoding="utf-8").splitlines()
    if line and not line.startswith("#")
]
LIFECYCLE = {row[0]: row[8] for row in MANIFEST_ROWS}
DATA_FREE = ("early-release", "EOF")
STREAM_LIFECYCLES = ("streamed", *DATA_FREE)
STREAM_CELLS = sum(life in STREAM_LIFECYCLES for life in LIFECYCLE.values())
WORKER = "adapters/dataprof/abi_worker.py"
# The fault runs: FAULT_CASES cases, the fault injected on the FAULT_AT-th.
FAULT_CASES = 5
FAULT_AT = 3


def find_coordinator() -> pathlib.Path:
    override = os.environ.get("ABI_COORDINATOR")
    if override:
        return pathlib.Path(override)
    name = "abi-coordinator" + (".exe" if os.name == "nt" else "")
    target = pathlib.Path(os.environ.get("CARGO_TARGET_DIR", ROOT / "target"))
    for profile in ("debug", "release"):
        if (target / profile / name).is_file():
            return target / profile / name
    raise SystemExit(f"no {name} under {target}; cargo build -p abi-coordinator")


def run(
    consumers: list[str], cases: str, out: pathlib.Path, limit: int | None, env: dict[str, str]
) -> dict[str, Any]:
    cmd = [str(find_coordinator())]
    for consumer in consumers:
        cmd += ["--worker", f"{consumer}={sys.executable}"]
        for arg in (WORKER, "--consumer", consumer):
            cmd += ["--worker-arg", f"{consumer}={arg}"]
    cmd += ["--cases", cases, "--out", str(out)]
    if limit is not None:
        cmd += ["--limit", str(limit)]
    subprocess.run(cmd, cwd=ROOT, env=dict(os.environ, **env), check=True, capture_output=True)
    report: dict[str, Any] = json.loads((out / "run.json").read_text(encoding="utf-8"))
    return report


def lines(worker: dict[str, Any]) -> list[dict[str, Any]]:
    text = pathlib.Path(worker["results_path"]).read_text(encoding="utf-8")
    return [json.loads(line) for line in text.splitlines()]


def check_pair(tmp: pathlib.Path) -> list[str]:
    problems: list[str] = []
    report = run(["pyarrow", "dataprof"], "corpus/a", tmp / "pair", None, {})
    for w in report["workers"]:
        name = w["name"]
        if w["termination"] != {"kind": "exited", "code": 0}:
            problems.append(f"{name}: did not exit cleanly: {w['termination']}")
        if w["cases_reported"] != w["cases_assigned"] or w["not_run"]:
            problems.append(f"{name}: {w['cases_reported']}/{w['cases_assigned']} reported")
        if w["protocol_errors"]:
            problems.append(f"{name}: protocol {w['protocol_errors']}")
        header, *rows = lines(w)
        # dataprof takes no stream-only producer, so its worker refuses every
        # stream sequence by name -- those, and nothing else, may be errors.
        # pyarrow runs them all.
        expected = STREAM_CELLS if name == "dataprof" else 0
        refused = [r for r in rows if r["status"] == "error"]
        if len(refused) != expected or any(
            "not performed by the dataprof worker" not in r["detail"]
            or LIFECYCLE[r["id"]] not in STREAM_LIFECYCLES
            for r in refused
        ):
            problems.append(f"{name}: {len(refused)} errors, expected {expected} stream refusals")
        if header.get("sanitizers") != [] or "fault_injection" in header:
            problems.append(f"{name}: header misstates the run: {header}")
        for row in rows:
            digest = row.get("digest", {})
            data_free = LIFECYCLE[row["id"]] in DATA_FREE
            if "sent" not in digest and not data_free:
                problems.append(f"{name} {row['case']}: no sent digest")
            gave_back = "received" in digest or "received_error" in digest
            if name == "dataprof" and gave_back:
                problems.append(f"dataprof {row['case']}: claims to hand back an array")
            if (
                name == "pyarrow"
                and row["status"] == "accepted"
                and not data_free
                and "received" not in digest
            ):
                problems.append(f"pyarrow {row['case']}: accepted with no received digest")
        print(
            f"  {name:<9} {header.get('consumer_version')}: {w['accepted']} accepted, "
            f"{w['rejected']} rejected of {w['cases_assigned']}"
        )
    return problems


def check_panic(tmp: pathlib.Path) -> list[str]:
    env = {"ABI_PYWORKER_FAULT": "panic", "ABI_PYWORKER_FAULT_AT": str(FAULT_AT)}
    report = run(["pyarrow"], "corpus/a", tmp / "panic", FAULT_CASES, env)
    w = report["workers"][0]
    if w["termination"] != {"kind": "exited", "code": 0}:
        return [f"panic: the worker died instead of recording it: {w['termination']}"]
    header, *rows = lines(w)
    problems = []
    if header.get("fault_injection") != f"panic@{FAULT_AT}":
        problems.append(f"panic: header does not record the injection: {header}")
    if len(rows) != FAULT_CASES:
        problems.append(f"panic: {len(rows)} lines, expected {FAULT_CASES}")
    for index, row in enumerate(rows, start=1):
        panicked = row.get("panicked", False)
        if index == FAULT_AT and not (row["status"] == "rejected" and panicked):
            problems.append(f"panic: case {index} is {row['status']}, panicked={panicked}")
        if index != FAULT_AT and (row["status"] != "accepted" or panicked):
            problems.append(f"panic: case {index} is {row['status']}, panicked={panicked}")
    return problems


def check_crash(tmp: pathlib.Path) -> list[str]:
    env = {"ABI_PYWORKER_FAULT": "segv", "ABI_PYWORKER_FAULT_AT": str(FAULT_AT)}
    report = run(["pyarrow"], "corpus/a", tmp / "crash", FAULT_CASES, env)
    w = report["workers"][0]
    problems = []
    # POSIX: signal 11. Windows has no signals; the fault arrives as an exit
    # status the coordinator classifies, and the check is only that it is
    # abnormal -- the SIGSEGV shape is asserted where there is one.
    termination = w["termination"]
    if os.name == "posix" and termination != {"kind": "signal", "signal": 11}:
        problems.append(f"crash: expected SIGSEGV, got {termination}")
    if termination.get("kind") == "exited" and termination.get("code") == 0:
        problems.append("crash: the worker exited cleanly")
    if w["cases_reported"] != FAULT_AT - 1:
        problems.append(
            f"crash: {w['cases_reported']} lines survived, expected {FAULT_AT - 1} -- "
            "a result line is not being flushed per case"
        )
    suspect = w["suspect"] or ""
    cases = (tmp / "crash" / "cases.txt").read_text(encoding="utf-8").split("\n")
    expected = cases[FAULT_AT - 1]
    if suspect != expected:
        problems.append(f"crash: suspect is {suspect!r}, expected case {FAULT_AT} {expected!r}")
    return problems


def main() -> int:
    if not (ROOT / "corpus" / "a" / "manifest.tsv").is_file():
        raise SystemExit("no corpus at corpus/a; run tools/gen_corpus.py first")
    problems: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        base = pathlib.Path(tmp)
        print("pair:")
        problems += check_pair(base)
        problems += check_panic(base)
        problems += check_crash(base)
    for problem in problems:
        print(f"MISMATCH: {problem}")
    if problems:
        return 1
    print("ok  pair ran to completion, a panic became a result line, a crash was attributed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
