#!/usr/bin/env python3
"""Generate Corpus A and check the result against the model that defines it.

`abicase-gen` builds cases; this drives it and then audits what came out.
The audit is the point: a generator that quietly emits fewer cases than the
model has cells would still produce a corpus, and the coverage figure computed
over it would be wrong in a way nothing else in the tree would notice.

Every count checked here comes from `coverage_matrix.py`, which is the
executable form of `docs/coverage-matrix.md`, so no number in this file is
maintained by hand.

The corpus is generated, not committed (coverage-matrix.md 3).
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from coverage_matrix import Case, enumerate_model

ROOT = Path(__file__).resolve().parent.parent

# case_id, bytes, then the seven dimensions of the model tuple.
MANIFEST_FIELDS = 9

# Lifecycles abicase-gen can build today. The three stream lifecycles wait on the
# C Stream Interface (issue #4). The generator has the same list and skips the
# rest, so this one exists to state what the corpus therefore does *not* cover.
SUPPORTED_LIFECYCLES = ("direct", "moved")


def find_generator(explicit: str | None) -> Path:
    """The abicase-gen binary, or a clear message about how to get one."""
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise SystemExit(f"no generator at {path}")
        return path
    candidates = [
        ROOT / "build" / "tools" / name for name in ("abicase-gen", "abicase-gen.exe")
    ] + [
        ROOT / "build" / "tools" / config / name
        for config in ("RelWithDebInfo", "Debug", "Release")
        for name in ("abicase-gen", "abicase-gen.exe")
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise SystemExit(
        "no abicase-gen binary found under build/tools; build it with\n"
        "  cmake --build build --target abicase-gen\n"
        "or pass --generator PATH"
    )


def write_tuples(cases: list[Case], path: Path) -> None:
    """One tuple per line, LF, in the model's own enumeration order."""
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        for case in cases:
            handle.write("\t".join(case) + "\n")


def read_manifest(path: Path) -> list[tuple[str, int, Case]]:
    rows: list[tuple[str, int, Case]] = []
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != MANIFEST_FIELDS:
            raise SystemExit(
                f"{path}:{lineno}: expected {MANIFEST_FIELDS} fields, found {len(fields)}"
            )
        rows.append((fields[0], int(fields[1]), Case(*fields[2:])))
    return rows


def audit(out_dir: Path, expected: list[Case]) -> list[str]:
    """Every disagreement between what the model says and what was produced."""
    problems: list[str] = []
    rows = read_manifest(out_dir / "manifest.tsv")

    produced = Counter(case.lifecycle for _, _, case in rows)
    wanted = Counter(case.lifecycle for case in expected if case.lifecycle in SUPPORTED_LIFECYCLES)
    for lifecycle in sorted(set(produced) | set(wanted)):
        if produced[lifecycle] != wanted[lifecycle]:
            problems.append(
                f"lifecycle {lifecycle}: model has {wanted[lifecycle]} cases, "
                f"generator produced {produced[lifecycle]}"
            )

    # The tuples themselves, not just how many there are: a generator that built
    # the right number of the wrong cells would pass a count check.
    if {case for _, _, case in rows} != {
        case for case in expected if case.lifecycle in SUPPORTED_LIFECYCLES
    }:
        problems.append("the tuples generated are not the tuples the model accepts")

    # A collision means two cells of the model built byte-identical cases, so one
    # of them is redundant -- a finding about the model, per coverage-matrix.md 8.
    ids = Counter(case_id for case_id, _, _ in rows)
    for case_id, count in ids.items():
        if count > 1:
            problems.append(f"case id {case_id} produced by {count} different tuples")

    on_disk = {path.stem for path in out_dir.glob("*.abicase")}
    missing = set(ids) - on_disk
    extra = on_disk - set(ids)
    if missing:
        problems.append(f"{len(missing)} manifest rows have no file, e.g. {sorted(missing)[0]}")
    if extra:
        problems.append(f"{len(extra)} files are not in the manifest, e.g. {sorted(extra)[0]}")

    for case_id, size, _ in rows:
        # Rows whose file is absent are already reported above; stat()ing them
        # here would raise instead of adding anything.
        if case_id in missing:
            continue
        actual = (out_dir / f"{case_id}.abicase").stat().st_size
        if actual != size:
            problems.append(f"{case_id}: manifest says {size} bytes, file is {actual}")
            break

    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=str(ROOT / "corpus" / "a"), help="output directory")
    parser.add_argument("--generator", default=None, help="path to abicase-gen")
    args = parser.parse_args()

    generator = find_generator(args.generator)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    for stale in list(out_dir.glob("*.abicase")) + list(out_dir.glob("manifest.tsv")):
        stale.unlink()

    accepted, _ = enumerate_model()
    tuples = out_dir / "tuples.tsv"
    write_tuples(accepted, tuples)

    result = subprocess.run(
        [str(generator), "--out", str(out_dir), str(tuples)],
        capture_output=True,
        text=True,
        check=False,
    )
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    tuples.unlink()
    if result.returncode != 0:
        return result.returncode

    problems = audit(out_dir, accepted)
    for problem in problems:
        print(f"MISMATCH: {problem}")
    if problems:
        return 1

    covered = sum(1 for case in accepted if case.lifecycle in SUPPORTED_LIFECYCLES)
    print(
        f"ok  {covered} of N = {len(accepted)} model cases generated "
        f"({covered * 100 // len(accepted)}%); the rest need the C Stream Interface, issue #4"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
