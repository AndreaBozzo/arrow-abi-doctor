# refval — reference validator

nanoarrow's `ArrowArrayViewValidate()` behind the worker protocol:
`abi-worker-refval`, run by the coordinator like any consumer.

**A baseline, not ground truth.** nanoarrow has had validation bugs of its own
in exactly this area (arrow-nanoarrow#626, offset-buffer validation for sliced
arrays). Its verdict is one voice in a multi-voice comparison, never the
deciding one: "nanoarrow accepts it, therefore it is valid" is a sentence this
project does not write.

**What it checks is an obligation, not an opinion.** Arrow's security model
(https://arrow.apache.org/docs/dev/format/Security.html, February 2026) asks
implementors for validation APIs that detect invalid data "without crashing on
invalid data", and `ArrowArrayViewValidate()` is nanoarrow's. The property under
test is therefore *validated without crashing*, not *returned the verdict we
expected*: a nanoarrow crash or hang is a finding at the same weight as a
consumer's, which is why it runs behind the same worker isolation rather than
in-process.

Each case is validated at the level it declares in `EXPECTED`
(`docs/abicase-format.md` §9). The format's four levels mirror nanoarrow's by
value on purpose, so nothing translates between the case and the verdict. The
validator is told the level through the executor's `begin` hook
(`abi/callseq.h`) — the one consumer allowed to read what a case expects,
because checking the expectation is its job. It attaches with
`ArrowArrayViewSetArrayMinimal()` and then validates at exactly that level:
`SetArray()` alone would validate at `default` whatever the case declared.

```sh
cmake --build build                 # builds refval/ with everything else
python tools/check_refval.py        # corpus A valid at full; B1 refused, and why
```

`check_refval.py` holds it to known verdicts: all of Corpus A (valid by
construction, declared `full`), the smoke fixture, and the B1 fixture whose
dictionary index 99 is out of range for a two-value dictionary. A validator
that quietly validated at a lower level than declared would still accept
Corpus A; the B1 fixture is what it would let through, and the check fails.

## The vendored nanoarrow

`nanoarrow/` is **nanoarrow 0.9.0**, pinned so that a verdict is attributable to
a version the way the coverage model demands (issue #3). It is nanoarrow's own
single-file bundle, not an edited copy, and it is excluded from the formatter
so a diff against a regenerated bundle stays meaningful.

| | |
|---|---|
| Release | `apache-arrow-nanoarrow-0.9.0`, published 2026-07-31 |
| Source tarball | `apache-arrow-nanoarrow-0.9.0.tar.gz`, SHA-512 below, checked against the release's `.sha512` |
| `include/nanoarrow/nanoarrow.h` SHA-256 | `144134a7ec6b36fcce2098ea99f208a8a039d6322b16ac1e29f89bfd28668112` |
| `src/nanoarrow.c` SHA-256 | `71df0f041932023f666fceae9a9e087bc26263b1058e69be5dd7ca656e08f04a` |
| License | Apache-2.0; `LICENSE.txt` and `NOTICE.txt` are the release's, verbatim |

Tarball SHA-512:

```
2fbdfe3274da9dcba5e3215ba0a7ff66da9f65395d1800841f0dc9a6bbc00b8c
c224f900bcb946c91969b3c6e79d132ad5077c9a537f861502c4763dbffb33b8
```

Regenerated with, from the extracted release:

```sh
python ci/scripts/bundle.py --output-dir <out> --symbol-namespace AbiRefval
```

keeping `include/nanoarrow/nanoarrow.h` and `src/nanoarrow.c`, with line endings
normalized to LF (the bundler writes the host's; the hashes above are of the LF
files). `--symbol-namespace` prefixes every exported symbol with `AbiRefval`,
so this copy can never collide with another nanoarrow linked into the same
process. It is built without the project's warning set — its warnings are
upstream's to fix — but with the sanitizers when they are on: a memory error
inside the reference validator is a finding like any other.

The first thing it did was correct us: `libabi/tests/fixture.h` claimed the
rich fixture's dictionary indices were out of range. They are 0 and 1 over a
two-value dictionary; nanoarrow validated it at `full`, and pyarrow's full
validation agreed.

## Findings

### nanoarrow validates unaligned offsets through misaligned `int32_t` loads

**Status: confirmed, filed upstream as
[apache/arrow-nanoarrow#945](https://github.com/apache/arrow-nanoarrow/issues/945)
(2026-09-23), and fixed there by
[#946](https://github.com/apache/arrow-nanoarrow/pull/946) (merged 2026-09-25),
which reads every site listed below through an unaligned-safe load. Not yet in
a release: the vendored 0.9.0 still does it, so the carve-out stays until a
release carrying #946 is vendored. That the fix silences the reproducer is read
from its diff, not yet run here.**

`ArrowArrayViewValidateDefault()` reads the first and last offsets of a `utf8` /
`binary` array as `data.as_int32[i]`. When the offsets buffer is not 4-byte
aligned, that is a misaligned load -- undefined behaviour in C, which x86
tolerates silently and a strict-alignment target need not. The input is legal:

> It is recommended, but not required, that the memory addresses of the buffers
> be aligned at least according to the type of primitive data that they
> contain. Consumers MAY decide not to support unaligned memory.
> — `docs/source/format/CDataInterface.rst`, apache/arrow main, read 2026-09-23

nanoarrow does not decline unaligned buffers and does not document declining
them; it validates the array and calls it valid. Under the table in the
top-level README that is undefined behaviour, a bug under every outcome, and
not a documented limitation.

- **Found** by `check_refval.py` under ASan + UBSan: every Corpus A `utf8` case
  at the `+1` alignment class (123 of 125 -- the two that do not are
  zero-length at offset 0), at exactly one site. No other type or alignment class triggers it: nanoarrow's
  validation never reads a primitive's data buffer, and `+4` keeps `int32`
  offsets aligned.
- **Reproduced without this project**: `findings/unaligned_offsets.c` builds a
  three-element string array with nanoarrow's own API, moves the offsets one
  byte, and calls `ArrowArrayViewSetArray()`. UBSan reports two misaligned
  loads; without a sanitizer it prints "valid".
- **Confirmed on the code that will ship**: the vendored 0.9.0 release, and a
  bundle of nanoarrow `main` at `ec8a58cae1` (2026-09-18, `0.10.0-SNAPSHOT`).
- **Wider than the corpus shows**: probed one process per case against `main`,
  it is also the `ArrowAssertIncreasingInt32/Int64()` loops at `full`,
  `binary`, and `large_string` at both `+1` and `+4` (int64 offsets); the list,
  map and large_list branches read offsets the same way, by inspection. A
  `memcpy` load at those sites removes every report with verdicts unchanged.
- **Not the same as** apache/arrow-nanoarrow#323, a bus error reported through
  R/ADBC and closed in 2023.
- **Guarded**: CI runs the reproducer under UBSan and requires the report
  (`refval_finding_unaligned_offsets`), and builds the validator itself with
  `-fno-sanitize=alignment` so this one known site does not mask everything
  else ASan and UBSan can catch in it. Both come out when a vendored nanoarrow
  stops doing it -- the test fails that day, by construction.

A fix would read the offsets with `memcpy` (what nanoarrow's own IPC reader and
most Arrow implementations do for unaligned data), or declare and document that
unaligned offsets are unsupported, which the specification permits.

### nanoarrow's `full` validation does not compare `null_count` to the bitmap

**Status: a divergence between validators, recorded; not a bug, not filed.**

Corpus B1's `null-count-disagrees` is a four-row `int32` array whose bitmap
marks two slots null and whose `null_count` says one. `cdi-null-count` makes
the field "The number of null items in the array", so the case is invalid.
nanoarrow accepts it at `full`; pyarrow's full validation refuses it with
`null_count value (1) doesn't match actual number of nulls in array (2)`.

nanoarrow never claimed otherwise: its `full` level is documented as "Validate
all buffer sizes and all buffer content", and `null_count` is a field of the
structure, not buffer content. So this is a difference in what two validators
cover, not one of them breaking a promise. `tools/check_b1.py` pins it by name
(`VALIDATOR_DIVERGENCES`), so a nanoarrow that starts checking fails the check
and the entry comes out. Whether nanoarrow should check it is a question for
its maintainers, not a defect report.

