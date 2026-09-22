#!/usr/bin/env python3
"""A worker for docs/worker-protocol.md that fronts a Python Arrow consumer.

    python abi_worker.py --consumer pyarrow|dataprof --cases <list> --results <jsonl>

One consumer per process; the coordinator starts this script once per consumer.
The two it fronts are a differential pair through each one's production import
path, with no test hook in between:

    pyarrow    Arrow C++'s ImportSchema / ImportArray, via the PyCapsule interface
    dataprof   arrow-rs `from_ffi`, via dataprof.profile() taking the capsules
               itself (dataprof#608)

pyarrow re-exports what it imported, so its lines carry a `received` digest and
a silent divergence in its import is visible. dataprof hands back a profile, not
an array, so it reports no `received` at all -- absent, never a copy of `sent`
(docs/worker-protocol.md, the `digest` object).

Neither wheel is sanitizer-instrumented, and the header says `sanitizers: []`.
That is a statement about what this run could have detected, not a formality:
a crash here is observed, an out-of-bounds read that does not crash is not.

The contract that matters, as in the C runtime: a result line per case, flushed
before the next case starts, so a crash is attributable to the case that caused
it. And catch BaseException around the consumer, not Exception -- pyo3 derives
PanicException from BaseException precisely so that it is not swallowed, and a
Rust panic across FFI has to be a result line rather than a dead worker.

Fault injection, for testing this file and nothing else:

    ABI_PYWORKER_FAULT=panic|segv   ABI_PYWORKER_FAULT_AT=<1-based case index>

`panic` raises a BaseException that is not an Exception, which is what a pyo3
PanicException is; `segv` kills the interpreter with a real SIGSEGV. The header
records the injection, so a run with it on cannot be mistaken for a measurement.
"""

from __future__ import annotations

import json
import os
import platform
import sys
from pathlib import Path
from typing import IO, Any

WORKER_PROTOCOL = 1
USAGE = (
    "usage: abi_worker.py --consumer pyarrow|dataprof --cases <path> --results <path>\n"
    "see docs/worker-protocol.md\n"
)
CONSUMERS = ("pyarrow", "dataprof")


class InjectedPanic(BaseException):
    """Stands in for pyo3's PanicException: a BaseException, not an Exception."""


def parse_args(argv: list[str]) -> dict[str, str] | None:
    """The protocol's arguments and nothing else; anything unrecognized is refused.

    An adapter that silently ignores an argument it was given reports on a run
    nobody asked for -- the same rule as adapters/common/worker.c.
    """
    out: dict[str, str] = {}
    i = 0
    while i < len(argv):
        flag = argv[i]
        if flag in ("--cases", "--results", "--consumer") and i + 1 < len(argv):
            out[flag[2:]] = argv[i + 1]
            i += 2
            continue
        return None
    if "cases" not in out or "results" not in out or out.get("consumer") not in CONSUMERS:
        return None
    return out


def read_cases(path: str) -> list[str]:
    """One path per line, blank lines and `#` comments skipped.

    Decoded strictly: a path that is not UTF-8 cannot be echoed back byte for
    byte in a JSON line, and the coordinator matches cases by exact string, so
    such a list is refused rather than reported as cases nobody assigned. There
    is no line-length limit to enforce -- nothing here truncates.
    """
    text = Path(path).read_bytes().decode("utf-8")
    cases = []
    for raw in text.split("\n"):
        line = raw.rstrip("\r")
        if line and not line.startswith("#"):
            cases.append(line)
    return cases


def host_arch() -> str:
    machine = platform.machine().lower()
    return {"amd64": "x86_64", "x86_64": "x86_64", "arm64": "aarch64"}.get(machine, machine)


def host_os() -> str:
    return {"win32": "windows", "linux": "linux", "darwin": "macos"}.get(sys.platform, sys.platform)


def emit(out: IO[str], obj: dict[str, Any]) -> None:
    out.write(json.dumps(obj, ensure_ascii=False, separators=(",", ":")) + "\n")
    out.flush()


def observer(case: Any) -> dict[str, Any]:
    """The same block the C runtime writes, from the case's own lifecycle log.

    `outstanding` is bytes not yet freed *at this instant*, not a leak verdict
    (issue #6); `capsules_outstanding` says whether a live capsule could still
    free them, which is the other half of that verdict.
    """
    life = case.lifecycle()
    events = life["events"]
    by_consumer = any(
        e["by_consumer"] and e["kind"] in ("SCHEMA_RELEASE_ENTER", "ARRAY_RELEASE_ENTER")
        for e in events
    )
    return {
        "violations": life["violations"],
        "bytes_allocated": life["bytes_allocated"],
        "bytes_freed": life["bytes_freed"],
        "blocks_allocated": life["blocks_allocated"],
        "blocks_freed": life["blocks_freed"],
        "outstanding": life["bytes_allocated"] - life["bytes_freed"],
        "released_by_consumer": by_consumer,
        "harness_released": any(e["kind"] == "HARNESS_RELEASED" for e in events),
        "capsules_outstanding": life["capsules_outstanding"],
        "events": len(events),
        "events_dropped": life["events_dropped"],
    }


