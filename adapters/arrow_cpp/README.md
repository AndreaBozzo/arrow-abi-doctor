# adapters/arrow_cpp — Arrow C++, built under the sanitizers

pyarrow already runs Arrow C++'s import under the coordinator, from a wheel no
one instrumented. There, a crash is observed but an out-of-bounds read that
does not crash is not. This worker calls the same import functions against
Arrow C++ built from source with ASan and UBSan (issue #11). Differences between
the two are therefore about instrumentation, not about the path taken.

## Provenance

| | |
|---|---|
| Version | Apache Arrow 25.0.1 |
| Source | `apache-arrow-25.0.1.tar.gz`, from `archive.apache.org/dist/arrow/arrow-25.0.1/` |
| SHA-256 | `43d5de0a581f43cf63a2c06b4dcf13b9ff6fcd800f023324596e5781093bc500`, as Apache publishes it in `apache-arrow-25.0.1.tar.gz.sha256` |
| Signature | `apache-arrow-25.0.1.tar.gz.asc` verifies against Apache's `KEYS` (`A2AC 7132 B5DA 7C27 3A7A 1476 65F4 A8CA 9769 ECD7`, "Apache Arrow Automated Release Signing") |

CMake refuses an archive whose hash differs. The signature was checked once,
when the version was pinned; the build does not repeat that check.

```sh
cmake -S . -B build -G Ninja -DABI_CXX=ON -DABI_ARROW_CPP=ON [-DABI_SANITIZERS=ON]
cmake --build build --target abi-worker-arrow-cpp
```

Only the core library is built, with every optional component off: the C Data
Interface bridge lives in the core. Under `ABI_SANITIZERS`, Arrow gets this
project's sanitizer flags, not `ARROW_USE_UBSAN`, which leaves out UBSan's
alignment and vptr checks by design. mimalloc is off, since ASan cannot see
into it.

## What it exercises

| Case | Arrow C++ entry point | Same as pyarrow's |
|---|---|---|
| schema + array | `arrow::ImportSchema`, then `arrow::ImportRecordBatch` | `pyarrow.record_batch(capsules)` |
| stream | `arrow::ImportRecordBatchReader`, then `ReadNext` | `RecordBatchReader.from_stream` |

`ImportSchema` releases the C schema whether or not it succeeds. The array and
the stream are moved into objects Arrow keeps alive, so its releases are
judged without the strict location rule, as pyarrow's are. Every batch is
re-exported with `arrow::ExportRecordBatch`, so every accepted line carries a
`received` digest.
