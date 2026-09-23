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


# A case with no CALLSEQ runs the default path, as the C executor does.
DEFAULT_CALLSEQ = [("IMPORT_SCHEMA", 0, 0), ("IMPORT_ARRAY", 0, 0), ("RELEASE_BASE", 0, 0)]
STREAM_OPS = ("STREAM_GET_SCHEMA", "STREAM_GET_NEXT", "STREAM_GET_LAST_ERROR", "EXPECT_EOF")
ARRAY_OPS = ("NOP", "MOVE_STRUCT", "IMPORT_SCHEMA", "IMPORT_ARRAY", "RELEASE_BASE")
# What each consumer can do. Anything else is refused by name before anything
# runs -- never replaced by the default path, which would report a case as run
# when the part that made it a case was skipped. The class C ops need a consumer
# that can be told to misbehave, and neither of these can.
PERFORMED = {
    "pyarrow": (*ARRAY_OPS, "STREAM_GET_SCHEMA", "STREAM_GET_NEXT", "EXPECT_EOF"),
    "dataprof": ARRAY_OPS,
}
# Why, where a consumer cannot perform a whole kind of sequence.
NOT_PERFORMED_BECAUSE = {
    "dataprof": "dataprof.profile() takes __arrow_c_array__ producers and refuses a "
    "stream-only one as an unsupported source type (checked on 0.11.0)",
}


def import_into(consumer: str, case: Any, digest: dict[str, Any]) -> tuple[str, Any]:
    """Hand both base structures over. Returns the detail and what the consumer holds.

    Both consumers take the schema and the array together, through the capsule
    protocol, so this is the IMPORT_ARRAY of a sequence whose IMPORT_SCHEMA only
    armed it.
    """
    import _abicase

    if consumer == "pyarrow":
        import pyarrow

        batch = pyarrow.record_batch(case)
        try:
            digest["received"] = _abicase.digest(*batch.__arrow_c_array__())
        except ValueError as exc:
            digest["received_error"] = str(exc)
        return f"{batch.num_rows} rows x {batch.num_columns} cols", batch

    import dataprof

    report = dataprof.profile(case, name="abicase")
    return f"{report.rows} rows x {report.columns} cols, profiled", report


def inject(fault: tuple[str, int] | None, index: int) -> None:
    if not fault or fault[1] != index:
        return
    if fault[0] == "panic":
        raise InjectedPanic("injected: stands in for a pyo3 PanicException")
    import faulthandler

    # A real SIGSEGV, not an exception. Private but long-standing CPython API,
    # used by its own test suite; typeshed does not list it.
    faulthandler._sigsegv()  # type: ignore[attr-defined]


class SequenceInvalid(Exception):
    """The sequence cannot run as written -- the harness's side, not the consumer's."""


def refusal(consumer: str, names: list[str]) -> str | None:
    """The first op this consumer cannot perform, with why; None when it can do all."""
    refused = next((n for n in names if n not in PERFORMED[consumer]), None)
    if refused is None and "IMPORT_SCHEMA" in names and "IMPORT_ARRAY" not in names:
        refused = "IMPORT_SCHEMA"  # a schema-only handoff: neither consumer takes one
    if refused is None:
        return None
    why = f"{refused}: not performed by the {consumer} worker"
    if refused in STREAM_OPS and consumer in NOT_PERFORMED_BECAUSE:
        why += f" -- {NOT_PERFORMED_BECAUSE[consumer]}"
    return why


