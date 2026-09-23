#!/usr/bin/env python3
"""Check Corpus B1: reproducible, canonical, cited, and judged as declared.

Corpus B1 is hand-written (tools/gen_b1.c) and committed (corpus/b1/). This
holds the committed files to five things:

1. **They are what the generator writes.** It regenerates the corpus into a
   temporary directory and requires every file, and the manifest, to match
   byte for byte -- so a case cannot drift from the definition that sits
   beside its clause, and nothing hand-edited slips in.
2. **They are canonical.** `abicase verify` on every file: decode, re-encode,
   byte equality.
3. **Every clause is cited.** Each case's `spec_clause` must name an entry of
   docs/spec-citations.md, and so must the clause Corpus A cases carry. A case
   whose expectation cannot be attributed to a clause is an opinion.
4. **The reference validator judges them as declared.** `REJECT` cases must be
   refused at their declared level; `UNSPECIFIED` ones -- undersized buffers no
   consumer can see -- are accepted at `none`. Where nanoarrow's verdict
   differs from the case's, the difference is named in VALIDATOR_DIVERGENCES
   with the evidence, and pinned: a change on either side fails this check.
5. **The harness survives them.** The null worker imports and releases every
   case with a clean lifecycle and nothing outstanding, and digests none of
   them: a digest trusts the buffers it reads, and on these the read would be
   the harness's own out-of-bounds access.

Needs the build (abicase, abicase-gen-b1, the null and refval workers) and, for
the Corpus A clause, a generated corpus/a.
"""

from __future__ import annotations

import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent
B1 = ROOT / "corpus" / "b1"
CITATIONS = ROOT / "docs" / "spec-citations.md"

# Cases where the pinned nanoarrow's verdict is not the case's expectation. Each
# entry is evidence, not an excuse: what nanoarrow does, why, and who else was
# asked. The check requires exactly this verdict, so either side changing fails.
VALIDATOR_DIVERGENCES = {
    "null-count-disagrees": (
        "accepted",
        "nanoarrow's FULL level validates buffer sizes and buffer content, and "
        "never compares null_count to the bitmap; pyarrow's full validation "
        "refuses the same case: null_count value (1) doesn't match actual "
        "number of nulls in array (2)",
    ),
}


def built(subdir: str, stem: str) -> pathlib.Path:
    build = pathlib.Path(os.environ.get("ABI_BUILD_DIR", ROOT / "build"))
    name = stem + (".exe" if os.name == "nt" else "")
    for config in ("", "RelWithDebInfo", "Debug", "Release"):
        candidate = build / subdir / config / name
        if candidate.is_file():
            return candidate
    raise SystemExit(f"no {name} under {build / subdir}; build it, or set ABI_BUILD_DIR")


def citation_ids() -> set[str]:
    text = CITATIONS.read_text(encoding="utf-8")
    return {m.strip() for m in re.findall(r"^### (.+)$", text, flags=re.M)}


def clause_of(abicase: pathlib.Path, case: pathlib.Path) -> str | None:
    """The spec_clause as the case file says it, not as any manifest does.

    None when the file does not decode -- reported by the caller as a problem
    with that case, not raised: one bad file should not hide the others.
    """
    dump = subprocess.run(
        [str(abicase), "dump", str(case)], capture_output=True, text=True, check=False
    )
    match = re.search(r'^\s*spec_clause:\s*"(.*)"\s*$', dump.stdout, flags=re.M)
    return match.group(1) if dump.returncode == 0 and match else None


def manifest(path: pathlib.Path) -> list[dict[str, str]]:
    fields = ("name", "case_id", "expected", "level", "clause")
    return [
        dict(zip(fields, line.split("\t"), strict=True))
        for line in path.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]


def run_worker(worker: pathlib.Path, cases: list[str]) -> dict[str, dict[str, Any]]:
    with tempfile.TemporaryDirectory() as tmp:
        listing = pathlib.Path(tmp) / "cases.txt"
        results = pathlib.Path(tmp) / "results.jsonl"
        listing.write_text("".join(c + "\n" for c in cases), encoding="utf-8")
        run = subprocess.run(
            [str(worker), "--cases", str(listing), "--results", str(results)],
            cwd=ROOT,
            check=False,  # a crash is reported as a problem, not raised
        )
        if run.returncode != 0:
            return {"__exit__": {"code": run.returncode}}
        lines = [json.loads(x) for x in results.read_text(encoding="utf-8").splitlines()]
    return {pathlib.Path(line["case"]).stem: line for line in lines[1:]}


