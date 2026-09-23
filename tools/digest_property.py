#!/usr/bin/env python3
"""Check the dual digest's central claim over the generated corpus.

`docs/digest.md` says the logical digest covers the type, the length, the
validity and the values, and that offset, alignment and buffer-state are layout
rather than data. The C tests assert that on hand-built pairs; this asserts it
across every case the model produces, which is where it would actually matter.

The generator derives every value from the *logical* index, so two cases whose
(type, length-class, null-pattern) agree hold the same data however they are laid
out. The logical digest must therefore partition the corpus exactly by those
three dimensions: equal within each group, distinct between them. Checking only
the first half would pass for a digest that returned a constant, and only the
second for one that hashed the layout as well.

Digests describe a run, not a file (`docs/digest.md` §5), which is why they are
computed here rather than stored in the corpus manifest: the manifest is a
portable artifact and must stay byte-identical across hosts, and a logical
digest is not — buffer contents are native-endian and the values do not survive
an endianness change (format §10.1).
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from coverage_matrix import Case
from gen_corpus import find_generator

ROOT = Path(__file__).resolve().parent.parent


def find_cli(explicit: str | None) -> Path:
    """The abicase binary, found the same way the generator is."""
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise SystemExit(f"no abicase at {path}")
        return path
    generator = find_generator(None)
    for name in ("abicase", "abicase.exe"):
        candidate = generator.parent / name
        if candidate.is_file():
            return candidate
    raise SystemExit(f"no abicase binary beside {generator}")


# Lifecycles that deliver a schema and no array.
DATA_FREE = ("early-release", "EOF")


def read_tuples(manifest: Path) -> dict[str, Case]:
    cases: dict[str, Case] = {}
    for line in manifest.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        cases[fields[0]] = Case(*fields[2:9])
    return cases


def digest_all(cli: Path, paths: list[Path]) -> dict[str, tuple[str, str]]:
    """physical and logical digest per case id, from one process."""
    # Bytes, not text: on Windows a text-mode pipe translates newlines on write
    # and the far side translates again, which corrupts what the child reads.
    payload = b"\n".join(str(p).encode("utf-8") for p in paths) + b"\n"
    result = subprocess.run(
        [str(cli), "digest", "-"], input=payload, capture_output=True, check=False
    )
    sys.stderr.write(result.stderr.decode("utf-8", "replace"))
    if result.returncode != 0:
        raise SystemExit(f"{cli} digest failed with {result.returncode}")

    digests: dict[str, tuple[str, str]] = {}
    for line in result.stdout.decode("utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        physical, logical, path = line.split("\t")
        digests[Path(path).stem] = (physical, logical)
    return digests


def check(cases: dict[str, Case], digests: dict[str, tuple[str, str]]) -> list[str]:
    problems: list[str] = []
    missing = set(cases) - set(digests)
    if missing:
        problems.append(f"{len(missing)} cases were not digested, e.g. {sorted(missing)[0]}")

    groups: dict[tuple[str, str, str], set[str]] = defaultdict(set)
    for case_id, case in cases.items():
        if case_id in digests:
            groups[(case.type, case.length, case.nulls)].add(digests[case_id][1])

    for key, values in sorted(groups.items()):
        if len(values) != 1:
            problems.append(
                f"logical digest is not constant across layouts for {key}: "
                f"{len(values)} distinct values"
            )

    seen: dict[str, tuple[str, str, str]] = {}
    for key, values in sorted(groups.items()):
        for digest in values:
            if digest in seen and seen[digest] != key:
                problems.append(
                    f"logical digest {digest[:12]} is shared by {seen[digest]} and {key}, "
                    "so it does not distinguish data that differs"
                )
            seen[digest] = key
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", default=str(ROOT / "corpus" / "a"))
    parser.add_argument("--abicase", default=None, help="path to the abicase binary")
    args = parser.parse_args()

    corpus = Path(args.corpus)
    manifest = corpus / "manifest.tsv"
    if not manifest.is_file():
        raise SystemExit(f"no corpus at {corpus}; run tools/gen_corpus.py first")

    # The data-free lifecycles deliver a schema and no array (coverage-matrix
    # 1.7): nothing to digest, and so outside a property about data. Excluded
    # by name and counted, never by whether a digest happened to fail.
    everything = read_tuples(manifest)
    cases = {k: c for k, c in everything.items() if c.lifecycle not in DATA_FREE}
    paths = [corpus / f"{case_id}.abicase" for case_id in cases]
    digests = digest_all(find_cli(args.abicase), paths)

    problems = check(cases, digests)
    for problem in problems:
        print(f"MISMATCH: {problem}")
    if problems:
        return 1

    groups = {(c.type, c.length, c.nulls) for c in cases.values()}
    physical = {p for p, _ in digests.values()}
    print(
        f"ok  {len(digests)} cases digested; the logical digest partitions them into "
        f"exactly {len(groups)} groups, one per (type, length-class, null-pattern)"
    )
    print(
        f"ok  {len(everything) - len(cases)} data-free cases ({', '.join(DATA_FREE)}) "
        "carry no array and are outside the property"
    )
    print(
        f"ok  {len(physical)} distinct physical digests, fewer than the case count because "
        "alignment and aliasing are properties of an address, not of bytes (digest.md §3)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
