# adapters — per-engine consumers

Each adapter sits behind one stable C interface, so adding an engine does not
touch the core and new engines can arrive as outside contributions.

| Adapter | Milestone | Language |
|---|---|---|
| `dataprof`  | M0.5 — smoke test of our own instrument | C + Python |
| `arrow_cpp` | M1 | C++ |
| `duckdb`    | M1 | C++ |
| `arrow_rs`  | M3 | Rust |
| `adbc`      | M3 | C |

`dataprof/` holds a CPython extension that presents a reconstructed case as an
Arrow PyCapsule producer, plus the smoke harness. Going in through the
PyCapsule interface means the consumer's real production import path is
exercised rather than a bespoke test hook — see `docs/m0.5-smoke.md`.
