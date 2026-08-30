//! Assigning cases to workers and turning what came back into a report.

use std::collections::HashSet;
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::time::Duration;

use serde_json::Value;

use crate::report::{REPORT_SCHEMA, RunReport, WorkerReport};
use crate::worker::{Termination, WorkerSpec, supervise};

pub struct RunConfig {
    /// Cases to assign, in order. Every worker gets all of them: the point of
    /// running several consumers is that they see the same input.
    pub cases: Vec<PathBuf>,
    pub workers: Vec<WorkerSpec>,
    /// Where the case list, the result streams, the captured output and the
    /// report are written.
    pub run_dir: PathBuf,
    /// Wall clock per worker. A consumer that deadlocks is a finding, and
    /// without this it is an eternity.
    pub timeout: Duration,
    /// Working directory for every worker, so relative case paths mean the
    /// same thing to all of them.
    pub working_dir: PathBuf,
}

pub struct Run;

impl Run {
    /// Runs every worker and returns the report.
    ///
    /// Errors here are failures of the *run* -- an unwritable directory, a case
    /// list that cannot be created. A worker that crashes is not an error; it
    /// is the result.
    pub fn execute(config: &RunConfig) -> io::Result<RunReport> {
        fs::create_dir_all(&config.run_dir)?;

        let assigned: Vec<String> = config
            .cases
            .iter()
            .map(|p| p.to_string_lossy().into_owned())
            .collect();

        let cases_path = config.run_dir.join("cases.txt");
        write_case_list(&cases_path, &assigned)?;

        let mut workers = Vec::with_capacity(config.workers.len());
        for spec in &config.workers {
            let results = config.run_dir.join(format!("{}.results.jsonl", spec.name));
            let stdout = config.run_dir.join(format!("{}.stdout", spec.name));
            let stderr = config.run_dir.join(format!("{}.stderr", spec.name));

            let supervised = supervise(
                spec,
                &cases_path,
                &results,
                &stdout,
                &stderr,
                config.timeout,
                &config.working_dir,
            )?;

            workers.push(collect(
                spec,
                &assigned,
                &results,
                &stdout,
                &stderr,
                supervised.termination,
                supervised.duration,
            ));
        }

        let report = RunReport {
            schema: REPORT_SCHEMA,
            run_dir: config.run_dir.clone(),
            cases_assigned: assigned.len(),
            workers,
        };

        let json = serde_json::to_string_pretty(&report)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        fs::write(config.run_dir.join("run.json"), json + "\n")?;
        Ok(report)
    }
}

/// LF regardless of host: a worker reads this with `fgets` and strips CR, but
/// the file is also an artifact of the run and the repository is LF throughout.
fn write_case_list(path: &Path, cases: &[String]) -> io::Result<()> {
    let mut file = fs::File::create(path)?;
    for case in cases {
        file.write_all(case.as_bytes())?;
        file.write_all(b"\n")?;
    }
    file.flush()
}

#[allow(clippy::too_many_arguments)]
fn collect(
    spec: &WorkerSpec,
    assigned: &[String],
    results: &Path,
    stdout: &Path,
    stderr: &Path,
    termination: Termination,
    duration: Duration,
) -> WorkerReport {
    let mut report = WorkerReport {
        name: spec.name.clone(),
        program: spec.program.clone(),
        header: None,
        termination,
        duration_ms: duration.as_millis(),
        cases_assigned: assigned.len(),
        cases_reported: 0,
        accepted: 0,
        rejected: 0,
        errored: 0,
        not_run: Vec::new(),
        suspect: None,
        protocol_errors: Vec::new(),
        results_path: results.to_path_buf(),
        stdout_path: stdout.to_path_buf(),
        stderr_path: stderr.to_path_buf(),
    };

    // A worker that died before its first write leaves no file at all, which is
    // a complete result and not an error: every case is not-run.
    let text = fs::read_to_string(results).unwrap_or_default();
    let lines: Vec<&str> = text.lines().filter(|l| !l.trim().is_empty()).collect();

    let mut reported: HashSet<String> = HashSet::new();
    let mut order: Vec<String> = Vec::new();

    for (index, line) in lines.iter().enumerate() {
        let last = index + 1 == lines.len();
        let parsed: Value = match serde_json::from_str(line) {
            Ok(value) => value,
            Err(e) => {
                // A process killed partway through writing a line leaves a torn
                // one. That is the expected shape of a crash, not a violation,
                // so it is only a protocol error when the worker had time to
                // finish writing and did not.
                if !(last && report.termination.is_abnormal()) {
                    report
                        .protocol_errors
                        .push(format!("unparseable line {}: {e}", index + 1));
                }
                continue;
            }
        };

        if index == 0 && parsed.get("worker_protocol").is_some() {
            report.header = Some(parsed);
            continue;
        }

        let Some(case) = parsed.get("case").and_then(Value::as_str) else {
            report
                .protocol_errors
                .push(format!("line {} has no `case`", index + 1));
            continue;
        };

        match parsed.get("status").and_then(Value::as_str) {
            Some("accepted") => report.accepted += 1,
            Some("rejected") => report.rejected += 1,
            Some("error") => report.errored += 1,
            Some(other) => report
                .protocol_errors
                .push(format!("line {}: unknown status {other:?}", index + 1)),
            None => report
                .protocol_errors
                .push(format!("line {} has no `status`", index + 1)),
        }

        if !reported.insert(case.to_string()) {
            report
                .protocol_errors
                .push(format!("case reported twice: {case}"));
        }
        order.push(case.to_string());
    }

    if report.header.is_none() && !lines.is_empty() {
        report.protocol_errors.push("no header line".to_string());
    }

    report.cases_reported = reported.len();

    let assigned_set: HashSet<&str> = assigned.iter().map(String::as_str).collect();
    for case in &order {
        if !assigned_set.contains(case.as_str()) {
            report
                .protocol_errors
                .push(format!("reported a case it was not assigned: {case}"));
        }
    }

    // In assignment order, so the first hole is the case the worker stopped on.
    report.not_run = assigned
        .iter()
        .filter(|c| !reported.contains(c.as_str()))
        .cloned()
        .collect();
    report.suspect = report.not_run.first().cloned();

    // Exit 0 means every assigned case produced a line. When it did not, the
    // worker is broken in a way no signal reports, and only this accounting
    // finds it.
    if report.termination.is_clean() && !report.not_run.is_empty() {
        report.protocol_errors.push(format!(
            "exited cleanly with {} case(s) unreported",
            report.not_run.len()
        ));
    }

    report
}
