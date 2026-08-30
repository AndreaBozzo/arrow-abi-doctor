# Worker protocol

Status: **normative for M1.** Version `worker_protocol` = 1.

A worker is a process that presents reconstructed cases to exactly one consumer
and writes down what happened. The coordinator supervises workers; it never
touches a consumer itself.

## Why a process boundary at all

The C Data Interface offers no way to survive a consumer that dereferences a
bad pointer. Arrow C++ and DuckDB are expected to segfault on some of what this
project feeds them — that is the point of feeding it — and an in-process harness
turns the first such finding into the end of the run, the report and every other
consumer's result. The design spec (§6) therefore makes isolation an
architectural requirement rather than later hardening.

What is isolated is **one consumer from another**. The interface itself stays
in-process: a reconstructed producer and the consumer it is handed to must live
in the same worker, because the whole object under test is the pointer handoff
between them. Splitting *those* two would test a different thing.

```
                  coordinator
                       |
         +-------------+-------------+
         v             v             v
      worker         worker        worker
       null          dataprof      Arrow C++
         |             |             |
   reconstructs each .abicase locally and presents it in-process
         |             |             |
   result line per case, flushed | exit code | signal | timeout
```

## Invocation

```
<worker> --cases <path> --results <path> [--consumer <name>]
```

- `--cases` — a text file, one case path per line. Blank lines and lines
  beginning with `#` are ignored. Paths are resolved relative to the process's
  working directory, which the coordinator sets to the repository root.
- `--results` — where the worker writes its result stream (below). The
  coordinator creates the containing directory; the worker truncates the file.
- `--consumer` — optional, for workers that front more than one consumer. A
  worker that fronts exactly one may reject the flag or ignore it.

Anything else a worker needs is its own business. Consumer selection,
sanitizer configuration and library paths are the coordinator's environment,
not arguments in this contract.

## The result stream

JSON, one object per line (JSONL), UTF-8, LF. **Every line is flushed before
the next case begins.** That single rule is what makes a crash attributable: if
the worker dies on case 37, lines 1..36 are already in the file and the
coordinator can name case 37 as the one that killed it. A worker that buffers
its output loses the entire run at the first fault and reports nothing about the
case that mattered most.

Flushed, not `fsync`ed. The failure being survived is *process* death — the
kernel still holds a flushed write when the process is gone. Surviving host
death is not a property this harness claims.

### Line 1: the header

```json
{"worker_protocol": 1, "consumer": "dataprof", "consumer_version": "0.11.0",
 "arch": "x86_64", "os": "linux", "compiler": "gcc 13.2.0", "sanitizers": ["address", "undefined"]}
```

`consumer` and `worker_protocol` are required; the rest are recorded when the
worker knows them and omitted when it does not. `docs/abicase-format.md` §4 is
explicit that these belong to the observation and not to the case, which is why
they are reported here and never written into an `.abicase`.

### Then: one line per case, in the order given

```json
{"case": "corpus/a/0022a771….abicase", "id": "0022a771…", "status": "accepted",
 "detail": "3 rows x 1 cols", "observer": {"violations": 0, "outstanding": 0,
 "bytes_allocated": 256, "bytes_freed": 256, "released_by_consumer": true}}
```

`status` is one of:

| `status` | meaning |
|---|---|
| `accepted` | the consumer took the case and completed |
| `rejected` | the consumer refused it and said why, without crashing |
| `error` | the *harness* failed on this case — could not load, decode or reconstruct it |

`case` must be the path **exactly as it appeared in the case list**, byte for
byte. The coordinator matches reported cases to assigned ones by string, so a
worker that helpfully absolutizes or normalizes the path reports cases nobody
assigned it and leaves every real one counted `not-run` — a full set of results
that reads as a total failure.

`rejected` is **not** a defect on its own. The specification permits a consumer
to decline a type, a non-zero offset or unaligned memory provided it documents
the limitation, so a clean rejection is a compatibility-matrix entry. Only
crashes, leaks, undefined behaviour and silent divergence are bugs; `README.md`
has the table.

A worker never reports its own death. A crash has no line, and its absence is
the record.

## Exit

| Exit code | Meaning |
|---|---|
| 0 | every assigned case produced a line |
| 1 | worker-level failure: bad arguments, unreadable case list, unwritable results |

A per-case failure is a result line with `status` `error`, not an exit code. A
worker that exits non-zero because one case failed has destroyed the
coordinator's ability to tell "this consumer is broken" from "this case is".

## What the coordinator adds

The coordinator records, per worker: exit code, terminating signal (POSIX) or
exception code (Windows), whether the wall-clock timeout fired, captured stdout
and stderr, and the parsed result stream.

The timeout kills **the worker process, and only that**. A consumer that forks
its own helpers leaves them running, and this is not yet a process group or a
job object. No consumer in M1 does that; a consumer that does will need one
before its results can be trusted, and this paragraph is here so that is found
by reading rather than by a hung runner.

Cases assigned but never reported are `not-run`, which is the status
`docs/coverage-matrix.md` §4 defines for exactly this — "the case was not
executed, for any reason, including a crash that ended the worker". They are
holes in the measurement, never passes. When the worker terminated abnormally,
the first unreported case is recorded as the **suspect**: the case it was
working on when it died. Suspect is an attribution, not a verdict — a worker can
die on case 37 because of state left by case 12 — and the report says so by
naming it `suspect` rather than `cause`.
