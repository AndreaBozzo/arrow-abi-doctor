#!/usr/bin/env python3
"""Check the reference validator on cases whose verdict is known.

`abi-worker-refval` runs nanoarrow's ArrowArrayViewValidate() at each case's
declared validation level (refval/README.md). This runs it over:

* all of Corpus A -- every case is valid Arrow by construction and declares
  `full`, so every one must be accepted, and at `full`. nanoarrow is vendored
  and pinned, so this is not pinning a consumer's moving verdict; it is a
  second opinion on the generator. A rejection here means the corpus holds an
  invalid array or nanoarrow has a bug, and either one needs a person.
* the smoke fixture, which must be accepted;
* the B1 fixture whose dictionary index 99 is out of range for a two-value
  dictionary, which a full validation must refuse -- and say why.

Every line must also carry a clean lifecycle and nothing outstanding: a
validator that leaks what it inspected, or blames the consumer for a release
the harness withheld, fails here.

A validator that quietly validated at a lower level than the case declared
would still accept Corpus A. The B1 fixture is what it would let through.
"""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "corpus" / "a" / "manifest.tsv"


def built(subdir: str, stem: str) -> pathlib.Path:
    build = pathlib.Path(os.environ.get("ABI_BUILD_DIR", ROOT / "build"))
    name = stem + (".exe" if os.name == "nt" else "")
    for config in ("", "RelWithDebInfo", "Debug", "Release"):
        candidate = build / subdir / config / name
        if candidate.is_file():
            return candidate
    raise SystemExit(f"no {name} under {build / subdir}; build it, or set ABI_BUILD_DIR")


def check_line(line: dict[str, Any], want: str, reason: str | None) -> list[str]:
    if not line:
        return [f"no line for a {want} fixture: the validator reported nothing for it"]
    where = line["case"]
    problems = []
    if line["status"] != want:
        problems.append(f"{where}: {line['status']}, expected {want}: {line['detail']}")
    elif want == "accepted" and "valid at full" not in line["detail"]:
        problems.append(f"{where}: accepted, but not at full: {line['detail']}")
    elif reason and reason not in line["detail"]:
        problems.append(f"{where}: rejected for the wrong reason: {line['detail']}")
    lifecycle = line.get("lifecycle") or {}
    if lifecycle.get("violations") or lifecycle.get("incomplete"):
        problems.append(f"{where}: lifecycle {lifecycle}")
    if (line.get("observer") or {}).get("outstanding", 1):
        problems.append(f"{where}: bytes outstanding: {line.get('observer')}")
    return problems


def main() -> int:
    if not MANIFEST.is_file():
        raise SystemExit(f"no corpus at {MANIFEST.parent}; run tools/gen_corpus.py first")
    worker = built("refval", "abi-worker-refval")
    abicase = built("tools", "abicase")
    corpus = [
        f"corpus/a/{line.split(chr(9))[0]}.abicase"
        for line in MANIFEST.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]

    with tempfile.TemporaryDirectory() as tmp:
        fixtures = {}
        for name in ("smoke", "bad-dict-index"):
            path = pathlib.Path(tmp) / f"{name}.abicase"
            subprocess.run([str(abicase), "fixture", name, "-o", str(path)], check=True)
            fixtures[name] = str(path)
        cases = pathlib.Path(tmp) / "cases.txt"
        results = pathlib.Path(tmp) / "results.jsonl"
        cases.write_text("".join(p + "\n" for p in [*corpus, *fixtures.values()]), "utf-8")
        run = subprocess.run(
            [str(worker), "--cases", str(cases), "--results", str(results)],
            cwd=ROOT,
            check=False,  # a crash is reported below as a finding, not raised
        )
        if run.returncode != 0:
            print(f"MISMATCH: the validator exited {run.returncode}")
            return 1
        header, *lines = (json.loads(x) for x in results.read_text(encoding="utf-8").splitlines())

    by_case = {line["case"]: line for line in lines}
    problems: list[str] = []
    if len(by_case) != len(corpus) + len(fixtures):
        problems.append(f"{len(by_case)} lines for {len(corpus) + len(fixtures)} cases")
    for case in corpus:
        if case in by_case:
            problems += check_line(by_case[case], "accepted", None)
    problems += check_line(by_case.get(fixtures["smoke"], {}), "accepted", None)
    problems += check_line(
        by_case.get(fixtures["bad-dict-index"], {}), "rejected", "dictionary index"
    )

    print(f"validator: nanoarrow {header.get('consumer_version')}")
    print(f"corpus A:  {len(corpus)} cases, each must be valid at full")
    for problem in problems[:20]:
        print(f"MISMATCH: {problem}")
    if problems:
        print(f"{len(problems)} problem(s)")
        return 1
    print("ok  corpus A and smoke valid at full; the out-of-range index refused, and why")
    return 0


if __name__ == "__main__":
    sys.exit(main())
