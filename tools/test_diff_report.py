#!/usr/bin/env python3
"""Checks for tools/diff_report.py, over synthetic runs.

A real run cannot reach the branches that matter most yet: with three of the
five lifecycles inexpressible, no real run is ever claimable, so "prints N/N
exactly when there are no holes" would be untested if it were only ever fed
real artifacts. These build the runs instead -- a manifest covering the whole
model, workers whose lines say exactly what each check needs -- and assert the
state, the route and the claim.

Plain asserts and a count, like the C suites; no test framework is a dependency.
"""

from __future__ import annotations

import sys
import tempfile
from collections.abc import Callable
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import diff_report
from coverage_matrix import Case, enumerate_model

CELLS, _ = enumerate_model()
FULL = {f"{i:032x}": cell for i, cell in enumerate(CELLS)}
DIRECT = {k: c for k, c in FULL.items() if c.lifecycle == "direct"}
SENT = {"physical": "a" * 32, "logical": "b" * 32}


def line(case_id: str, status: str = "accepted", **extra: Any) -> dict[str, Any]:
    out: dict[str, Any] = {
        "case": f"corpus/a/{case_id}.abicase",
        "id": case_id,
        "status": status,
        "detail": "",
        "observer": {"violations": 0, "outstanding": 0},
        "digest": {"ver": 1, "sent": dict(SENT), "received": dict(SENT)},
    }
    out.update(extra)
    return out


def worker(
    lines: list[dict[str, Any]], name: str = "w", suspect: str | None = None
) -> dict[str, Any]:
    return {
        "name": name,
        "header": {"worker_protocol": 1, "consumer": name},
        "lines": lines,
        "suspect": suspect,
        "protocol_errors": [],
    }


def run(
    manifest: dict[str, Case], lines: list[dict[str, Any]], limits: Any = (), **kw: Any
) -> dict[str, Any]:
    return diff_report.build(CELLS, manifest, [worker(lines, **kw)], list(limits))


def cell_record(report: dict[str, Any], case_id: str) -> dict[str, Any]:
    return next(c for c in report["cells"] if c["case_id"] == case_id)["consumers"]["w"]


def first(manifest: dict[str, Case], pred: Callable[[Case], bool]) -> str:
    return next(k for k, c in manifest.items() if pred(c))


EITHER = first(DIRECT, lambda c: c.offset != "0" and c.length != "zero")
ACCEPT = first(DIRECT, lambda c: c.offset == "0" and c.alignment == "natural")


def test_full_clean_run_is_claimable() -> None:
    report = run(FULL, [line(k) for k in FULL])
    assert report["claim"]["claimable"], report["claim"]
    assert report["claim"]["text"].endswith(f": {len(CELLS)}/{len(CELLS)}")
    assert "`coverage_model_ver` 2" in report["claim"]["text"]
    assert report["consumers"]["w"]["states"]["agree"] == len(CELLS)


def test_inexpressible_cells_block_the_claim() -> None:
    report = run(DIRECT, [line(k) for k in DIRECT])
    states = report["consumers"]["w"]["states"]
    assert states["inexpressible"] == len(CELLS) - len(DIRECT), states
    assert not report["claim"]["claimable"] and report["claim"]["text"] is None


def test_a_worker_killed_mid_run_is_not_n_over_n() -> None:
    ids = list(FULL)
    dead = ids[40]
    report = run(FULL, [line(k) for k in ids[:40]], suspect=f"corpus/a\\{dead}.abicase")
    assert not report["claim"]["claimable"]
    states = report["consumers"]["w"]["states"]
    assert states["not-run"] == len(ids) - 40, states
    record = cell_record(report, dead)
    assert record["state"] == "not-run" and record["route"] == "defect", record
    assert [d["case_id"] for d in report["defects"]] == [dead]


def test_a_planted_logical_mismatch_is_a_disagreement() -> None:
    lines = [line(k) for k in FULL]
    lines[7]["digest"]["received"]["logical"] = "c" * 32
    report = run(FULL, lines)
    record = cell_record(report, lines[7]["id"])
    assert record["state"] == "disagree" and "silent divergence" in record["reason"], record
    assert len(report["defects"]) == 1


def test_a_physical_only_change_is_representation_not_defect() -> None:
    lines = [line(k) for k in FULL]
    lines[3]["digest"]["received"]["physical"] = "d" * 32
    report = run(FULL, lines)
    record = cell_record(report, lines[3]["id"])
    assert record["state"] == "agree" and record["route"] == "representation", record
    assert not report["defects"]


def test_an_undocumented_either_rejection_goes_to_review() -> None:
    lines = [line(k) for k in DIRECT]
    target = next(x for x in lines if x["id"] == EITHER)
    target.update(status="rejected", detail="First offset 7 is larger than values length 0")
    report = run(DIRECT, lines)
    record = cell_record(report, EITHER)
    assert record["state"] == "agree" and record["route"] == "undocumented", record
    assert not report["compatibility"] and len(report["undocumented_limits"]) == 1
    assert report["review"][0]["cells"] == 1
    assert "First offset N" in report["review"][0]["rejection"]


