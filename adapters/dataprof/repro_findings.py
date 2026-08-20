#!/usr/bin/env python3
"""Regression checks for what the M0.5 smoke run turned up in dataprof.

Both findings are fixed upstream and this file is now their guard: it states the
expected post-fix behaviour and exits non-zero if a build shows the old one.

  finding 1  a non-pyarrow Arrow PyCapsule producer was refused
             fixed by dataprof#608 (fa1aa7d), on master since 2026-08-20
  finding 2  an out-of-range dictionary index panicked across FFI
             fixed by dataprof#610 (0ca2766), on master since 2026-08-20

Deliberately uses nothing from arrow-abi-doctor: an upstream reproducer that
depends on an unpublished tool is one nobody can run. pyarrow alone is enough
for finding 1. It is *not* enough for finding 2 -- pa.DictionaryArray.from_arrays
bounds-checks, so the invalid input cannot be built from Python at all and the
check has to come in over the C Data Interface. That case lives in run_smoke.py,
and the fact that it has to is the argument for this harness existing.

Report the consumer commit alongside the result: a reproducer without the
consumer version is not reproducible. Set DATAPROF_SRC to a checkout to have it
resolved automatically.
"""

from __future__ import annotations

import os
import subprocess
import sys
import traceback

OK, REJECT, CRASH = "ok", "reject", "crash"


def build_info() -> str:
    import dataprof

    # The version string lags master between releases, so it does not identify
    # what a build actually contains; the wheel's path and a checkout commit do.
    where = os.path.dirname(getattr(dataprof, "__file__", "") or "?")
    src = os.environ.get("DATAPROF_SRC")
    commit = ""
    if src:
        try:
            commit = subprocess.run(
                ["git", "-C", src, "rev-parse", "--short", "HEAD"],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            ).stdout.strip()
        except Exception:  # noqa: BLE001
            commit = ""
    if commit:
        suffix = f", checkout {commit}"
    elif src:
        # A git worktree of a Windows repo does not resolve from WSL: its .git
        # file points at a C:/ path git cannot follow from the other side.
        suffix = f", checkout of {src} unresolved"
    else:
        suffix = ", checkout unknown (set DATAPROF_SRC)"
    return f"dataprof {dataprof.__version__} from {where}{suffix}"


def attempt(label: str, fn) -> str:
    """Runs fn, classifying the outcome. BaseException on purpose: a pyo3
    PanicException derives from BaseException, so `except Exception` misses it --
    which was the substance of finding 2."""
    try:
        fn()
    except BaseException as exc:  # noqa: BLE001
        kind = CRASH if not isinstance(exc, Exception) else REJECT
        print(f"  [{'CRASH ' if kind is CRASH else 'reject'}] {label}")
        print(f"           {type(exc).__name__}: {exc}")
        return kind
    print(f"  [ ok   ] {label}")
    return OK


def verdict(passed: bool, note: str) -> bool:
    print(f"  => {'PASS' if passed else 'FAIL'}: {note}")
    return passed


# --------------------------------------------------------------------------
# Finding 1: a non-pyarrow object implementing the Arrow PyCapsule interface was
# refused, although profile() documents "Arrow PyCapsule-compatible objects".
# Fixed in #608 by matching the API rather than the Python type name.
# --------------------------------------------------------------------------


class PyCapsuleProducer:
    """A minimal Arrow PyCapsule producer.

    Delegates to a pyarrow RecordBatch, so the *data* is byte-identical to what
    dataprof accepts when handed the batch directly. The only difference is the
    type of the Python object, which isolates the behaviour under test.
    """

    def __init__(self, batch):
        self._batch = batch

    def __arrow_c_schema__(self):
        return self._batch.__arrow_c_schema__()

    def __arrow_c_array__(self, requested_schema=None):
        return self._batch.__arrow_c_array__(requested_schema)


def finding_1() -> bool:
    import dataprof
    import pyarrow as pa

    print("\nfinding 1: PyCapsule producer that is not a pyarrow object")
    print("  expected after dataprof#608: accepted, same as the RecordBatch")

    batch = pa.record_batch({"a": pa.array([1, 2, 3]), "b": pa.array(["x", "y", "z"])})

    # control: the same data, as a pyarrow RecordBatch
    control = attempt("control: dataprof.profile(pa.RecordBatch)", lambda: dataprof.profile(batch))
    # the case under test: same capsules, different Python type
    under_test = attempt(
        "under test: dataprof.profile(PyCapsuleProducer)",
        lambda: dataprof.profile(PyCapsuleProducer(batch)),
    )

    if control is not OK:
        return verdict(False, "control failed; this build cannot profile a RecordBatch at all")
    if under_test is OK:
        return verdict(True, "the PyCapsule producer is accepted")
    return verdict(
        False,
        "pre-#608 behaviour: a documented-supported producer is refused on object type",
    )


# --------------------------------------------------------------------------
# Finding 2: a dictionary index out of range for its dictionary panicked instead
# of erroring. The input is invalid, so a refusal is correct -- the finding was
# that the refusal arrived as a pyo3 PanicException, which `except Exception`
# does not catch. Fixed in #610 by validating FFI-imported data.
#
# There is no pyarrow-only reproducer: see the module docstring.
# --------------------------------------------------------------------------


def finding_2() -> bool:
    import dataprof
    import pyarrow as pa

    print("\nfinding 2: dictionary index out of range for its dictionary")
    print("  expected after dataprof#610: a catchable error, not a panic")

    indices = pa.array([0, 1, 99, 0], type=pa.int8())
    values = pa.array(["aa", "bb"], type=pa.string())
    try:
        dic = pa.DictionaryArray.from_arrays(indices, values)
    except BaseException as exc:  # noqa: BLE001
        print(f"  [ n/a  ] pyarrow refused to build the array: {exc}")
        return verdict(
            True,
            "not reachable from Python; the C Data Interface case is in run_smoke.py",
        )

    batch = pa.record_batch([dic], names=["d"])
    print(f"  pyarrow accepted the array at construction: {batch.num_rows} rows")
    outcome = attempt("under test: dataprof.profile(batch)", lambda: dataprof.profile(batch))
    if outcome is CRASH:
        return verdict(False, "pre-#610 behaviour: refusal arrives as a panic across FFI")
    return verdict(True, "refused as a catchable error" if outcome is REJECT else "accepted")


def main() -> int:
    print("dataprof PyCapsule / dictionary regression checks")
    print("build:", build_info())
    try:
        import pyarrow

        print("pyarrow:", pyarrow.__version__)
    except ImportError:
        print("pyarrow is required")
        return 2

    results = []
    for fn in (finding_1, finding_2):
        try:
            results.append(fn())
        except BaseException:  # noqa: BLE001
            traceback.print_exc()
            results.append(False)

    failed = results.count(False)
    print("\n" + "-" * 74)
    print(f"{len(results)} check(s), {failed} failed")
    print("regression checks:", "FAIL" if failed else "PASS")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
