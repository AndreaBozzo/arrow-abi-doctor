# arrow-abi-doctor

An adversarial, structure-aware harness for the Arrow **C Data Interface** and
**C Stream Interface**, built around portable, minimized reproducers.

**Status: M0.5.** The `.abicase` container format is implemented and verified
across three platforms including a big-endian one; cases reconstruct into real
`ArrowSchema` / `ArrowArray` structures, and a first adapter feeds them to
`dataprof` and `pyarrow` with the lifecycle observed. Differential pairs, a
reference validator and worker isolation are M1.

---

## Why

The C Data and C Stream interfaces are tested upstream with valid data, between
official Arrow implementations, along the nominal path. `arrow-abi-doctor`
exercises four axes that testing does not systematically cover:

- **valid but rare inputs** — non-zero offsets, slicing, moves, all-null,
  length 0, unusual alignment, aliased buffers;
- **non-Arrow consumers** — DuckDB, ADBC drivers, embedded engines;
- **the lifecycle** — release callbacks, ownership, aliasing, leaks, *observed
  rather than inferred*;
- **portable reproducibility** — every case becomes a transferable artifact you
  can attach to an issue.

### The claim, stated so it can be checked

> There does not appear to be a published adversarial, structure-aware harness
> for C Data / C Stream that combines heterogeneous non-Arrow consumers,
> lifecycle instrumentation, and portable minimized reproducers.

The novelty is in the *combination*, not in the absence of prior art. Arrow has
cross-language integration tests for the C Data Interface
(`archery integration --run-c-data`) and, where possible, also checks that
memory consumption is unchanged after the test. nanoarrow has
`ArrowArrayViewValidate()` at four levels. Both are real, and neither is what
this is.

### What this is not

This is **robustness testing against trusted-but-defective producers, and
against validation boundaries.** It is explicitly *not* hardening against
malicious C Data producers.

That distinction is not diplomacy. The C Data Interface passes raw pointers, and
the specification states it is not designed for untrusted producers, because
pointer legitimacy is not verifiable in general; it does recommend validation
against trusted-but-buggy ones. A project claiming to fuzz a trust boundary that
the specification says does not exist would be answered, correctly, by pointing
that out.

### Evidence the bug class is real

- **DuckDB #21691** — `std::stoi` crash parsing the Arrow format string for
  fixed-size binary/list. Reached before any data exists, which is why a
  schema-only case is a first-class citizen of the format.
- **Arrow #40898** — release callback never invoked on import when a
  non-validity buffer was NULL; the leak was visible only by instrumenting the
  allocator.
- **arrow-nanoarrow #626** — fix to offset-buffer validation for sliced arrays.

nanoarrow is used as a **reference validator**: an independent baseline, not
ground truth. Strong evidence always has several voices:

```
spec        valid (clause quoted verbatim)
nanoarrow   FULL accept
Arrow C++   accept
arrow-rs    accept
<engine>    crash
```

Never: "nanoarrow accepts it, therefore it is valid."

### A disagreement is not automatically a defect

The specification permits a consumer not to support every data type, non-zero
offsets, or unaligned memory, **provided it documents the limitation**.

| Outcome in the consumer | Verdict | Destination |
|---|---|---|
| clean rejection, limitation documented | compatibility | compatibility matrix |
| clean rejection, limitation undocumented | documentation gap | undocumented-limits map |
| crash, leak, undefined behaviour | **bug** | upstream issue |
| accepts and returns divergent values | **bug** | upstream issue |

The "documented limitation" defence covers rejection. It does not cover memory
corruption, and it does not cover silence.

---

## The `.abicase` format

The deliverable of M0, and the thing the rest of the project is scaffolding for.

`ArrowSchema` and `ArrowArray` contain raw pointers, and Arrow's schema metadata
encoding uses **native-endian `int32`** — so no portable memory image of an
Arrow C structure exists. `.abicase` is instead a canonical *description* from
which the structures are rebuilt on the target host.

Two properties earn their complexity:

**Allocations and views are modelled separately.** Serializing buffers as
independent blocks would destroy aliasing: two children sharing one allocation
would come back as two allocations with identical contents, and every ownership
and lifetime test built on them would silently stop testing anything. So the
format stores backing *allocations*, and separately the *views* onto them:

```
buffer A -> allocation 3, offset 0
buffer B -> allocation 3, offset 16
buffer C -> allocation 3, offset 16     exact alias of B
buffer D -> allocation 4, offset 1      intentional misalignment
```

**The encoding is strictly canonical.** Exactly one valid byte string per case,
so `encode(decode(b)) == b` holds byte for byte, and the payload digest is a
stable identity for the case — usable for corpus deduplication and for citing a
reproducer in an issue.

Full specification: [docs/abicase-format.md](docs/abicase-format.md).
Verification records: [M0](docs/m0-verification.md), [M0.5](docs/m0.5-smoke.md).

```
$ abicase selftest
fixture:  rich
size:     908
case_id:  af3ded84db715f0957eec7188f9859ec

$ abicase verify case.abicase
ok   case.abicase: 908 bytes, class A, case_id af3ded84db715f0957eec7188f9859ec

$ abicase dump case.abicase     # structure and topology, host-independent
```

