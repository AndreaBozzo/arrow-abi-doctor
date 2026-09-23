#!/usr/bin/env python3
"""Turn a coordinator run into the per-cell report docs/coverage-matrix.md §4 defines.

    python tools/diff_report.py <run-dir>/run.json [--manifest corpus/a/manifest.tsv]

The coordinator writes one result stream per worker. This reads all of them
together against the model and puts every one of the N cells, per consumer, in
exactly one §4 state:

    agree          the case ran, and the outcome is one the model allows
    disagree       the case ran, and the outcome is a defect
    not-run        the case was not executed, for any reason -- a harness error,
                   a crash that ended the worker, a --limit that left it out
    inexpressible  the case could not be constructed at all (the lifecycles the
                   generator cannot build yet)

and prints the one claim the model licenses only when `not-run` and
`inexpressible` are both zero.

What counts as a defect is §5: a crash, a leak, a lifecycle violation -- counted
by the observer or named by the state machine of abi/lifecycle.h -- a caught
panic, or a `logical` digest mismatch is a bug under every expected outcome; a
rejection of an `ACCEPT` cell is a defect; a clean rejection of an `EITHER` cell
is not. That last one is then *routed*: to the compatibility matrix when
docs/documented-limits.toml says the consumer documents the limitation, and
otherwise to the undocumented-limits map and a review list. Never silently to
compatibility -- apache/arrow-rs#10910 was a consumer defect whose symptom was
exactly such a rejection, and each rejection's text is kept verbatim so a
reviewer can tell the two apart.

The model and the manifest are imported, not re-read by hand: coverage_matrix.py
is the one enumeration (AGENTS.md), and gen_corpus.py already parses the
manifest. What this adds is judgement over results, and nothing about cells.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import tomllib
from collections import Counter
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

from coverage_matrix import MODEL_VERSION, Case, enumerate_model
from gen_corpus import read_manifest

ROOT = Path(__file__).resolve().parent.parent
LIMITS = ROOT / "docs" / "documented-limits.toml"
REPORT_SCHEMA = "abi-doctor/diff-report/1"
STATES = ("agree", "disagree", "not-run", "inexpressible")
CLAIM = (
    "exhaustive over the finite equivalence-class matrix defined in "
    "`docs/coverage-matrix.md`, `coverage_model_ver` {ver}"
)
# §5: the dimensions whose classes a consumer may decline, and the class of each
# that stays `ACCEPT` whatever the documentation says. `type` has none.
ROUTABLE = {"offset": "0", "alignment": "natural", "type": None}


def expected_outcome(cell: Case) -> str:
    """docs/coverage-matrix.md §5, cell by cell."""
    return "EITHER" if cell.offset != "0" or cell.alignment != "natural" else "ACCEPT"


def load_limits(path: Path) -> list[dict[str, Any]]:
    """The documented-limits table, validated: a malformed entry is refused."""
    limits: list[dict[str, Any]] = tomllib.loads(path.read_text(encoding="utf-8")).get("limit", [])
    for i, entry in enumerate(limits):
        where = f"{path.name} limit {i + 1}"
        for key in ("consumer", "dimension", "values", "source", "quote"):
            if not entry.get(key):
                raise SystemExit(f"{where}: `{key}` is required")
        if entry["dimension"] not in ROUTABLE:
            raise SystemExit(f"{where}: §5 does not let a consumer decline `{entry['dimension']}`")
        accept_class = ROUTABLE[entry["dimension"]]
        if accept_class is not None and accept_class in entry["values"]:
            raise SystemExit(f"{where}: {entry['dimension']}={accept_class} is an ACCEPT cell")
    return limits


def documented(limits: list[dict[str, Any]], consumer: str, cell: Case, detail: str) -> Any:
    """The entry that documents this rejection, or None."""
    for entry in limits:
        if entry["consumer"] != consumer:
            continue
        if getattr(cell, entry["dimension"]) not in entry["values"]:
            continue
        if entry.get("detail") and not re.search(entry["detail"], detail):
            continue
        return entry
    return None


# One return per row of §5's table, in the order the rows must be tried: a panic
# or a leak outranks how the case was answered. Split up, the order is hidden.
def classify(  # noqa: PLR0911
    line: dict[str, Any], cell: Case, consumer: str, limits: list[dict[str, Any]]
) -> tuple[str, str | None, str]:
    """(state, route, reason) for one executed case.

    route is None for a plain agreement, "defect", "compatibility",
    "undocumented" (which also puts it on the review list) or "representation".
    """
    status = line.get("status")
    detail = str(line.get("detail", ""))
    if status == "error":
        return "not-run", None, f"harness error: {detail}"

    if line.get("panicked"):
        return "disagree", "defect", f"panic across the FFI boundary: {detail}"
    obs = line.get("observer") or {}
    if obs.get("violations", 0):
        return "disagree", "defect", f"{obs['violations']} lifecycle violation(s)"
    lifecycle = line.get("lifecycle") or {}
    if lifecycle.get("incomplete"):
        # The event log overflowed, so the state machine judged a truncated
        # path: no verdict either way, which is a hole, not a pass.
        return "not-run", None, "lifecycle log truncated"
    if lifecycle.get("violations"):
        return "disagree", "defect", "lifecycle: " + "; ".join(lifecycle["violations"])
    if obs.get("outstanding", 0) and not obs.get("capsules_outstanding", 0):
        return "disagree", "defect", f"leak: {obs['outstanding']} bytes outstanding"

    if status == "rejected":
        entry = documented(limits, consumer, cell, detail)
        if entry is not None:
            return "agree", "compatibility", f"documented at {entry['source']}: {detail}"
        if expected_outcome(cell) == "ACCEPT":
            return "disagree", "defect", f"rejected an ACCEPT cell: {detail}"
        return "agree", "undocumented", detail

    digest = line.get("digest") or {}
    sent, received = digest.get("sent"), digest.get("received")
    if sent and received:
        if sent["logical"] != received["logical"]:
            return "disagree", "defect", "silent divergence: the logical digest changed"
        if sent["physical"] != received["physical"]:
            return "agree", "representation", "layout changed, data intact"
    return "agree", None, detail


def case_key(path: str) -> str:
    """The case id a path names: the coordinator's case lists are corpus paths."""
    return Path(path.replace("\\", "/")).stem


