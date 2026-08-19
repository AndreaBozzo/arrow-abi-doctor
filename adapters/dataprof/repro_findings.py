#!/usr/bin/env python3
"""Minimal, self-contained reproducers for what the M0.5 smoke run turned up.

Deliberately uses nothing from arrow-abi-doctor. Both findings surfaced while
exercising dataprof through the Arrow PyCapsule interface, but neither needs
libabi to reproduce, and an upstream issue that depends on an unpublished tool
is an issue nobody can act on. pyarrow alone is enough for both.

Run against a build of the branch under test and report the commit alongside the
result: a reproducer without the consumer version is not reproducible.
"""

from __future__ import annotations

import subprocess
import sys
import traceback


def build_info() -> str:
    import dataprof

    try:
        commit = subprocess.run(
            ["git", "-C", "C:/dev/dataprof", "rev-parse", "--short", "HEAD"],
            capture_output=True,
            text=True,
            timeout=10,
        ).stdout.strip()
    except Exception:  # noqa: BLE001
        commit = "?"
    return f"dataprof {dataprof.__version__} (checkout {commit or '?'})"


def attempt(label: str, fn):
    """Runs fn, classifying the outcome. BaseException on purpose: a pyo3
    PanicException does not derive from Exception."""
    try:
        fn()
    except BaseException as exc:  # noqa: BLE001
        kind = "CRASH " if not isinstance(exc, Exception) else "reject"
        print(f"  [{kind}] {label}")
        print(f"           {type(exc).__name__}: {exc}")
        return type(exc).__name__
    print(f"  [ ok   ] {label}")
    return None


# --------------------------------------------------------------------------
# Finding 1: a non-pyarrow object implementing the Arrow PyCapsule interface is
# refused, although profile() documents "Arrow PyCapsule-compatible objects".
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


def finding_1() -> None:
    import dataprof
    import pyarrow as pa

    print("\nfinding 1: PyCapsule producer that is not a pyarrow object")
    print("  docstring: profile() accepts 'Arrow PyCapsule-compatible objects'")

    batch = pa.record_batch({"a": pa.array([1, 2, 3]), "b": pa.array(["x", "y", "z"])})

    # control: the same data, as a pyarrow RecordBatch
    attempt("control: dataprof.profile(pa.RecordBatch)", lambda: dataprof.profile(batch))
    # the case under test: same capsules, different Python type
    attempt(
        "under test: dataprof.profile(PyCapsuleProducer)",
        lambda: dataprof.profile(PyCapsuleProducer(batch)),
    )


# --------------------------------------------------------------------------
# Finding 2: a dictionary index out of range for its dictionary panics rather
# than producing an error. The input is invalid, so a refusal is correct -- the
# finding is that the refusal arrives as a panic across the FFI boundary.
# --------------------------------------------------------------------------


def make_bad_dictionary(pa):
    """dictionary<int8, utf8> with index 99 over a 2-value dictionary."""
    indices = pa.array([0, 1, 99, 0], type=pa.int8())
    values = pa.array(["aa", "bb"], type=pa.string())
    # from_arrays does not bounds-check indices against the dictionary
    return pa.DictionaryArray.from_arrays(indices, values)


def finding_2() -> None:
    import dataprof
    import pyarrow as pa

    print("\nfinding 2: dictionary index out of range for its dictionary")
    print("  the input is invalid; the expected outcome is a clean error")

    try:
        dic = make_bad_dictionary(pa)
    except BaseException as exc:  # noqa: BLE001
        print(f"  [ n/a  ] pyarrow refused to build the array: {exc}")
        return

    batch = pa.record_batch([dic], names=["d"])
    print(f"  pyarrow accepted the array at construction: {batch.num_rows} rows")
    attempt("under test: dataprof.profile(batch)", lambda: dataprof.profile(batch))


def main() -> int:
    print("dataprof PyCapsule / dictionary findings")
    print("build:", build_info())
    try:
        import pyarrow

        print("pyarrow:", pyarrow.__version__)
    except ImportError:
        print("pyarrow is required")
        return 2

    for fn in (finding_1, finding_2):
        try:
            fn()
        except BaseException:  # noqa: BLE001
            traceback.print_exc()
    return 0


if __name__ == "__main__":
    sys.exit(main())
