//! Worker isolation: one consumer crashing must not end the run.
//!
//! The C Data Interface gives a consumer raw pointers and no way to survive one
//! that dereferences a bad one. Arrow C++ and DuckDB are expected to fault on
//! some of what this project feeds them -- that is what feeding it is for -- and
//! an in-process harness turns the first such finding into the end of the run,
//! the report, and every other consumer's result. That inverts the project's
//! central claim, which is that a crash is a *datum*.
//!
//! So each consumer runs in its own process. What is isolated is one consumer
//! from another; the interface itself stays in-process, because a reconstructed
//! producer and the consumer it is handed to are the object under test and
//! splitting them would test something else.
//!
//! The contract with a worker is `docs/worker-protocol.md`. This crate only
//! supervises: it starts workers, times them, notices how they died, and reads
//! whatever they managed to write down.

pub mod report;
pub mod run;
pub mod worker;

pub use report::{RunReport, WorkerReport};
pub use run::{Run, RunConfig};
pub use worker::{Termination, WorkerSpec};