def test_a_documented_either_rejection_is_compatibility() -> None:
    lines = [line(k) for k in DIRECT]
    next(x for x in lines if x["id"] == EITHER).update(status="rejected", detail="no offsets")
    limit = {
        "consumer": "w",
        "dimension": "offset",
        "values": ["1", "7", "8", "9"],
        "source": "https://example.invalid/doc",
        "quote": "Non-zero offsets are not supported.",
    }
    report = run(DIRECT, lines, [limit])
    record = cell_record(report, EITHER)
    assert record["route"] == "compatibility", record
    assert not report["review"]


def test_a_documented_limit_whose_detail_does_not_match_is_not_used() -> None:
    lines = [line(k) for k in DIRECT]
    next(x for x in lines if x["id"] == EITHER).update(status="rejected", detail="out of bounds")
    limit = {
        "consumer": "w",
        "dimension": "offset",
        "values": ["1", "7", "8", "9"],
        "detail": "offset.*not supported",
        "source": "https://example.invalid/doc",
        "quote": "Non-zero offsets are not supported.",
    }
    report = run(DIRECT, lines, [limit])
    assert cell_record(report, EITHER)["route"] == "undocumented"


def test_rejecting_an_accept_cell_is_a_defect() -> None:
    lines = [line(k) for k in DIRECT]
    next(x for x in lines if x["id"] == ACCEPT).update(status="rejected", detail="nope")
    report = run(DIRECT, lines)
    record = cell_record(report, ACCEPT)
    assert record["state"] == "disagree" and record["route"] == "defect", record


def test_a_type_limit_covers_an_accept_cell() -> None:
    lines = [line(k) for k in DIRECT]
    kind = DIRECT[ACCEPT].type
    next(x for x in lines if x["id"] == ACCEPT).update(status="rejected", detail="unsupported")
    limit = {
        "consumer": "w",
        "dimension": "type",
        "values": [kind],
        "source": "https://example.invalid/types",
        "quote": f"{kind} is not supported.",
    }
    report = run(DIRECT, lines, [limit])
    assert cell_record(report, ACCEPT)["route"] == "compatibility"


def test_a_panic_is_a_defect_even_on_an_either_cell() -> None:
    lines = [line(k) for k in DIRECT]
    next(x for x in lines if x["id"] == EITHER).update(
        status="rejected", detail="PanicException", panicked=True
    )
    report = run(DIRECT, lines)
    assert cell_record(report, EITHER)["route"] == "defect"


def test_outstanding_bytes_are_a_leak_only_with_no_live_capsule() -> None:
    lines = [line(k) for k in DIRECT]
    lines[0]["observer"] = {"violations": 0, "outstanding": 64, "capsules_outstanding": 0}
    lines[1]["observer"] = {"violations": 0, "outstanding": 64, "capsules_outstanding": 1}
    report = run(DIRECT, lines)
    assert cell_record(report, lines[0]["id"])["route"] == "defect"
    assert cell_record(report, lines[1]["id"])["state"] == "agree"


def test_a_lifecycle_violation_is_a_defect() -> None:
    lines = [line(k) for k in DIRECT]
    lines[0]["lifecycle"] = {"incomplete": False, "violations": ["never-released array /"]}
    report = run(DIRECT, lines)
    record = cell_record(report, lines[0]["id"])
    assert record["route"] == "defect" and "never-released" in record["reason"], record


def test_a_truncated_lifecycle_log_is_not_run() -> None:
    lines = [line(k) for k in FULL]
    lines[0]["lifecycle"] = {"incomplete": True, "violations": []}
    report = run(FULL, lines)
    assert cell_record(report, lines[0]["id"])["state"] == "not-run"
    assert not report["claim"]["claimable"]


def test_a_harness_error_is_not_run() -> None:
    lines = [line(k) for k in FULL]
    lines[0].update(status="error", detail="decode failed")
    report = run(FULL, lines)
    assert cell_record(report, lines[0]["id"])["state"] == "not-run"
    assert not report["claim"]["claimable"]


def test_malformed_limits_are_refused() -> None:
    bad = [
        'consumer = "w"',  # not a [[limit]] table at all: ignored, not refused
        '[[limit]]\nconsumer="w"\ndimension="nulls"\nvalues=["all"]\nsource="s"\nquote="q"',
        '[[limit]]\nconsumer="w"\ndimension="offset"\nvalues=["0"]\nsource="s"\nquote="q"',
        '[[limit]]\nconsumer="w"\ndimension="offset"\nvalues=["1"]\nsource="s"',
    ]
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "limits.toml"
        path.write_text(bad[0], encoding="utf-8")
        assert diff_report.load_limits(path) == []
        for text in bad[1:]:
            path.write_text(text, encoding="utf-8")
            try:
                diff_report.load_limits(path)
            except SystemExit:
                continue
            raise AssertionError(f"accepted a malformed limit:\n{text}")


def test_the_shipped_limits_file_parses() -> None:
    diff_report.load_limits(diff_report.LIMITS)


def main() -> int:
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith("test_")]
    failures = 0
    for name, fn in tests:
        try:
            fn()
        except AssertionError as exc:
            failures += 1
            print(f"FAIL {name}: {exc}")
    print(f"{len(tests)} checks, {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
