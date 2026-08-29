#!/usr/bin/env python3
"""Read generated Corpus A back with pyarrow and check it says what the model says.

`tools/gen_corpus.py` proves the corpus is *well-formed*: every file encodes
canonically and round-trips byte for byte. That is a claim about the container.
It would pass just as happily if the generator wrote its validity bitmap
most-significant-bit first, or applied `ArrowArray.offset` to the wrong buffer,
or laid an aliased buffer at the wrong place in its allocation. Nothing there
reads the bytes back as an Arrow array.

This does. Every case is imported through the C Data Interface and every slot is
compared against the value the model says belongs in it -- across all five
types, four null patterns, five offset classes, three alignment classes and four
buffer states.

The expected values are recomputed here rather than read from the generator, and
that duplication is deliberate. It is the opposite case from the model
enumeration, which must exist exactly once (AGENTS.md): agreement between two
implementations of the same rule is evidence, while agreement between one
implementation and itself is not. Keep this a second opinion.

pyarrow is one voice, not ground truth. A disagreement here is a reason to look,
starting with which of the two is wrong.
"""

from __future__ import annotations

import math
import pathlib
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
MANIFEST = ROOT / "corpus" / "a" / "manifest.tsv"

# docs/coverage-matrix.md 1.2. Restated rather than imported: this file is a
# second opinion on the generator, and one that shares its tables is worth less.
LENGTHS = {"zero": 0, "one": 1, "small": 9, "medium": 1024}


def is_null(pattern: str, i: int, length: int) -> bool:
    """Null-pattern classes of the model, over logical indices (matrix 1.4)."""
    return {
        "none": False,
        "all": True,
        "alternating": i % 2 == 0,
        "sparse": i == length - 1,
    }[pattern]


def expected(kind: str, i: int) -> Any:
    """The value the generator puts in logical slot `i` of a field of `kind`."""
    if kind in ("int32", "int64"):
        return 1000 + i
    if kind == "float64":
        # Slots 0 and 1 are the two values with more than one bit pattern per
        # logical value; they are why float64 is in the model at all.
        if i == 0:
            return "-0.0"
        if i == 1:
            return "nan"
        return i + 0.5
    if kind == "utf8":
        return chr(ord("a") + i % 26) * (i % 3 + 1)
    if kind == "bool":
        return (i % 3) != 0
    raise SystemExit(f"unknown type class {kind}")


def matches(want: Any, got: Any) -> bool:
    if want == "nan":
        return isinstance(got, float) and math.isnan(got)
    if want == "-0.0":
        return got == 0.0 and math.copysign(1, got) < 0
    return bool(got == want)


def check_case(batch: Any, kind: str, length: int, pattern: str) -> str | None:
    """The first disagreement between this batch and the model, or None."""
    if batch.num_rows != length:
        return f"length {batch.num_rows}, model says {length}"
    column = batch.column(0)
    want_nulls = sum(is_null(pattern, i, length) for i in range(length))
    if column.null_count != want_nulls:
        return f"null_count {column.null_count}, model says {want_nulls}"
    for i, got in enumerate(column.to_pylist()):
        if is_null(pattern, i, length):
            if got is not None:
                return f"slot {i} should be null, is {got!r}"
            continue
        want = expected(kind, i)
        if not matches(want, got):
            return f"slot {i}: {got!r}, model says {want!r}"
    return None


def main() -> int:
    import _abicase
    import pyarrow

    if not MANIFEST.is_file():
        raise SystemExit(f"no corpus at {MANIFEST.parent}; run tools/gen_corpus.py first")

    rows = [
        line.split("\t")
        for line in MANIFEST.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]
    print(f"corpus:   {len(rows)} cases from {MANIFEST.parent}")
    print(f"consumer: pyarrow {pyarrow.__version__}")

    rejected: list[str] = []
    disagreed: list[str] = []
    for case_id, _size, kind, length_class, _offset, pattern, _buffers, _align, _life in rows:
        case = _abicase.load(str(MANIFEST.parent / f"{case_id}.abicase"))
        try:
            try:
                batch = pyarrow.record_batch(case)
            except BaseException as exc:  # noqa: BLE001
                # BaseException: a consumer crash has to be a recorded result,
                # not the end of the run. See run_smoke.py for why pyo3 makes
                # this load-bearing.
                rejected.append(f"{case_id} [{kind} {length_class} {pattern}]: {exc}")
                continue
            problem = check_case(batch, kind, LENGTHS[length_class], pattern)
            if problem:
                disagreed.append(f"{case_id} [{kind} {length_class} {pattern}]: {problem}")
        finally:
            case.release_all()

    verified = len(rows) - len(rejected) - len(disagreed)
    print(f"verified: {verified}")
    print(f"rejected: {len(rejected)}")
    print(f"disagree: {len(disagreed)}")
    for line in rejected[:10]:
        print(f"  REJECTED {line}")
    for line in disagreed[:10]:
        print(f"  DISAGREE {line}")

    # Every case in the model is valid Arrow, so a rejection is a defect
    # somewhere -- matrix 5 permits a documented refusal of a non-zero offset or
    # a non-natural alignment, but pyarrow supports both, so here it would mean
    # the generator built something it should not have.
    return 1 if rejected or disagreed else 0


if __name__ == "__main__":
    sys.exit(main())
