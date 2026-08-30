# adapters — per-engine consumers

Each adapter sits behind one stable C interface, so adding an engine does not
touch the core and new engines can arrive as outside contributions. Every
adapter but `dataprof` speaks [docs/worker-protocol.md](../docs/worker-protocol.md)
and runs in its own process, supervised by `coordinator/`.

| Adapter | Milestone | Language |
|---|---|---|
| `null`      | M1 — the minimal conforming consumer, and the control voice | C |
| `faulty`    | M1 — a test instrument, not a consumer | C |
| `dataprof`  | M0.5 — smoke test of our own instrument | C + Python |
| `arrow_cpp` | M1 | C++ |
| `duckdb`    | M1 | C++ |
| `arrow_rs`  | M3 | Rust |
| `adbc`      | M3 | C |

`common/` holds the worker runtime the C-family adapters share: argument
parsing, the case list, and the result stream — including the per-case flush
that makes a crash attributable to a case.

`null/` imports a reconstruction and immediately releases it, which is all the
interface asks of a consumer. That makes it the control voice of a differential
run: a case the null consumer cannot survive is a defect on *our* side, because
there is no consumer logic left for it to be in. It reads no buffer and compares
no value, so a silent divergence is invisible to it by construction.

`faulty/` crashes, hangs, exits non-zero or truncates its stream on demand
(`ABI_FAULTY_MODE`, `ABI_FAULTY_AT`). It is never a voice in a differential run;
it exists so the coordinator's abnormal-termination paths are exercised by a
process that really does those things rather than by a test double.

`dataprof/` holds a CPython extension that presents a reconstructed case as an
Arrow PyCapsule producer, plus the smoke harness. Going in through the
PyCapsule interface means the consumer's real production import path is
exercised rather than a bespoke test hook — see `docs/m0.5-smoke.md`. It
predates the worker protocol and still runs standalone.
