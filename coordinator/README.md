# coordinator — worker isolation

Rust. Landed in **M1** (issue #1), and an architectural requirement rather than
later hardening: a consumer segfault must not take down the run, the other
consumers, or the report.

The C Data Interface itself stays in-process — a rebuilt producer and the
consumer must live in the same worker. What is isolated is one consumer from
another. Each worker reports timeout, exit code, signal, stdout/stderr,
sanitizer output and the result artifact, which is what turns a crash into an
observed datum instead of the end of the run.

The contract with a worker is [docs/worker-protocol.md](../docs/worker-protocol.md).
This crate only supervises; it never touches a consumer.

```sh
cmake --build build                 # the workers are C targets
cargo test                          # the isolation tests, which really do crash one
cargo run -p abi-coordinator -- \
    --worker null=./build/adapters/abi-worker-null \
    --cases corpus/a --limit 100 --out /tmp/run
```

An interpreted worker gets its script and arguments through the repeatable
`--worker-arg <name>=<arg>`; `adapters/README.md` has the Python worker's
invocation.

A run directory holds `cases.txt`, one `<worker>.results.jsonl`,
`<worker>.stdout` and `<worker>.stderr` per consumer, and `run.json`.

## What the report says, and what it does not

- **`rejected` is not a defect.** The specification lets a consumer decline a
  type, a non-zero offset or unaligned memory provided it documents the
  limitation, so a clean rejection is a compatibility entry.
- **`not-run` is never a pass.** `docs/coverage-matrix.md` §4 defines it as a
  hole in the measurement, "including a crash that ended the worker", and N/N is
  claimable only when it is zero.
- **`suspect` is an attribution, not a verdict.** It is the first case the
  worker did not report, which is the one it was working on when it died — but a
  worker can die on case 37 because of state left by case 12.

`run.json` records what each worker did. What it *means* against the model —
each of the N cells, per consumer, as `agree` / `disagree` / `not-run` /
`inexpressible`, the defects, the compatibility and undocumented-limit routing,
and whether the coverage claim can be made — is `tools/diff_report.py`, which
reads a run directory and writes `report.json` beside it:

```sh
python tools/diff_report.py /tmp/run/run.json
```

A rejection is routed to the compatibility matrix only when
`docs/documented-limits.toml` records, with a verbatim quote, that the consumer
documents the limitation. Everything else goes to a review list, grouped by
rejection text, because a clean refusal is also what some defects look like
(apache/arrow-rs#10910).
