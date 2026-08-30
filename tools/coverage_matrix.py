#!/usr/bin/env python3
"""Reference enumerator for the bounded conformance model.

`docs/coverage-matrix.md` is the normative document; this script is the
executable form of the same model, and it exists so that N is a number this
repository computes rather than a number someone asserted. `--check` compares
the value recorded in the document against the one enumerated here and fails
if they have drifted apart, which is the failure mode a hand-maintained
count has.

The enumeration is deliberately structured as "full cartesian product, then
named constraints" and reports how many combinations each constraint removed.
A constraint that removes nothing is either wrong or redundant, and that is
worth seeing.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections.abc import Callable, Iterator
from itertools import product
from pathlib import Path
from typing import NamedTuple

MODEL_VERSION = 2

DOC = Path(__file__).resolve().parent.parent / "docs" / "coverage-matrix.md"

TYPE = ("int32", "int64", "float64", "utf8", "bool")
LENGTH = ("zero", "one", "small", "medium")
OFFSET = ("0", "1", "7", "8", "9")
NULLS = ("none", "all", "alternating", "sparse")
BUFFERS = ("normal", "empty", "omitted-validity", "aliased")
ALIGN = ("natural", "+1", "+4")
LIFECYCLE = ("direct", "moved", "streamed", "early-release", "EOF")

DIMENSIONS = (
    ("type", TYPE),
    ("length", LENGTH),
    ("offset", OFFSET),
    ("nulls", NULLS),
    ("buffers", BUFFERS),
    ("alignment", ALIGN),
    ("lifecycle", LIFECYCLE),
)

# Lifecycles that deliver a schema but never an array. The data-bearing
# dimensions are inapplicable there, so they are pinned to a sentinel rather
# than left free -- otherwise one case would be counted 1880 times.
DATA_FREE = ("early-release", "EOF")
SENTINEL = {
    "length": "zero",
    "offset": "0",
    "nulls": "none",
    "buffers": "normal",
    "alignment": "natural",
}


class Case(NamedTuple):
    type: str
    length: str
    offset: str
    nulls: str
    buffers: str
    alignment: str
    lifecycle: str


# Each entry is (name, holds). `holds` returns False for a combination the
# model excludes. The names are the ones used in docs/coverage-matrix.md; the
# two lists are meant to be read side by side.
CONSTRAINTS: tuple[tuple[str, Callable[[Case], bool]], ...] = (
    (
        "data-free-lifecycle",
        lambda c: (
            c.lifecycle not in DATA_FREE or all(getattr(c, k) == v for k, v in SENTINEL.items())
        ),
    ),
    ("null-pattern-needs-elements", lambda c: c.length != "zero" or c.nulls == "none"),
    (
        "single-element-null-collapse",
        lambda c: c.length != "one" or c.nulls in ("none", "all"),
    ),
    ("empty-buffers-need-zero-length", lambda c: c.buffers != "empty" or c.length == "zero"),
    ("empty-buffers-need-zero-offset", lambda c: c.buffers != "empty" or c.offset == "0"),
    ("empty-buffers-have-no-alignment", lambda c: c.buffers != "empty" or c.alignment == "natural"),
    (
        "omitted-validity-needs-no-nulls",
        lambda c: c.buffers != "omitted-validity" or c.nulls == "none",
    ),
    ("aliasing-needs-elements", lambda c: c.buffers != "aliased" or c.length != "zero"),
)


# Case(*combo) pairs DIMENSIONS with Case's fields by position. Reordering one
# without the other would mislabel every tuple and still produce a plausible N,
# so the pairing is asserted rather than trusted. Likewise a sentinel naming a
# value outside its own dimension would silently empty a whole constraint.
assert tuple(name for name, _ in DIMENSIONS) == Case._fields, "DIMENSIONS/Case order"
assert set(SENTINEL) <= set(Case._fields), "SENTINEL names a field that does not exist"
assert all(v in dict(DIMENSIONS)[k] for k, v in SENTINEL.items()), "sentinel outside its dimension"


def all_combinations() -> Iterator[Case]:
    for combo in product(*(values for _, values in DIMENSIONS)):
        yield Case(*combo)


def enumerate_model() -> tuple[list[Case], dict[str, int]]:
    """Return the accepted cases and, per constraint, how many it was first to reject."""
    accepted: list[Case] = []
    rejected: dict[str, int] = {name: 0 for name, _ in CONSTRAINTS}
    for case in all_combinations():
        for name, holds in CONSTRAINTS:
            if not holds(case):
                rejected[name] += 1
                break
        else:
            accepted.append(case)
    return accepted, rejected


def sole_match(text: str, pattern: str, description: str) -> str:
    """The one capture of `pattern` in `text`; a gate that guesses is not a gate."""
    found = re.findall(pattern, text, re.MULTILINE)
    if len(found) != 1:
        raise SystemExit(f"{DOC}: expected exactly one {description}, found {len(found)}")
    return str(found[0])


def section(text: str, heading: str) -> str:
    """The body of one `##` section, so a lookup cannot stray into another.

    Class names recur across the document with different numbers beside them --
    `zero` is a length of 0 in section 1.2 and a count of 475 in section 3 --
    so every count below is read from section 3 and nowhere else.
    """
    marker = f"\n## {heading}"
    if marker not in text:
        raise SystemExit(f"{DOC}: no section '## {heading}'")
    rest = text[text.index(marker) + 1 :]
    end = rest.find("\n## ", 1)
    return rest if end < 0 else rest[:end]


def check_document(accepted: list[Case], rejected: dict[str, int]) -> list[str]:
    """Every mismatch between what the document states and what this enumerates.

    The document quotes N, the per-constraint removal counts and the three
    breakdown tables. All of them are checked: N alone would not catch a
    reordering of the constraints, which changes the attribution of every count
    while leaving the total untouched.
    """
    text = DOC.read_text(encoding="utf-8")
    body = section(text, "3. N")
    problems: list[str] = []

    doc_ver = int(sole_match(text, r"^`coverage_model_ver` = (\d+)", "`coverage_model_ver` = <n>"))
    if doc_ver != MODEL_VERSION:
        problems.append(
            f"document says coverage_model_ver = {doc_ver}, this script is {MODEL_VERSION}"
        )

    # Section 4 quotes the only claim the model licenses, version included, and that
    # quotation is what a report copies. The header above it was checked and the
    # quotation was not, so it sat at version 1 for the whole of version 2 -- the
    # drift this document exists to prevent, in the sentence that states the claim.
    claim_ver = int(
        sole_match(
            text,
            r"^> `docs/coverage-matrix\.md`, `coverage_model_ver` (\d+)$",
            "the licensed-claim quotation in section 4",
        )
    )
    if claim_ver != MODEL_VERSION:
        problems.append(
            f"section 4's licensed claim names coverage_model_ver {claim_ver}, "
            f"this script is {MODEL_VERSION}"
        )

    doc_n = int(sole_match(body, r"^N = \*\*(\d+)\*\*", "'N = **<number>**' in section 3"))
    if doc_n != len(accepted):
        problems.append(f"document says N = {doc_n}, enumeration gives {len(accepted)}")

    for name, count in rejected.items():
        if count == 0:
            problems.append(f"constraint {name!r} removes nothing, so it is wrong or redundant")
        quoted = sole_match(body, rf"^ +{re.escape(name)} +(\d+)$", f"a removal count for {name!r}")
        if int(quoted) != count:
            problems.append(
                f"{name}: document quotes {quoted} removed, enumeration removes {count}"
            )

    breakdowns = (("lifecycle", LIFECYCLE), ("buffers", BUFFERS), ("length", LENGTH))
    for field, values in breakdowns:
        for value in values:
            count = sum(1 for c in accepted if getattr(c, field) == value)
            quoted = sole_match(
                body, rf"`{re.escape(value)}` \| (\d+)", f"a {field} count for {value!r}"
            )
            if int(quoted) != count:
                problems.append(
                    f"{field} {value}: document quotes {quoted}, enumeration gives {count}"
                )

    return problems


def report(accepted: list[Case], rejected: dict[str, int]) -> None:
    total = 1
    for _, values in DIMENSIONS:
        total *= len(values)
    print(f"coverage_model_ver = {MODEL_VERSION}")
    print(f"raw product        = {total}")
    print()
    print("removed by constraint (first one to reject wins):")
    width = max(len(name) for name in rejected)
    for name, count in rejected.items():
        flag = "   <- removes nothing" if count == 0 else ""
        print(f"  {name:<{width}}  {count:>6}{flag}")
    print()
    print(f"N = {len(accepted)}")
    print()
    print("by lifecycle:")
    for value in LIFECYCLE:
        print(f"  {value:<14} {sum(1 for c in accepted if c.lifecycle == value):>6}")
    print()
    print("by buffer-state:")
    for value in BUFFERS:
        print(f"  {value:<18} {sum(1 for c in accepted if c.buffers == value):>6}")
    print()
    print("by length-class:")
    for value in LENGTH:
        print(f"  {value:<8} {sum(1 for c in accepted if c.length == value):>6}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--check",
        action="store_true",
        help="compare against docs/coverage-matrix.md and exit non-zero on a mismatch",
    )
    mode.add_argument("--list", action="store_true", help="print every accepted combination")
    args = parser.parse_args()

    accepted, rejected = enumerate_model()

    if args.list:
        for case in accepted:
            print("\t".join(case))
        return 0

    if args.check:
        problems = check_document(accepted, rejected)
        for problem in problems:
            print(f"MISMATCH: {problem}")
        if problems:
            return 1
        print(f"ok  coverage_model_ver {MODEL_VERSION}, N = {len(accepted)}, breakdowns agree")
        return 0

    report(accepted, rejected)
    return 0


if __name__ == "__main__":
    sys.exit(main())
