# coordinator — worker isolation

Rust. Lands in **M1**, and is an architectural requirement rather than later
hardening: a consumer segfault must not take down the run, the other consumers,
or the report.

The C Data Interface itself stays in-process — a rebuilt producer and the
consumer must live in the same worker. What is isolated is one consumer from
another. Each worker reports timeout, exit code, signal, stdout/stderr,
sanitizer output and the result artifact, which is what turns a crash into an
observed datum instead of the end of the run.
