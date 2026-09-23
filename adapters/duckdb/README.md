# adapters/duckdb — DuckDB as a C Data Interface consumer

DuckDB's Arrow import is its own code, not a re-export of Arrow C++, which is
what makes it an independent voice in a differential run (issue #12).

## Provenance

| | |
|---|---|
| Version | DuckDB 1.5.5 |
| Source | the tag's source archive, `archive/refs/tags/v1.5.5.tar.gz` |
| SHA-256 | `f33155ff962e6e1e08fd1e9caffa487d4325aa60999e2eabc76feff534d6558b` |
| Commit | `d8cdaa33fda8df955cc76ef58a280f68f4cd43fa` |

GitHub publishes no hash for a generated source archive, so the SHA-256 is the
one this project computed when pinning it. The archive's own embedded commit id
(`git get-tar-commit-id`) is the tag's commit as the GitHub API reports it,
which is what ties the hash to the release. CMake refuses an archive with any
other hash.

It is built by DuckDB's own CMake as an external project, not vendored, and
not from the release amalgamation. The amalgamation is one 25 MB translation
unit. Built plainly it took 13.5 minutes and 13.7 GB of compiler memory, and
under ASan it was still swapping on a 15 GB host after 31 minutes. The source
build took 20.5 minutes on 6 jobs, with about 3 GB per compiler.

```sh
cmake -S . -B build -G Ninja -DABI_DUCKDB=ON [-DABI_SANITIZERS=ON]
cmake --build build --target abi-worker-duckdb
```

DuckDB is built as RelWithDebInfo, with its assertions as released, and
without the parquet extension, the shell or its unit tests. Under
`ABI_SANITIZERS` there are two departures from the shipped build:

- DuckDB itself is instrumented with ASan and UBSan, except UBSan's alignment
  check (see the first finding below).
- jemalloc is off. DuckDB allocates its vectors through it, and ASan cannot
  see into an allocator it did not replace.

## What it exercises

| Case | DuckDB entry point | Ownership, per DuckDB's header |
|---|---|---|
| schema + array | `duckdb_schema_from_arrow`, `duckdb_data_chunk_from_arrow` | the schema stays the caller's; the array becomes the data chunk's |
| stream | not run | — |

The chunk DuckDB built is exported back with `duckdb_data_chunk_to_arrow`, so
every accepted line carries a `received` digest. DuckDB moves what it takes
into its own storage, so its releases are judged without the strict location
rule, as pyarrow's are.

**Streams are not run.** DuckDB's current C API has no stream import. The
deprecated one, `duckdb_arrow_scan`, never releases the schemas it asks the
stream for; see the finding below. Run through it, every stream cell of the
model would report that one known leak, and nothing else in those cells could
be seen. So DuckDB makes no claim over the stream lifecycles, as dataprof makes
none.

## Findings

### An unaligned buffer is kept, and read through a misaligned pointer

**Status: reproduced on 1.5.5 built from the pinned source, and on DuckDB's
development branch (`v2.0-cyanoptera` at `795e1c1`, a Debug build, which has
UBSan on by default). Not filed upstream yet**; no DuckDB issue found for it.

Two sites, the same on both builds. One unaligned case per type, alignment and
offset in Corpus A, each run in its own process:

| Buffer | Misaligned by | UBSan |
|---|---|---|
| `utf8` offsets | +1 | `SetVectorString<uint32_t>`, **during the import** |
| `int32` values | +1 | `ArrowScalarBaseData<int>::Append`, on the zero-copied vector |
| `int64`, `double` values | +1, +4 | `ArrowScalarBaseData<…>::Append`, likewise |
| `int32` values, `utf8` offsets | +4 | none: aligned for 4 bytes |
| `bool` | any | none |

The C Data Interface lets a consumer decline unaligned memory, provided it
documents that ("Consumers MAY decide not to support unaligned memory").
DuckDB does not decline it. `duckdb_data_chunk_from_arrow` makes the caller's
values buffer the data of its own vector, with no copy, at the same unaligned
address. DuckDB's typed reads through that vector are then loads of a
misaligned `int32_t`, which is undefined behaviour in C++. Corpus A found it
on its first unaligned cell (`int32`, length one, offset 1, alignment +1).

[`findings/unaligned_int32.c`](findings/unaligned_int32.c) reproduces it with
DuckDB's C API and nothing of this project: four `int32` values one byte past
a 4-byte boundary, imported, then exported with `duckdb_data_chunk_to_arrow`.
Against DuckDB built with UBSan's alignment check on:

```
src/include/duckdb/common/arrow/appender/scalar_data.hpp:116:62: runtime error:
  load of misaligned address 0x5c750dfaac21 for type 'const int', which requires 4 byte alignment
    #0 in duckdb::ArrowScalarBaseData<int, int, duckdb::ArrowScalarConverter>::Append(...)
    #1 in duckdb::ArrowAppendData::AppendChild(...) src/common/arrow/appender/append_data.cpp:39
    #2 in duckdb::ArrowAppender::Append(...) src/common/arrow/arrow_appender.cpp:41
    #3 in duckdb::ArrowConverter::ToArrowArray(...) src/common/arrow/arrow_converter.cpp:23
    #4 in duckdb_data_chunk_to_arrow src/main/capi/arrow-c.cpp:60
```

On x86-64 the load happens to work and the values come back intact. That is
the hardware tolerating it, not the code being correct. On a platform that
traps on misaligned loads, or under a compiler that exploits the UB, it need
not work.

So DuckDB is built here with `-fno-sanitize=alignment`, carved out by name as
refval carves out nanoarrow's (#945). Without the carve-out, UBSan would abort
the worker at the first unaligned cell and hide everything after it. The ctest
`duckdb_finding_unaligned_int32` checks the precondition on every sanitizer
run: that DuckDB's vector data *is* the caller's unaligned buffer. It fails the
day a pinned DuckDB copies or refuses such a buffer, and the carve-out comes
out then.

### `duckdb_arrow_scan` leaks every schema it asks for — known upstream

**Status: known and not fixed. Not filed from here.** duckdb/duckdb#16050
proposed the fix and was closed unmerged on 2025-10-01, because the API is
deprecated in favour of the new Arrow C API (duckdb/duckdb#18246).

`duckdb_arrow_scan` calls the stream's `get_schema`, and so does the view's
bind (`FactoryGetSchema`). Each time, it sets the returned root `release` to
NULL and never calls it. Under the C Stream Interface, the caller of
`get_schema` owns the result and must release it.

[`findings/arrow_scan_schema_leak.c`](findings/arrow_scan_schema_leak.c)
reproduces it with DuckDB's C API and nothing of this project. Against 1.5.5
built from the pinned source, with sanitizers:

```
sum(x) = 6
DuckDB v1.5.5: get_schema() called 5 time(s), 0 schema(s) released
```

LeakSanitizer attributes the leaked schemas to `duckdb_arrow_scan`
(`src/main/capi/arrow-c.cpp:448`) and to `FactoryGetSchema` (`:359`) under
`ArrowScanBind`. The query answer is right; only ownership is wrong.

The ctest `duckdb_finding_arrow_scan_schema_leak` requires the leak, so it fails
when a pinned DuckDB stops leaking or drops the API. At that point, this
section and the worker's stream decision need revisiting.
