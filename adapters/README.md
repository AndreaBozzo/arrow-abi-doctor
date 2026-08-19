# adapters — per-engine consumers

Each adapter sits behind one stable C interface, so adding an engine does not
touch the core and new engines can arrive as outside contributions.

| Adapter | Milestone | Language |
|---|---|---|
| `dataprof`  | M0.5 — smoke test of our own instrument | C |
| `arrow_cpp` | M1 | C++ |
| `duckdb`    | M1 | C++ |
| `arrow_rs`  | M3 | Rust |
| `adbc`      | M3 | C |

M0 has none of these, deliberately: nothing in this repository yet says anything
about any engine's behaviour.