def build(
    cells: list[Case],
    manifest: dict[str, Case],
    workers: list[dict[str, Any]],
    limits: list[dict[str, Any]],
) -> dict[str, Any]:
    """The report, from the model, the manifest and each worker's results.

    `workers` entries: name, header, lines (result lines, header excluded),
    not_run (paths), suspect (path or None), protocol_errors.
    """
    constructed = {cell: case_id for case_id, cell in manifest.items()}
    per_cell: dict[Case, dict[str, Any]] = {cell: {} for cell in cells}
    findings: dict[str, list[dict[str, Any]]] = {
        "defect": [],
        "compatibility": [],
        "undocumented": [],
        "representation": [],
    }
    summary: dict[str, Any] = {}
    problems: list[str] = []

    for w in workers:
        name = w["name"]
        by_id: dict[str, dict[str, Any]] = {}
        for line in w["lines"]:
            case_id = line.get("id") or case_key(line.get("case", ""))
            if case_id not in manifest:
                problems.append(f"{name}: reported {line.get('case')!r}, not in the manifest")
                continue
            by_id[case_id] = line
        suspect = case_key(w["suspect"]) if w.get("suspect") else None

        for cell in cells:
            case_id = constructed.get(cell)
            record: dict[str, Any]
            if case_id is None:
                record = {"state": "inexpressible"}
            elif case_id == suspect:
                record = {"state": "not-run", "route": "defect", "reason": "crash: suspect"}
            elif case_id not in by_id:
                record = {"state": "not-run", "reason": "not executed"}
            else:
                line = by_id[case_id]
                state, route, reason = classify(line, cell, name, limits)
                record = {"state": state, "status": line.get("status"), "reason": reason}
                if route:
                    record["route"] = route
            record["case_id"] = case_id
            per_cell[cell][name] = record
            if record.get("route") in findings:
                findings[record["route"]].append(
                    {"consumer": name, "case_id": case_id, **cell._asdict(), **record}
                )

        counts = Counter(per_cell[cell][name]["state"] for cell in cells)
        # Exactly one state per cell: anything else is this tool's bug, and a
        # coverage figure built on it would be wrong without looking wrong.
        assert sum(counts.values()) == len(cells), (name, counts)
        header = w.get("header") or {}
        summary[name] = {
            "consumer": header.get("consumer"),
            "consumer_version": header.get("consumer_version"),
            "sanitizers": header.get("sanitizers"),
            "fault_injection": header.get("fault_injection"),
            "states": {state: counts.get(state, 0) for state in STATES},
            "protocol_errors": w.get("protocol_errors", []),
        }

    holes = {
        name: s["states"]["not-run"] + s["states"]["inexpressible"] for name, s in summary.items()
    }
    claimable = bool(workers) and not any(holes.values()) and not problems

    # §4 puts every cell in a state per consumer, so a consumer that ran the
    # whole model can make the claim even when another in the same run could
    # not. The disagreements go with it: M2-B is "no disagreement across N/N",
    # and exhaustive coverage alone is not that.
    # A test instrument -- the faulty worker, or any worker running with fault
    # injection -- is not a consumer, and nothing it does is a measurement.
    def instrument(s: dict[str, Any]) -> bool:
        return s["consumer"] == "faulty" or bool(s["fault_injection"])

    per_consumer = {
        name: {
            "claimable": not holes[name] and not problems and not instrument(s),
            "text": (CLAIM.format(ver=MODEL_VERSION) + f": {len(cells)}/{len(cells)}")
            if not holes[name] and not problems and not instrument(s)
            else None,
            "disagree": s["states"]["disagree"],
            "instrument": instrument(s),
        }
        for name, s in summary.items()
    }
    splits = []
    for cell in cells:
        # The verdict, not only the status: an accepted case whose data changed
        # is a different answer from one whose data survived, and it is the
        # split the logical digest exists to show.
        statuses = {
            name: (record.get("status") or record["state"])
            + (" (defect)" if record.get("route") == "defect" else "")
            for name, record in per_cell[cell].items()
            if record["state"] != "inexpressible"
        }
        if len(set(statuses.values())) > 1:
            splits.append({"case_id": constructed.get(cell), **cell._asdict(), **statuses})

    return {
        "schema": REPORT_SCHEMA,
        "coverage_model_ver": MODEL_VERSION,
        "N": len(cells),
        "claim": {
            "claimable": claimable,
            "text": (CLAIM.format(ver=MODEL_VERSION) + f": {len(cells)}/{len(cells)}")
            if claimable
            else None,
            "holes": holes,
            "per_consumer": per_consumer,
        },
        "consumers": summary,
        "defects": findings["defect"],
        "compatibility": findings["compatibility"],
        "undocumented_limits": findings["undocumented"],
        "review": review_list(findings["undocumented"]),
        "representation": findings["representation"],
        "splits": splits,
        "problems": problems,
        "cells": [
            {**cell._asdict(), "case_id": constructed.get(cell), "consumers": per_cell[cell]}
            for cell in cells
        ],
    }


