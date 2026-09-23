# corpus

Generated and hand-written cases, by class.

| Directory | Class | Meaning |
|---|---|---|
| `a/`  | A  | conforming producer, valid but rare input |
| `b1/` | B1 | producer invalid in structure or data |
| `b2/` | B2 | producer invalid in protocol or lifecycle |
| `c/`  | C  | consumer misuse |

The committed M0 fixture lives in `libabi/tests/fixtures/` instead, because it
is a format test rather than a case for any engine.

## Generating `a/`

Corpus A is the frozen conformance model of
[docs/coverage-matrix.md](../docs/coverage-matrix.md), instantiated. It is
**generated, not committed** — several megabytes reproducible from two
files already in the repository:

```sh
cmake --build build --target abicase-gen
python tools/gen_corpus.py          # writes corpus/a/ and audits the result
cd adapters/dataprof && python check_corpus.py   # reads it back as arrays
```

`gen_corpus.py` checks the container and `check_corpus.py` checks the contents.
The first would pass unchanged with the validity bitmap written backwards; the
second imports every case through the C Data Interface and compares every slot
against the model.

The enumeration is not duplicated: `tools/coverage_matrix.py` produces the
tuples and `abicase-gen` builds the case each one describes. The driver then
checks what came out against the model — per-lifecycle counts, the tuple
set itself, case-id uniqueness, and that every manifest row has the file it
claims.

Each case is **one field inside a struct**, not a bare top-level array. A bare
`i` at the top level is legal C Data Interface but is refused by every
record-batch importer before any dimension of the model has been looked at, so a
corpus of them would measure nothing. The seven dimensions describe the field;
the struct root is fixed and uninteresting so that everything varying between
cases varies in the field.

`manifest.tsv` maps each case id back to its model cell. That mapping is
deliberately *not* inside the files: the case id is a digest over the payload,
so a tuple stamped into the case would make every id unique by construction and
the uniqueness check meaningless. A duplicate id means two cells of the model
built the same case, which is a finding about the model (coverage-matrix §8).

Two lifecycles are constructible today, `direct` and `moved` — 3760 of
N = 5650. A `moved` case is the `direct` array with `MOVE_STRUCT` ahead of the
handoff in its CALLSEQ, which the executor performs and the lifecycle state
machine checks. The three stream lifecycles wait on the C Stream Interface
(#4); the generator counts what it skipped rather than passing over it
silently.

`b1/`, `b2/` and `c/` are enumerated defects rather than a product of
equivalence classes, so they are hand-written and committed, and they are not
counted in N.

## `b1/`

Ten producers invalid in structure or data, each written out in
`tools/gen_b1.c` beside the clause it breaks, and each carrying that clause as
its `spec_clause` -- an entry of `docs/spec-citations.md`, quoted verbatim.
`manifest.tsv` lists them. Two kinds:

- **`REJECT`, six cases** -- the defect is in something a consumer can see: a
  NULL validity buffer with nulls, a NULL data buffer, fewer child pointers
  than declared, a `null_count` the bitmap contradicts, decreasing offsets, a
  dictionary index out of range. A validator at the declared level must refuse
  them, and say why.
- **`UNSPECIFIED`, four cases** -- a buffer smaller than the array needs. The C
  Data Interface carries no buffer sizes, so no consumer can see it, and the
  specification places the obligation on the producer alone. What a consumer
  does with one is recorded, never judged.

```sh
cmake --build build --target abicase-gen-b1
python tools/check_b1.py      # reproducible, canonical, cited, judged as declared
```

The committed files are what the generator writes; `check_b1.py` regenerates
them and requires byte equality, so a case cannot drift from its definition.
The harness itself digests none of them: a digest trusts the buffers it reads,
and on these that read would be ours.