---

## Build

Requires a C11 compiler, CMake ≥ 3.16 and Ninja.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DABI_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

With sanitizers — the truncation and corruption sweeps are only meaningful under
one:

```sh
cmake -S . -B build/asan -G Ninja -DABI_SANITIZERS=ON
cmake --build build/asan && ./build/asan/libabi/abicase_tests
```

Cross-architecture check (needs `gcc-s390x-linux-gnu` and `qemu-user-static`):

```sh
./tools/cross-arch-check.sh
```

`-DABI_CXX=ON` enables the C++ language for the M1 adapters; it is off by
default so a host with only a C compiler can configure. The Python adapter,
lint and format tooling, the sanitizer and big-endian hosts, and what to do when
setuptools cannot find MSVC are all in
[docs/dev-environment.md](docs/dev-environment.md).

---

## Scope

In `.abicase` and verified at M0; **enabled in the generated corpus** per
milestone (§3.6 of the design spec):

| Feature | v0 (M1) | M3 |
|---|---|---|
| int32, int64, float64, utf8, bool | yes | yes |
| null bitmap, all-null, zero-null | yes | yes |
| offset ≠ 0, slicing, length 0 | yes | yes |
| move semantics | yes | yes |
| stream: EOF, early release, mid-stream error | yes | yes |
| non-standard alignment | yes | yes |
| buffer aliasing | yes | yes |
| dictionary + slicing | no | yes |
| nested (list, struct, union, map) | no | no |
| run-end, string_view, list_view | no | no |

Nested types and the newer encodings are **out of scope**, deliberately. The
axes above are the ones where cross-implementation behaviour is least covered
and most likely to differ; adding nested types would multiply the surface
without sharpening the question. That decision gets revisited only after M2.

**Non-goals:** testing every Arrow implementation, generating SQL,
reimplementing Arrow, optimizing for stars or iterations per second.

**ADBC** is an M3 adapter but a declared direction from v0: the ODBC → ADBC
migration window is 2026 and does not stay open long.

---

## Milestones

- **M0** — the format. Canonical encoding, alias-aware allocation/view model,
  provenance. Byte-identical round trip on one host; structural and topological
  equivalence verified between two architectures — not *logical*, since buffer
  data is native-endian and scalar values do not cross an endianness change
  (format spec §10.1). ✅
- **M0.5** — smoke. A `dataprof` adapter, an existing C Data / C Stream consumer
  under our own control. Not a differential pair: a test of the instrument. ✅
  ([record](docs/m0.5-smoke.md)) It turned up two defects in that consumer, both
  filed and both since fixed upstream (dataprof
  [#608](https://github.com/AndreaBozzo/dataprof/pull/608),
  [#610](https://github.com/AndreaBozzo/dataprof/pull/610)). One of them — an
  out-of-range dictionary index panicking across FFI — has no pyarrow-only
  reproducer, because pyarrow refuses to construct the invalid array at all. It
  took a C Data Interface producer to express it, which is the gap this project
  is pointed at.
- **M1** — first differential pair. Corpus A (v0 feature set) and B1, reference
  validator, Arrow C++ and DuckDB adapters, observer with instrumented
  allocator, event log, state machine and dual digest, worker isolation.
  Blocking constraint: `docs/coverage-matrix.md` published **within** M1 —
  defining it afterwards would invalidate M2-B. Now
  [published and frozen](docs/coverage-matrix.md) at `coverage_model_ver` 1,
  N = 5650; `tools/coverage_matrix.py --check` recomputes N in CI so the figure
  cannot drift from the model.
- **M2** — 90 days. Succeeds on either: **(A)** a Corpus A disagreement between
  two consumers, classified as crash/leak or silent divergence, reproduced in a
  `.abicase` under 10 KB, filed upstream and accepted as valid; or **(B)** no
  disagreement across N/N combinations of the pre-declared bounded conformance
  model, with a coverage report showing which surface was explored and which was
  not, plus the undocumented-limitations map.
- **M3** — Corpus B2, Corpus C-act with producer-mode adapters, arrow-rs and
  ADBC adapters, dictionary enabled in Corpus A.
- **M4** — `semdiff`. Starts only after M2.

M2-B is not a consolation prize: it is a measurement of a specific surface of
Arrow interoperability, for which no equivalent published measurement is
apparent. It only counts if the surface was defined beforehand.

---

## Layout

```
libabi/       C -- the .abicase format, the case model, reconstruction,
              the lifecycle observer, generators (M1)
tools/        the abicase CLI, the cross-architecture check, and the
              enumerator for the bounded conformance model
refval/       nanoarrow binding, the reference validator          (M1)
adapters/     per-engine consumers; dataprof lands first (M0.5)
observer/     instrumented allocator, event log, state machine    (M1)
coordinator/  Rust -- worker isolation, timeouts, artifacts       (M1)
corpus/       a/ b1/ b2/ c/
docs/         format spec, coverage matrix, spec citations
```

## License

Apache-2.0. Same licence as Arrow, so cases can be donated upstream to
`arrow-testing` without a relicensing conversation.