def check_reproducible(gen: pathlib.Path) -> list[str]:
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([str(gen), tmp], check=True, capture_output=True)
        fresh = {p.name: p.read_bytes() for p in pathlib.Path(tmp).iterdir()}
    committed = {p.name: p.read_bytes() for p in B1.iterdir() if p.name != "README.md"}
    for name in sorted(set(fresh) | set(committed)):
        if name not in committed:
            problems.append(f"{name}: the generator writes it and it is not committed")
        elif name not in fresh:
            problems.append(f"{name}: committed, and the generator does not write it")
        elif fresh[name] != committed[name]:
            problems.append(f"{name}: committed bytes differ from the generator's")
    return problems


def check_validator(rows: list[dict[str, str]], lines: dict[str, Any]) -> list[str]:
    if "__exit__" in lines:
        return [f"the reference validator exited {lines['__exit__']['code']}"]
    problems = []
    for row in rows:
        line = lines.get(row["name"])
        if line is None:
            problems.append(f"{row['name']}: the validator reported nothing")
            continue
        want = "rejected" if row["expected"] == "REJECT" else "accepted"
        if row["name"] in VALIDATOR_DIVERGENCES:
            want = VALIDATOR_DIVERGENCES[row["name"]][0]
        if line["status"] != want:
            problems.append(f"{row['name']}: {line['status']}, expected {want}: {line['detail']}")
        elif want == "accepted" and f"valid at {row['level']}" not in line["detail"]:
            problems.append(f"{row['name']}: accepted, not at {row['level']}: {line['detail']}")
        if (line.get("lifecycle") or {}).get("violations"):
            problems.append(f"{row['name']}: validator lifecycle {line['lifecycle']}")
    return problems


def check_harness(rows: list[dict[str, str]], lines: dict[str, Any]) -> list[str]:
    if "__exit__" in lines:
        return [f"the null worker exited {lines['__exit__']['code']}"]
    problems = []
    for row in rows:
        line = lines.get(row["name"], {})
        where = row["name"]
        if line.get("status") != "accepted":
            problems.append(f"{where}: null worker {line.get('status')}: {line.get('detail')}")
        if (line.get("lifecycle") or {}).get("violations"):
            problems.append(f"{where}: lifecycle {line['lifecycle']}")
        if (line.get("observer") or {}).get("outstanding", 1):
            problems.append(f"{where}: bytes outstanding: {line.get('observer')}")
        digest = line.get("digest") or {}
        if "sent" in digest or "not digested" not in digest.get("sent_error", ""):
            problems.append(f"{where}: an invalid case was digested: {digest}")
    return problems


def main() -> int:
    abicase = built("tools", "abicase")
    rows = manifest(B1 / "manifest.tsv")
    cited = citation_ids()
    problems = check_reproducible(built("tools", "abicase-gen-b1"))

    for row in rows:
        case = B1 / f"{row['name']}.abicase"
        verify = subprocess.run(
            [str(abicase), "verify", str(case)], capture_output=True, check=False
        )
        if verify.returncode != 0:
            problems.append(f"{row['name']}: not canonical: {verify.stderr.decode().strip()}")
        clause = clause_of(abicase, case)
        if clause is None:
            problems.append(f"{row['name']}: does not decode")
            continue
        if clause != row["clause"]:
            problems.append(f"{row['name']}: file says {clause!r}, manifest {row['clause']!r}")
        if clause not in cited:
            problems.append(f"{row['name']}: spec_clause {clause!r} names no citation entry")
        if row["expected"] not in ("REJECT", "UNSPECIFIED"):
            problems.append(f"{row['name']}: expected {row['expected']}, a B1 case refuses")

    corpus_a = sorted((ROOT / "corpus" / "a").glob("*.abicase"))
    if not corpus_a:
        problems.append("no corpus/a to take Corpus A's clause from; run tools/gen_corpus.py")
    elif (clause := clause_of(abicase, corpus_a[0])) not in cited:
        problems.append(f"Corpus A's spec_clause {clause!r} names no citation entry")

    paths = [f"corpus/b1/{row['name']}.abicase" for row in rows]
    problems += check_validator(rows, run_worker(built("refval", "abi-worker-refval"), paths))
    problems += check_harness(rows, run_worker(built("adapters", "abi-worker-null"), paths))

    reject = sum(row["expected"] == "REJECT" for row in rows)
    print(f"corpus B1: {len(rows)} cases, {reject} REJECT, {len(rows) - reject} UNSPECIFIED")
    print(f"citations: {len(cited)} entries in {CITATIONS.name}")
    for name, (verdict, why) in VALIDATOR_DIVERGENCES.items():
        print(f"  known divergence: nanoarrow {verdict} {name} -- {why[:90]}...")
    for problem in problems:
        print(f"MISMATCH: {problem}")
    if problems:
        print(f"{len(problems)} problem(s)")
        return 1
    print("ok  reproducible, canonical, cited; validator and harness as declared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