class Session:
    """What the consumer holds while one case's call sequence runs.

    Each op is one call to step(). The references are the hold: a consumer
    object kept alive keeps its capsule, and so its structure, alive, and
    dropping it is the consumer's release.
    """

    def __init__(
        self,
        consumer: str,
        case: Any,
        digest: dict[str, Any],
        fault: tuple[str, int] | None,
        index: int,
    ) -> None:
        self.consumer, self.case, self.digest = consumer, case, digest
        self.fault, self.index = fault, index
        self.held: Any = None
        self.reader: Any = None
        self.batch: Any = None
        self.last_eof: bool | None = None
        self.rows = self.batches = 0
        self.detail = "ran its call sequence"

    def step(self, name: str) -> None:
        if name == "MOVE_STRUCT":
            self.case.move()
        elif name == "IMPORT_ARRAY":
            inject(self.fault, self.index)
            self.detail, self.held = import_into(self.consumer, self.case, self.digest)
        elif name == "STREAM_GET_SCHEMA":
            self.open_stream()
        elif name == "STREAM_GET_NEXT":
            self.open_stream()
            self.next_batch()
        elif name == "EXPECT_EOF" and self.last_eof is not True:
            raise SequenceInvalid(
                "EXPECT_EOF: the stream had not answered EOF -- the harness's "
                "producer delivered more than the case holds"
            )
        elif name == "RELEASE_BASE":
            self.drop()

    def open_stream(self) -> None:
        if self.reader is not None:
            return
        import pyarrow

        inject(self.fault, self.index)
        # Takes the stream and calls get_schema() on it.
        self.reader = pyarrow.RecordBatchReader.from_stream(self.case)
        self.detail = "schema only"

    def next_batch(self) -> None:
        import _abicase

        self.batch = None  # done with the last batch, before asking for the next
        try:
            self.batch = self.reader.read_next_batch()
        except StopIteration:
            self.last_eof = True
        else:
            self.last_eof = False
            self.batches += 1
            self.rows += self.batch.num_rows
            if self.batches == 1:
                try:
                    self.digest["received"] = _abicase.digest(*self.batch.__arrow_c_array__())
                except ValueError as exc:
                    self.digest["received_error"] = str(exc)
        self.detail = f"{self.rows} rows in {self.batches} batch(es)" + (
            ", then EOF" if self.last_eof else ""
        )

    def drop(self) -> None:
        self.held = self.reader = self.batch = None


def run_case(consumer: str, path: str, index: int, fault: tuple[str, int] | None) -> dict[str, Any]:
    import _abicase

    try:
        case = _abicase.load(path)
    except BaseException as exc:  # noqa: BLE001 -- a harness failure is a result line
        return {"case": path, "id": "", "status": "error", "detail": describe(exc)}
    case_id = case.case_id()

    # What is handed over, digested before the handoff -- from the array form of
    # the case, which a stream case has too: the data is the same either way.
    digest: dict[str, Any] = {"ver": 1}
    try:
        digest["sent"] = case.digest()
    except ValueError as exc:
        digest["sent_error"] = str(exc)

    ops = case.callseq()
    names = [name for name, _a0, _a1 in ops or DEFAULT_CALLSEQ]
    if any(name in STREAM_OPS for name in names):
        # Executed on the stream form; the array form was only ever digested,
        # never handed over, so its own log judges nothing.
        case.release_all()
        case = _abicase.load(path, True)
    callseq: dict[str, Any] = {
        "outcome": "accepted",
        "defaulted": not ops,
        "op_count": len(names),
        "executed": 0,
        "ops": [],
    }

    panicked = False
    status = "accepted"
    session = Session(consumer, case, digest, fault, index)
    why_not = refusal(consumer, names)
    if why_not is not None:
        callseq["outcome"], status, session.detail = "unsupported", "error", why_not
    else:
        for name in names:
            try:
                session.step(name)
            except SequenceInvalid as exc:
                callseq["outcome"], status, session.detail = "invalid", "error", str(exc)
                break
            except BaseException as exc:  # noqa: BLE001 -- see below; load-bearing
                # BaseException: pyo3's PanicException derives from it so that a
                # Rust panic is not swallowed by `except Exception`, and a
                # consumer that panics has to be a recorded rejection, not the
                # end of the worker. Not an Exception means a panic crossed the
                # FFI boundary: the worker survived it, so the line says
                # `rejected`, but it is not the clean refusal that word means
                # elsewhere (dataprof#609 was one of these).
                callseq["outcome"], status, session.detail = "rejected", "rejected", describe(exc)
                panicked = not isinstance(exc, Exception)
                digest.pop("received", None)
                digest.pop("received_error", None)
                break
            callseq["executed"] += 1
            callseq["ops"].append(name)
    # Whatever the sequence left in the consumer's hands goes with the case.
    session.drop()

    # Every consumer object is gone by now, so the capsules have been collected
    # and released what the consumer did not; release_all() then cleans up
    # anything never taken, and logs it as the harness's doing. Only then is the
    # log complete enough to judge -- and not strictly: the consumers move the
    # structures into their own storage on import, legally and untracked.
    case.release_all()
    line = {
        "case": path,
        "id": case_id,
        "status": status,
        "detail": session.detail,
        "observer": observer(case),
        "digest": digest,
        "callseq": callseq,
        "lifecycle": case.lifecycle_verdict(False),
    }
    if panicked:
        line["panicked"] = True
    del session, case
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
