# arrow-abi-doctor

A structure-aware test harness for the Arrow **C Data Interface** and **C Stream
Interface**. It builds portable, minimized reproducers (`.abicase` files),
hands them to real consumers in isolated processes, and observes the whole
lifecycle: imports, moves, releases, leaks and the data that comes back.

**Status: M1 complete.** Corpus A covers a frozen conformance model of
N = 5650 cells ([docs/coverage-matrix.md](docs/coverage-matrix.md)).

| Consumer | Build | Agree | Disagree | Not run |
|---|---|---|---|---|
| null (control) | this repo | 5650 | 0 | 0 |
| nanoarrow 0.9.0 (reference validator) | vendored | 5650 | 0 | 0 |
| pyarrow 25.0.1 | wheel | 5650 | 0 | 0 |
| Arrow C++ 25.0.1 | source, ASan + UBSan | 5650 | 0 | 0 |
| DuckDB 1.5.5 | source, ASan + UBSan | 3760 | 0 | 1890 — no stream import in its C API |
| dataprof 0.11.0 (arrow-rs) | wheel | 3760 | 0 | 1890 — refuses stream-only producers |

"Agree" means the case ran its call sequence, the lifecycle was clean, nothing
leaked, and the data that came back has the logical digest of what was sent.

## Findings

| Where | What | Status |
|---|---|---|
| dataprof | two FFI defects, one a panic across the boundary ([record](docs/m0.5-smoke.md)) | fixed: [#608](https://github.com/AndreaBozzo/dataprof/pull/608), [#610](https://github.com/AndreaBozzo/dataprof/pull/610) |
| arrow-rs | zero-length `Utf8` slice at a non-zero offset overruns its values buffer | fixed: [apache/arrow-rs#10910](https://github.com/apache/arrow-rs/issues/10910) |
| nanoarrow | validation reads unaligned offsets through misaligned loads (UB) | filed: [apache/arrow-nanoarrow#945](https://github.com/apache/arrow-nanoarrow/issues/945) |
| DuckDB | Arrow import reads unaligned buffers through misaligned loads (UB) | filed: [duckdb/duckdb#26076](https://github.com/duckdb/duckdb/issues/26076) ([record](adapters/duckdb/README.md#findings)) |

## Scope

This is robustness testing against **trusted-but-defective** producers and
validation boundaries, not hardening against malicious ones. The specification
says the interface is not designed for untrusted producers.

A rejection is not automatically a defect: the specification lets a consumer
skip a type, non-zero offsets or unaligned memory, provided it documents that.

| Consumer outcome | Verdict |
|---|---|
| clean rejection, documented | compatibility entry |
| clean rejection, undocumented | documentation gap |
| crash, leak, undefined behaviour | bug |
| accepts, returns different data | bug |

Covered by the generated corpus:

| Feature | Now | M3 |
|---|---|---|
| int32, int64, float64, utf8, bool | yes | yes |
| validity bitmaps, all-null, no-null | yes | yes |
| offset ≠ 0, slicing, length 0 | yes | yes |
| moves; streams with one batch, EOF, early release | yes | yes |
| unusual alignment, aliased buffers | yes | yes |
| several batches, mid-stream errors (#19) | no | yes |
| dictionaries | no | yes |
| nested types, run-end, views | no | no |

## The `.abicase` format

No portable memory image of an Arrow C structure exists: it holds raw pointers,
and schema metadata is native-endian. An `.abicase` is a canonical
*description* instead, rebuilt into real structures on the target host.

- **Allocations and views are separate**, so aliasing survives a round trip:
  two buffers sharing one allocation come back sharing it.
- **The encoding is canonical.** `encode(decode(b)) == b` byte for byte, so the
  payload digest identifies the case.

Specs: [format](docs/abicase-format.md) ·
[worker protocol](docs/worker-protocol.md) · [digests](docs/digest.md) ·
[spec citations](docs/spec-citations.md)

## Build

C11, CMake ≥ 3.16, Ninja.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DABI_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

| Option | Adds |
|---|---|
| `-DABI_SANITIZERS=ON` | ASan and UBSan, Linux |
| `-DABI_DUCKDB=ON` | DuckDB, built from pinned source |
| `-DABI_CXX=ON -DABI_ARROW_CPP=ON` | Arrow C++, built from pinned source |

The engines build on Linux only and take 10–20 minutes each.
[AGENTS.md](AGENTS.md) lists every command: corpus generation, the coordinator
and the differential report. [docs/dev-environment.md](docs/dev-environment.md)
covers host setup.

## Milestones

- **M0** — the format: canonical, alias-aware, verified on a big-endian host. ✅
- **M0.5** — smoke test against dataprof. ✅
- **M1** — Corpus A and B1, reference validator, lifecycle observer, dual
  digest, worker isolation, and Arrow C++ and DuckDB as a differential pair. ✅
- **M2** — 90-day measurement. Succeeds on either (A) a cross-consumer defect
  filed upstream and accepted, or (B) no disagreement across all N cells.
- **M3** — invalid-lifecycle corpus (B2), producer-mode adapters, arrow-rs and
  ADBC adapters, dictionaries.

## Layout

```
libabi/       the format, reconstruction, lifecycle observer, digests
tools/        abicase CLI, model enumerator, corpus generators, report
coordinator/  Rust: one process per consumer, so a crash is a result
adapters/     workers: null, faulty, dataprof/pyarrow, arrow_cpp, duckdb
refval/       nanoarrow as the reference validator
corpus/       a/ is generated; b1/ is committed
docs/         specifications and records
```

## License

Apache-2.0, the same as Arrow, so cases can go upstream to `arrow-testing`
unchanged.