def describe(exc: BaseException) -> str:
    return f"{type(exc).__module__}.{type(exc).__qualname__}: {exc}"


def consume(consumer: str, case: Any, digest: dict[str, Any]) -> str:
    """Hand the case to the consumer; returns the `detail` of an accepted line."""
    import _abicase

    if consumer == "pyarrow":
        import pyarrow

        batch = pyarrow.record_batch(case)
        try:
            digest["received"] = _abicase.digest(*batch.__arrow_c_array__())
        except ValueError as exc:
            digest["received_error"] = str(exc)
        return f"{batch.num_rows} rows x {batch.num_columns} cols"

    import dataprof

    report = dataprof.profile(case, name="abicase")
    return f"{report.rows} rows x {report.columns} cols, profiled"


def run_case(consumer: str, path: str, index: int, fault: tuple[str, int] | None) -> dict[str, Any]:
    import _abicase

    try:
        case = _abicase.load(path)
    except BaseException as exc:  # noqa: BLE001 -- a harness failure is a result line
        return {"case": path, "id": "", "status": "error", "detail": describe(exc)}
    case_id = case.case_id()

    digest: dict[str, Any] = {"ver": 1}
    try:
        digest["sent"] = case.digest()
    except ValueError as exc:
        digest["sent_error"] = str(exc)

    panicked = False
    try:
        if fault and fault[1] == index:
            if fault[0] == "panic":
                raise InjectedPanic("injected: stands in for a pyo3 PanicException")
            import faulthandler

            # A real SIGSEGV, not an exception. Private but long-standing CPython
            # API, used by its own test suite; typeshed does not list it.
            faulthandler._sigsegv()  # type: ignore[attr-defined]
        # BaseException: pyo3's PanicException derives from it so that a Rust
        # panic is not swallowed by `except Exception`, and a consumer that
        # panics has to be a recorded rejection, not the end of the worker.
        detail = consume(consumer, case, digest)
        status = "accepted"
    except BaseException as exc:  # noqa: BLE001 -- see above; load-bearing
        detail, status = describe(exc), "rejected"
        # Not an Exception means a panic crossed the FFI boundary: the worker
        # survived it, so the line says `rejected`, but it is not the clean
        # refusal that word means elsewhere (dataprof#609 was one of these).
        panicked = not isinstance(exc, Exception)
        digest.pop("received", None)
        digest.pop("received_error", None)

    # Every consumer object is out of scope by now, so the capsules have been
    # collected and released what the consumer did not; release_all() then
    # cleans up anything never taken, and logs it as the harness's doing.
    case.release_all()
    line = {
        "case": path,
        "id": case_id,
        "status": status,
        "detail": detail,
        "observer": observer(case),
        "digest": digest,
    }
    if panicked:
        line["panicked"] = True
    del case
    return line


def consumer_version(consumer: str) -> str:
    if consumer == "pyarrow":
        import pyarrow

        return str(pyarrow.__version__)
    import dataprof

    return str(dataprof.__version__)


def parse_fault() -> tuple[str, int] | None:
    mode = os.environ.get("ABI_PYWORKER_FAULT")
    if not mode:
        return None
    if mode not in ("panic", "segv"):
        raise SystemExit(f"ABI_PYWORKER_FAULT={mode!r}: expected panic or segv")
    return mode, int(os.environ.get("ABI_PYWORKER_FAULT_AT", "1"))


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args is None:
        sys.stderr.write(USAGE)
        return 1
    # _abicase lives next to this file; the coordinator runs workers from the
    # repository root, so the directory is not on the path by default.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    try:
        cases = read_cases(args["cases"])
        version = consumer_version(args["consumer"])
        fault = parse_fault()
        out = open(args["results"], "w", encoding="utf-8", newline="\n")  # noqa: SIM115
    except (OSError, UnicodeDecodeError, ImportError) as exc:
        sys.stderr.write(f"error: {describe(exc)}\n")
        return 1

    with out:
        header: dict[str, Any] = {
            "worker_protocol": WORKER_PROTOCOL,
            "consumer": args["consumer"],
            "consumer_version": version,
            "arch": host_arch(),
            "os": host_os(),
            "runtime": f"{platform.python_implementation()} {platform.python_version()}",
            "sanitizers": [],
        }
        if fault:
            header["fault_injection"] = f"{fault[0]}@{fault[1]}"
        emit(out, header)
        for index, path in enumerate(cases, start=1):
            emit(out, run_case(args["consumer"], path, index, fault))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
