//! The run report: what was assigned, what came back, and what did not.

use std::path::PathBuf;

use serde::Serialize;
use serde_json::Value;

use crate::worker::Termination;

/// Bumped when the shape below changes in a way a reader would notice.
pub const REPORT_SCHEMA: &str = "abi-doctor/run-report/1";

#[derive(Debug, Serialize)]
pub struct RunReport {
    pub schema: &'static str,
    /// Absolute path of the directory holding every artifact of this run.
    pub run_dir: PathBuf,
    pub cases_assigned: usize,
    pub workers: Vec<WorkerReport>,
}

impl RunReport {
    /// Workers that were destroyed rather than returning. Not a run failure --
    /// recording these instead of dying with them is the point of the crate.
    pub fn crashed(&self) -> impl Iterator<Item = &WorkerReport> {
        self.workers.iter().filter(|w| w.termination.is_abnormal())
    }

    /// True when every worker was started and supervised. A consumer that
    /// crashed still leaves the *run* complete; a worker whose binary was
    /// missing does not, because nothing was measured.
    pub fn complete(&self) -> bool {
        !self
            .workers
            .iter()
            .any(|w| matches!(w.termination, Termination::SpawnFailed { .. }))
    }
}

#[derive(Debug, Serialize)]
pub struct WorkerReport {
    pub name: String,
    pub program: PathBuf,
    /// The worker's own header line, verbatim: consumer, version, host,
    /// compiler, sanitizers. `docs/abicase-format.md` 4 puts these in the
    /// observation rather than in the case, so this is where they live.
    /// `None` when the worker died before writing one.
    pub header: Option<Value>,
    pub termination: Termination,
    pub duration_ms: u128,

    pub cases_assigned: usize,
    pub cases_reported: usize,
    /// By `status` as the worker reported it. `rejected` is a compatibility
    /// entry, not a defect: the specification lets a consumer decline a type,
    /// a non-zero offset or unaligned memory provided it documents doing so.
    pub accepted: usize,
    pub rejected: usize,
    pub errored: usize,

    /// Assigned but never reported. `docs/coverage-matrix.md` 4 calls this
    /// `not-run` and is explicit that it is a hole in the measurement and never
    /// a pass -- "including a crash that ended the worker".
    pub not_run: Vec<String>,

    /// The first unreported case: the one the worker was on when it stopped
    /// reporting. An attribution, not a verdict -- a worker can die on case 37
    /// because of state left behind by case 12 -- which is why the field is
    /// called `suspect`.
    pub suspect: Option<String>,

    /// The worker broke its own contract: it exited cleanly with cases
    /// unreported, or reported a case nobody assigned it. Distinct from a
    /// crash, and distinct from a finding about Arrow.
    pub protocol_errors: Vec<String>,

    pub results_path: PathBuf,
    pub stdout_path: PathBuf,
    pub stderr_path: PathBuf,
}

impl WorkerReport {
    /// One line for the human summary.
    pub fn summary_line(&self) -> String {
        let verdict = if self.termination.is_abnormal() {
            "CRSH"
        } else if !self.protocol_errors.is_empty() {
            "PROT"
        } else if self.termination.is_clean() {
            "ok  "
        } else {
            "--  "
        };
        let mut line = format!(
            "  [{}] {:<12} {:>4}/{} reported ({} accepted, {} rejected, {} error) | {}",
            verdict,
            self.name,
            self.cases_reported,
            self.cases_assigned,
            self.accepted,
            self.rejected,
            self.errored,
            self.termination.summary()
        );
        if let Some(suspect) = &self.suspect {
            line.push_str(&format!(
                "\n           not-run: {}, suspect: {}",
                self.not_run.len(),
                suspect
            ));
        }
        for problem in &self.protocol_errors {
            line.push_str(&format!("\n           protocol: {problem}"));
        }
        line
    }
}