def review_list(undocumented: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Undocumented rejections grouped by consumer and text: the work list.

    Each group is one question for a human -- is this a limitation the consumer
    should document, or a defect that happens to surface as a refusal? -- asked
    once rather than once per cell.
    """
    groups: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for entry in undocumented:
        # Digits vary per cell (an offset, a length) without changing the
        # question, so they are masked for grouping and kept in the examples.
        text = re.sub(r"\d+", "N", entry["reason"])
        groups.setdefault((entry["consumer"], text), []).append(entry)
    return [
        {
            "consumer": consumer,
            "rejection": text,
            "cells": len(entries),
            "types": sorted({e["type"] for e in entries}),
            "offsets": sorted({e["offset"] for e in entries}),
            "lengths": sorted({e["length"] for e in entries}),
            "examples": [e["case_id"] for e in entries[:3]],
        }
        for (consumer, text), entries in sorted(groups.items(), key=lambda kv: -len(kv[1]))
    ]


def load_run(run_json: Path) -> list[dict[str, Any]]:
    run = json.loads(run_json.read_text(encoding="utf-8"))
    workers = []
    for w in run["workers"]:
        path = Path(w["results_path"])
        rows = (
            [json.loads(x) for x in path.read_text(encoding="utf-8").splitlines() if x]
            if path.is_file()
            else []
        )
        header = rows[0] if rows and "worker_protocol" in rows[0] else None
        workers.append(
            {
                "name": w["name"],
                "header": header,
                "lines": rows[1:] if header else rows,
                "not_run": w.get("not_run", []),
                "suspect": w.get("suspect"),
                "protocol_errors": w.get("protocol_errors", []),
            }
        )
    return workers


def print_summary(report: dict[str, Any]) -> None:
    n = report["N"]
    print(f"model: coverage_model_ver {report['coverage_model_ver']}, N = {n}")
    for name, s in report["consumers"].items():
        states = ", ".join(f"{k} {v}" for k, v in s["states"].items())
        version = s["consumer_version"] or "?"
        print(f"  {name:<10} {version:<10} {states}")
        if s["fault_injection"]:
            print(f"             fault injection {s['fault_injection']}: not a measurement")
    for key, label in (
        ("defects", "defects"),
        ("undocumented_limits", "undocumented limits (on the review list)"),
        ("compatibility", "compatibility entries"),
        ("representation", "representation differences"),
        ("splits", "cells where consumers split"),
    ):
        print(f"{label}: {len(report[key])}")
    for d in report["defects"][:10]:
        print(
            f"  DEFECT {d['consumer']} {d['case_id']} [{d['type']} {d['length']} "
            f"off={d['offset']} {d['buffers']} {d['alignment']}]: {d['reason'][:160]}"
        )
    for group in report["review"][:10]:
        print(
            f"  REVIEW {group['consumer']} x{group['cells']} types={group['types']} "
            f"offsets={group['offsets']} lengths={group['lengths']}: {group['rejection'][:140]}"
        )
    for problem in report["problems"]:
        print(f"  PROBLEM {problem}")
    claim = report["claim"]
    for name, mine in claim["per_consumer"].items():
        if mine["instrument"]:
            print(f"claim: {name}: none -- a test instrument, not a consumer")
        elif mine["claimable"]:
            print(f"claim: {name}: {mine['text']}, {mine['disagree']} disagree")
        else:
            print(f"claim: {name}: not claimable -- {claim['holes'][name]} cells not run")
    if claim["claimable"]:
        print(f"claim, every consumer: {claim['text']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("run_json", type=Path, help="run.json from abi-coordinator")
    parser.add_argument("--manifest", type=Path, default=ROOT / "corpus" / "a" / "manifest.tsv")
    parser.add_argument("--limits", type=Path, default=LIMITS)
    parser.add_argument("--out", type=Path, help="default: report.json next to run.json")
    parser.add_argument(
        "--expect-divergence",
        metavar="WORKER",
        help="exit 1 unless WORKER shows silent divergences and no other kind of defect: "
        "for a planted fault, e.g. the faulty worker in drop-offset mode",
    )
    args = parser.parse_args()

    cells, _ = enumerate_model()
    manifest = {case_id: cell for case_id, _size, cell in read_manifest(args.manifest)}
    report = build(cells, manifest, load_run(args.run_json), load_limits(args.limits))
    out = args.out or args.run_json.with_name("report.json")
    out.write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    print_summary(report)
    print(f"report: {out}")
    if args.expect_divergence:
        return check_planted(report, args.expect_divergence)
    return 0


def check_planted(report: dict[str, Any], name: str) -> int:
    """A planted divergence has to surface as one, and only as one.

    Says nothing about the real consumers in the same run: their results are
    findings, and a check that pinned them would fail the day one is fixed.
    """
    if name not in report["consumers"]:
        print(f"MISMATCH: no worker named {name!r}")
        return 1
    mine = [d for d in report["defects"] if d["consumer"] == name]
    diverged = [d for d in mine if d["reason"].startswith("silent divergence")]
    if not diverged or len(diverged) != len(mine):
        print(f"MISMATCH: {name}: {len(diverged)} divergences among {len(mine)} defects")
        return 1
    if report["consumers"][name]["states"]["disagree"] != len(diverged):
        print(f"MISMATCH: {name}: disagree count does not match its divergences")
        return 1
    print(f"ok  planted divergence surfaced: {name} disagrees on {len(diverged)} cells")
    return 0


if __name__ == "__main__":
    sys.exit(main())
