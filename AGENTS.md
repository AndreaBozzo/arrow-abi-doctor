# AGENTS.md

Shared instructions for coding agents working in `arrow-abi-doctor`.
`CLAUDE.md` imports this file; Codex and other AGENTS.md-aware tools read it
directly. Keep the two in sync by editing **this** file.

## What this project is

An adversarial, structure-aware harness for the Arrow **C Data Interface** and
**C Stream Interface**. It builds portable, minimized reproducers (`.abicase`
files) and feeds them to real consumers while observing the lifecycle.

Two framings matter and are easy to get wrong in prose:

- It is **robustness testing against trusted-but-defective producers and
  validation boundaries.** It is *not* hardening against malicious C Data
  producers. The specification says the interface is not designed for untrusted
  producers because pointer legitimacy is not verifiable; claiming to fuzz a
  trust boundary that does not exist invites exactly that correction.
- A consumer rejecting a case is **not automatically a defect.** The spec allows
  a consumer to not support a type, a non-zero offset or unaligned memory
  *provided it documents the limitation*. Rejection maps to a compatibility or
  documentation-gap entry; only crashes, leaks, undefined behaviour and silent
  divergence are bugs. `README.md` has the table.

Nested types, run-end, string_view and list_view are **deliberately out of
scope** until after M2. Do not add them opportunistically.

## Commands

```sh
# build and test (Windows or Linux)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DABI_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure

# a single suite, with its own output rather than ctest's summary
./build/libabi/abicase_tests
./build/libabi/reconstruct_tests

# sanitizers and Valgrind -- WSL only, no runtime on MinGW
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DABI_SANITIZERS=ON -DABI_WERROR=ON
cmake --build build/asan && ctest --test-dir build/asan --output-on-failure
valgrind --error-exitcode=9 --leak-check=full --show-leak-kinds=all \
         --errors-for-leak-kinds=all -q ./build/libabi/abicase_tests

# big-endian: the M0 gate, not an optional extra
./tools/cross-arch-check.sh

# adapter smoke and the dataprof regression checks
cd adapters/dataprof && python setup.py build_ext --inplace
python run_smoke.py            # add --log for the full lifecycle event log
python repro_findings.py
python check_leak_accounting.py   # leaked vs. still-held-by-the-consumer (#6)

# the frozen conformance model: recompute N and compare against the document
python tools/coverage_matrix.py            # full breakdown
python tools/coverage_matrix.py --check    # what CI runs

# Corpus A: instantiate the model, audit it, then read it back as arrays
cmake --build build --target abicase-gen
python tools/gen_corpus.py                 # writes corpus/a/, also what CI runs
cd adapters/dataprof && python check_corpus.py   # every slot vs. the model

# lint and format
ruff check . && ruff format --check . && mypy .
clang-format --dry-run --Werror libabi/src/*.c libabi/src/*.h \
    libabi/include/abi/*.h libabi/tests/*.c libabi/tests/*.h \
    tools/*.c adapters/dataprof/*.c
```

The C tests are a hand-rolled harness, not a framework: they print
`N checks, M failure(s)` and return non-zero on failure. There is no way to run
one check in isolation; run the suite.

Environment specifics — which host has sanitizers, why setuptools cannot find
MSVC here, how to build a dataprof wheel from a given commit — are in
[docs/dev-environment.md](docs/dev-environment.md).

## Architecture

**`.abicase` is a description, not a memory image.** `ArrowSchema` and
`ArrowArray` hold raw pointers and Arrow's metadata encoding is native-endian
`int32`, so no portable image exists. The file describes a case; the structures
are rebuilt on the target host. Two properties carry the design:

- **Allocations and views are separate.** Serializing buffers independently
  would destroy aliasing — two children sharing one allocation would come back
  as two identical-but-distinct allocations, and every ownership test built on
  them would silently stop testing anything. The format stores backing
  allocations, and separately the views (allocation id, byte offset, length)
  onto them. Aliasing is verified by *pointer comparison*, never by comparing
  contents.
- **The encoding is strictly canonical.** Exactly one valid byte string per
  case, so `encode(decode(b)) == b` byte for byte and the payload digest is a
  stable case identity. Anything that makes two encodings valid is a bug.

Pipeline, and the file to read for each stage:

```
case.c        build/own an AbiCase (arena-allocated)
validate.c    structural + canonicality + class rules
encode.c      AbiCase -> canonical bytes      decode.c  bytes -> AbiCase
reconstruct.c AbiCase -> real ArrowSchema/ArrowArray, plus the observer
adapters/     present a reconstruction to a consumer (dataprof: PyCapsules)
```

Three things in `reconstruct.c` that look wrong until you know why:

1. **Metadata is written in native byte order**, contradicting the format's
   every-integer-is-little-endian rule. Deliberate: Arrow's metadata wire form
   is native-endian `int32`, so it must match the host the consumer runs on.
   The big-endian run is the only place that half of the encoder is exercised.
2. **Release frees what was allocated, not what was declared.** A B1 case may
   declare `n_children = 5` while providing 2. The extra slots are real,
   NULL-initialized, so a consumer walking `n_children` hits a NULL — the defect
   under test, in the consumer. Allocating only the provided count would have it
   read past *our* array, making the fault ours and aborting under a sanitizer.
   The real slot count therefore lives in `private_data`.
