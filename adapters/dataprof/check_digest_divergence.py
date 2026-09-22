#!/usr/bin/env python3
"""Show that the logical digest catches a consumer that silently drops the offset.

Silent divergence is the one defect class a crash detector cannot see: the
consumer takes the data, finishes cleanly and returns the wrong values. The
logical digest (docs/digest.md) exists to make it observable, which is a claim,
so this makes a divergence really happen and checks the digest reports it --
cell by cell, not "somewhere".

The divergent consumer is `abi-worker-faulty` in `drop-offset` mode. It digests
each case as handed over (`sent`) and as a consumer that ignores
`ArrowArray.offset` would read it back (`received`): the same buffers, offset
zeroed. Whether that read changes the data is predicted independently, by
pyarrow: import the case, rebuild the column over the same buffers at offset 0,
and compare the two value lists under the digest's normalization rules (null
slots ignored, -0.0 equal to +0.0, one NaN). The worker and pyarrow must agree
on every cell of Corpus A.

The prediction is not "every offset != 0 cell". Length-zero cells hold no values
to shift, and a length-one bool with no nulls reads `false` in the window and
`false` before it (tools/gen_corpus.c fills the pre-window slots with different
values precisely so that such coincidences are rare, not impossible). Stating
the rule by hand would be a third implementation of the fill; asking pyarrow
makes it a second voice instead.

Also checked: `physical` moves exactly when the offset does, since the offset is
a declared field and `physical` covers declared fields.

Needs pyarrow, the `_abicase` extension, the built faulty worker, and corpus/a.
"""

from __future__ import annotations

import json
import math
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
MANIFEST = ROOT / "corpus" / "a" / "manifest.tsv"


def find_worker() -> pathlib.Path:
    """The faulty worker, under a single- or multi-config build tree."""
    build = pathlib.Path(os.environ.get("ABI_BUILD_DIR", ROOT / "build"))
    name = "abi-worker-faulty" + (".exe" if os.name == "nt" else "")
    for sub in ("", "RelWithDebInfo", "Debug", "Release"):
        candidate = build / "adapters" / sub / name
        if candidate.is_file():
            return candidate
    raise SystemExit(f"no {name} under {build}; build it, or set ABI_BUILD_DIR")


def normalize(values: list[Any]) -> list[Any]:
    """docs/digest.md 2, as far as it reaches a Python value list."""

    def one(v: Any) -> Any:
        if isinstance(v, float) and math.isnan(v):
            return "nan"
        # -0.0 == 0.0 already, so it needs no rewriting to compare equal.
        return v

    return [one(v) for v in values]


def predict_divergence(case_path: pathlib.Path) -> bool:
    """pyarrow's opinion: does an offset-free read change the data?"""
    import _abicase
    import pyarrow

    case = _abicase.load(str(case_path))
    try:
        column = pyarrow.record_batch(case).column(0)
        shifted = pyarrow.Array.from_buffers(column.type, len(column), column.buffers(), offset=0)
        return normalize(column.to_pylist()) != normalize(shifted.to_pylist())
    finally:
        case.release_all()


def run_worker(worker: pathlib.Path, paths: list[str]) -> dict[str, Any]:
    with tempfile.TemporaryDirectory() as tmp:
        cases = pathlib.Path(tmp) / "cases.txt"
        results = pathlib.Path(tmp) / "results.jsonl"
        cases.write_text("".join(p + "\n" for p in paths), encoding="utf-8")
        env = dict(os.environ, ABI_FAULTY_MODE="drop-offset")
        subprocess.run(
            [str(worker), "--cases", str(cases), "--results", str(results)],
            cwd=ROOT,
            env=env,
            check=True,
        )
        lines = results.read_text(encoding="utf-8").splitlines()
    header, *rows = (json.loads(line) for line in lines)
    if header.get("consumer_version") != "drop-offset":
        raise SystemExit(f"worker did not run in drop-offset mode: {header}")
    return {row["case"]: row for row in rows}


def main() -> int:
    if not MANIFEST.is_file():
        raise SystemExit(f"no corpus at {MANIFEST.parent}; run tools/gen_corpus.py first")
    rows = [
        line.split("\t")
        for line in MANIFEST.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]
    paths = {row[0]: f"corpus/a/{row[0]}.abicase" for row in rows}
    worker = find_worker()
    results = run_worker(worker, list(paths.values()))
    print(f"corpus:  {len(rows)} cases, worker {worker.name} (drop-offset)")

    problems: list[str] = []
    diverged = 0
    for case_id, _size, kind, length, offset, nulls, buffers, align, _life in rows:
        cell = f"{case_id} [{kind} {length} off={offset} {nulls} {buffers} {align}]"
        line = results.get(paths[case_id])
        if line is None:
            problems.append(f"{cell}: no result line")
            continue
        digest = line.get("digest", {})
        sent, received = digest.get("sent"), digest.get("received")
        if not sent or not received:
            problems.append(f"{cell}: digest incomplete: {digest}")
            continue

        predicted = predict_divergence(ROOT / paths[case_id])
        observed = sent["logical"] != received["logical"]
        diverged += observed
        if predicted != observed:
            problems.append(
                f"{cell}: pyarrow says the data {'changes' if predicted else 'survives'}, "
                f"the logical digest says it {'changes' if observed else 'survives'}"
            )
        moved = sent["physical"] != received["physical"]
        if moved != (offset != "0"):
            problems.append(f"{cell}: physical digest moved={moved} with offset {offset}")

    print(f"diverged: {diverged} cells, per the logical digest")
    print(f"survived: {len(rows) - diverged} cells")
    for problem in problems[:20]:
        print(f"  MISMATCH {problem}")
    if problems:
        print(f"{len(problems)} problem(s)")
        return 1
    # A check that never sees a divergence has proved nothing about detecting one.
    if diverged == 0:
        print("no cell diverged: the worker is not dropping the offset")
        return 1
    print("ok  the logical digest and pyarrow agree on every cell")
    return 0


if __name__ == "__main__":
    sys.exit(main())