3. **The observer records release *depth*, not counts.** A counter cannot
   distinguish `consumer -> parent.release() -> child.release()` (correct) from
   `consumer -> child.release()` (violation). A release entered at depth 0 came
   from outside; a *child* release at depth 0 is the violation. Allocation
   figures come from an instrumented allocator with a per-block size header —
   without one, "0 leaks" is declared rather than measured.

Case classes: **A** conforming-but-rare, **B1** invalid structure or data, **B2**
invalid protocol or lifecycle, **C** consumer misuse. Validation enforces
per-class rules — `USE_AFTER_RELEASE` is rejected outside class C, for instance —
so an executor can trust what it is handed.

## Evidence discipline

This is the project's actual asset. A harness that reports something it did not
measure is worth less than no harness.

- **Confirm against the code that will ship.** A published wheel can be far
  behind master; a finding reproduced only against it proves nothing. Build from
  the branch and re-run. Reading the source is supporting evidence, not
  confirmation.
- **State status per finding**, never in a blanket sentence covering several.
- **Prefer a reproducer that does not depend on this tool.** Where none exists
  because the invalid input is unreachable through the consumer's own
  constructors, say so explicitly — that fact is itself the argument for the
  harness.
- **Never write "nanoarrow accepts it, therefore it is valid."** nanoarrow is
  one voice; it has had validation bugs of its own.
- **`docs/coverage-matrix.md` is normative and frozen.** N is computed by
  `tools/coverage_matrix.py`, not maintained by hand, and CI fails if the two
  disagree. A class may not be removed because it produced a disagreement; §8 of
  that document says what a legitimate change looks like.
- **The corpus generator does not enumerate the model.**
  `tools/coverage_matrix.py` produces the tuples; `tools/gen_corpus.c` builds
  the case each one describes. A second enumeration would drift, and the drift
  would show up as a coverage figure rather than as a failing test. Nothing
  tuple-derived may enter the payload either: the case id is a digest over it,
  so a tuple stamped into `notes` or a per-case seed would make every id unique
  by construction and the duplicate-id check vacuous. `manifest.tsv` carries
  that mapping instead.
- **Well-formed is not correct.** `gen_corpus.py` checks the container:
  canonical bytes, a file per model cell, unique ids. It would pass unchanged
  if the validity bitmap were written most-significant-bit first — verified,
  by doing it. `adapters/dataprof/check_corpus.py` is the other half: it
  imports every case through the C Data Interface and compares every slot
  against the model. Its copy of the fill rules is a deliberate second
  implementation, the opposite case from the enumeration above; do not
  "unify" the two.
- **A regression check that has never failed guards nothing.** Run it against
  the unfixed build and confirm it fails. `repro_findings.py` is written this
  way: it passes on dataprof master and fails on the 0.10.0 wheel.
- Do not claim a measurement the run did not make. The allocator counter means
  "outstanding at this instant", which is why it is called `outstanding` and not
  `leaked`: while a capsule cut from the case is still alive it owns bytes that
  nothing has leaked. `run_smoke.py` reports **leaked** and **still held by the
  consumer** separately, and `check_leak_accounting.py` guards the distinction
  (issue #6).

## Conventions

- **C**: C11, two-space indent, 80 columns, `clang-format` applied across the
  tree and gated in CI. `libabi/include/abi/arrow_abi.h` is vendored verbatim
  from the Arrow spec and is excluded via `.clang-format-ignore` — do not
  reformat or "clean up" that file.
- **Python**: ruff + mypy, 100 columns, configured in `pyproject.toml`. Consumer
  imports are deliberately deferred into the functions that use them (PLC0415 is
  disabled) so a host lacking pyarrow can still import the harness.
- **Catch `BaseException`, not `Exception`, around consumer calls.** pyo3 derives
  `PanicException` from `BaseException` specifically so it is not swallowed; the
  `# noqa: BLE001` sites are load-bearing and annotated.
- **Line endings are LF**, enforced by `.gitattributes`. `*.abicase` is binary:
  never touch those bytes.
- **Commits and PRs carry no AI attribution** — no `Co-Authored-By` trailers, no
  "Generated with" footers.
- Roadmap work is tracked in GitHub issues under the M1 milestone; the milestone
  order in `README.md` is a dependency order, not a wish list.

## Traps that have cost real time

- **A quoted heredoc still strips one backslash level** when a script reads it
  on stdin, so a patch pattern containing an escape never matches and reports
  itself as "pattern not found". Use an exact-match editor for those. Always
  assert the expected replacement count: a bulk edit that does not count its
  matches reports success on zero replacements.
- **Git Bash `/tmp`, WSL `/tmp` and Windows `%TEMP%` are three different
  places.** A Windows binary cannot open a Git Bash `/tmp/...` path at all.
- **`compile_commands.json` cannot be linked into the source tree at configure
  time** — CMake writes it during generate. `.clangd` points at `build/`
  instead. Do not reintroduce the `file(CREATE_LINK ...)` block.
- **`cl.exe` ignores `-std=c11` with a warning**, silently building
  C89-with-extensions. `adapters/dataprof/setup.py` picks flags per compiler.
