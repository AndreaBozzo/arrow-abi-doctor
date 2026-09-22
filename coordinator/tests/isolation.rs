//! Issue #1's acceptance condition, verified by crashing a worker rather than
//! by reading the code path that would have handled it.
//!
//! These tests need the C workers, which CMake builds. They fail rather than
//! skip when the build is missing: a test that quietly skips is a test that has
//! never failed, and this repository does not count those as guards.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Duration;

use abi_coordinator::{Run, RunConfig, Termination, WorkerSpec};

fn repo_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .to_path_buf()
}

/// Finds a binary CMake built, across the single- and multi-config generators.
///
/// `ABI_BUILD_DIR` overrides the search for a build tree kept elsewhere -- WSL
/// and Windows share this checkout and must not share a build directory.
fn built(subdir: &str, stem: &str) -> PathBuf {
    let root = repo_root();
    let build = std::env::var_os("ABI_BUILD_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| root.join("build"));
    let name = format!("{stem}{}", std::env::consts::EXE_SUFFIX);

    let candidates = [
        build.join(subdir).join(&name),
        // Multi-config generators (MSVC) put the configuration in the path.
        build.join(subdir).join("RelWithDebInfo").join(&name),
        build.join(subdir).join("Debug").join(&name),
        build.join(subdir).join("Release").join(&name),
    ];
    for candidate in &candidates {
        if candidate.is_file() {
            return candidate.clone();
        }
    }
    panic!(
        "{name} not found under {}.\nBuild the C targets first:\n  \
         cmake -S . -B build && cmake --build build\n\
         or point ABI_BUILD_DIR at an existing build tree.",
        build.display()
    );
}

fn scratch(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("abi-coordinator-{name}"));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).expect("scratch dir");
    dir
}

/// `n` real `.abicase` files.
///
/// `abicase selftest` writes the same case every time, so these are copies
/// under distinct names. That is exactly what these tests want: the cases are
/// the units of accounting here, and identical content keeps every worker's
/// per-case work equal, so a difference in the report is a difference in how
/// the worker ended and nothing else.
fn make_cases(dir: &Path, n: usize) -> Vec<PathBuf> {
    let first = dir.join("case-0.abicase");
    let status = Command::new(built("tools", "abicase"))
        .arg("selftest")
        .arg("-o")
        .arg(&first)
        .status()
        .expect("run abicase selftest");
    assert!(status.success(), "abicase selftest failed: {status:?}");

    let mut cases = vec![first.clone()];
    for i in 1..n {
        let path = dir.join(format!("case-{i}.abicase"));
        fs::copy(&first, &path).expect("copy case");
        cases.push(path);
    }
    cases
}

fn config(dir: &Path, cases: Vec<PathBuf>, workers: Vec<WorkerSpec>) -> RunConfig {
    RunConfig {
        cases,
        workers,
        run_dir: dir.join("run"),
        timeout: Duration::from_secs(60),
        working_dir: repo_root(),
    }
}

fn null_worker() -> WorkerSpec {
    WorkerSpec::new("null", built("adapters", "abi-worker-null"))
}

fn faulty(mode: &str, at: usize) -> WorkerSpec {
    WorkerSpec::new("faulty", built("adapters", "abi-worker-faulty"))
        .with_env("ABI_FAULTY_MODE", mode)
        .with_env("ABI_FAULTY_AT", at.to_string())
}

/// The acceptance condition of issue #1.
#[test]
fn a_segfaulting_worker_is_reported_and_the_run_continues() {
    let dir = scratch("segv");
    let cases = make_cases(&dir, 5);
    // The faulty worker is listed *first*, so the surviving worker runs after
    // the crash. Listed second it would already have finished, and the test
    // would pass on a coordinator that simply stopped at the fault.
    let workers = vec![faulty("segv", 3), null_worker()];

    let report = Run::execute(&config(&dir, cases.clone(), workers)).expect("run completes");

    assert!(report.complete(), "every worker was started and supervised");
    assert_eq!(report.cases_assigned, 5);

    let crashed = &report.workers[0];
    assert_eq!(crashed.name, "faulty");
    match &crashed.termination {
        // POSIX and Windows report the same fault under different names.
        Termination::Signal { signal } => assert_eq!(*signal, 11, "SIGSEGV"),
        Termination::Exception { code } => assert_eq!(*code, 0xC000_0005, "access violation"),
        other => panic!("expected a segfault, got {other:?}"),
    }
    assert!(crashed.termination.is_abnormal());

    // It died on the third case, so the first two are recorded and the third is
    // named. This is the property the per-case flush buys: without it the whole
    // stream would be lost and no case could be blamed.
    assert_eq!(
        crashed.cases_reported, 2,
        "cases written before the fault survive"
    );
    assert_eq!(crashed.not_run.len(), 3);
    assert_eq!(
        crashed.suspect.as_deref(),
        Some(cases[2].to_string_lossy().as_ref()),
        "the case it was working on when it died"
    );
    // A crash is not a protocol violation. Conflating them would make every
    // finding look like a broken adapter.
    assert!(
        crashed.protocol_errors.is_empty(),
        "{:?}",
        crashed.protocol_errors
    );

    // The whole point: the other consumer still produced a full result.
    let survivor = &report.workers[1];
    assert_eq!(survivor.name, "null");
    assert_eq!(survivor.termination, Termination::Exited { code: 0 });
    assert_eq!(survivor.cases_reported, 5);
    assert_eq!(survivor.accepted, 5);
    assert!(survivor.not_run.is_empty());
    assert!(survivor.suspect.is_none());
    assert!(
        survivor.protocol_errors.is_empty(),
        "{:?}",
        survivor.protocol_errors
    );

    // The report is an artifact, not just a return value.
    let written = fs::read_to_string(dir.join("run").join("run.json")).expect("run.json");
    assert!(written.contains("abi-doctor/run-report/1"));
}

#[test]
fn a_hanging_worker_is_killed_and_reported_as_a_timeout() {
    let dir = scratch("hang");
    let cases = make_cases(&dir, 3);
    let mut config = config(&dir, cases, vec![faulty("hang", 2), null_worker()]);
    config.timeout = Duration::from_secs(2);

    let report = Run::execute(&config).expect("run completes");

    let hung = &report.workers[0];
    assert!(
        matches!(hung.termination, Termination::TimedOut { .. }),
        "got {:?}",
        hung.termination
    );
    assert_eq!(hung.cases_reported, 1);
    assert!(hung.suspect.is_some());

    // The clock is per worker, so the one after it is unaffected.
    assert_eq!(report.workers[1].cases_reported, 3);
}

#[test]
fn a_nonzero_exit_is_not_treated_as_a_crash() {
    let dir = scratch("exit");
    let cases = make_cases(&dir, 3);

    let report =
        Run::execute(&config(&dir, cases, vec![faulty("exit", 2)])).expect("run completes");

    let worker = &report.workers[0];
    assert_eq!(worker.termination, Termination::Exited { code: 42 });
    assert!(
        !worker.termination.is_abnormal(),
        "a bad exit code is not a fault"
    );
    assert!(!worker.termination.is_clean());
    assert_eq!(worker.cases_reported, 1);
}

/// `exit(-1)` reaches a Windows parent as 0xFFFFFFFF. A coordinator that
/// classified the whole NTSTATUS error range as a fault would file this
/// consumer -- which refused cleanly and said so -- as a crash, and a
/// compatibility entry would become a bug report.
///
/// This discriminates on Windows only; on POSIX the exit code and the signal
/// are separate fields and there was never anything to confuse.
#[test]
fn a_negative_exit_code_is_not_mistaken_for_a_fault() {
    let dir = scratch("negexit");
    let cases = make_cases(&dir, 3);
    let worker = faulty("exit", 2).with_env("ABI_FAULTY_EXIT_CODE", "-1");

    let report = Run::execute(&config(&dir, cases, vec![worker])).expect("run completes");

    let worker = &report.workers[0];
    assert!(
        !worker.termination.is_abnormal(),
        "a negative exit code is not a fault, got {:?}",
        worker.termination
    );
    assert!(
        matches!(worker.termination, Termination::Exited { .. }),
        "got {:?}",
        worker.termination
    );
}

/// A worker that exits 0 having reported less than it was given is broken in a
/// way no exit status shows. Only the case accounting finds it, which is why
/// the accounting exists.
#[test]
fn a_short_stream_with_a_clean_exit_is_a_protocol_error() {
    let dir = scratch("silent");
    let cases = make_cases(&dir, 4);

    let report =
        Run::execute(&config(&dir, cases, vec![faulty("silent", 3)])).expect("run completes");

    let worker = &report.workers[0];
    assert_eq!(worker.termination, Termination::Exited { code: 0 });
    assert_eq!(worker.cases_reported, 2);
    assert_eq!(worker.not_run.len(), 2);
    assert!(
        worker
            .protocol_errors
            .iter()
            .any(|e| e.contains("exited cleanly")),
        "got {:?}",
        worker.protocol_errors
    );
}

/// A missing binary is a broken run, not a finding about a consumer. If those
/// read alike, a typo in a path becomes a bug report against Arrow.
#[test]
fn a_worker_that_cannot_start_makes_the_run_incomplete() {
    let dir = scratch("nostart");
    let cases = make_cases(&dir, 2);
    let missing = WorkerSpec::new("ghost", dir.join("no-such-worker"));

    let report =
        Run::execute(&config(&dir, cases, vec![missing, null_worker()])).expect("run completes");

    assert!(matches!(
        report.workers[0].termination,
        Termination::SpawnFailed { .. }
    ));
    assert!(!report.workers[0].termination.is_abnormal(), "not a crash");
    assert!(!report.complete(), "nothing was measured for that worker");
    assert_eq!(
        report.workers[1].cases_reported, 2,
        "the run still measured the others"
    );
}

/// A case path too long for the worker's line buffer must stop the worker, not
/// be split into two paths.
///
/// A silently truncated line becomes a `case` string the coordinator never
/// assigned, while the real case is counted `not-run` -- a misattributed result
/// dressed as a measurement. Refusing the list is the correct failure: the
/// protocol gives exit 1 to a worker that cannot read its arguments.
///
/// This drives the worker directly. The coordinator writes the case list from
/// real paths and so cannot produce this input, which is exactly why the worker
/// has to defend against it on its own.
#[test]
fn an_over_long_case_list_line_is_refused_rather_than_split() {
    let dir = scratch("longline");
    let list = dir.join("cases.txt");
    // Comfortably past the worker's 4096-byte buffer.
    fs::write(&list, format!("{}.abicase\n", "x".repeat(5000))).expect("write case list");

    let output = Command::new(built("adapters", "abi-worker-null"))
        .arg("--cases")
        .arg(&list)
        .arg("--results")
        .arg(dir.join("results.jsonl"))
        .output()
        .expect("run the worker");

    assert_eq!(
        output.status.code(),
        Some(1),
        "worker-level failure is exit 1"
    );
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        stderr.contains("longer than"),
        "unhelpful diagnostic: {stderr}"
    );
    // Nothing was reported, rather than something wrong being reported.
    assert!(
        !dir.join("results.jsonl").exists()
            || fs::read_to_string(dir.join("results.jsonl"))
                .unwrap()
                .is_empty(),
        "a refused case list must produce no results"
    );
}

/// `sanitizers` is an array of one name per element, not one comma-joined
/// string. The build hands the worker "address,undefined" as a single define,
/// and emitting that verbatim gives `["address,undefined"]` -- which every
/// reader of a run report would then have to know to split again.
///
/// Checked through the shape the header always has: a sanitizer-free build
/// reports `[]`, and the element count matches the names, so a regression to
/// the joined form shows up as one element where there should be two.
#[test]
fn the_header_reports_sanitizers_as_separate_names() {
    let dir = scratch("header");
    let cases = make_cases(&dir, 1);

    let report = Run::execute(&config(&dir, cases, vec![null_worker()])).expect("run completes");
    let header = report.workers[0].header.as_ref().expect("header line");

    let sanitizers = header["sanitizers"]
        .as_array()
        .expect("sanitizers is an array");
    for entry in sanitizers {
        let name = entry.as_str().expect("each sanitizer is a string");
        assert!(
            !name.contains(','),
            "sanitizers must be one name per element, got {name:?}"
        );
    }
    // The rest of the header is what a report needs to identify the run at all.
    assert_eq!(header["worker_protocol"], 1);
    assert_eq!(header["consumer"], "null");
    assert!(header["arch"].is_string());
    assert!(header["os"].is_string());
    assert!(header["compiler"].is_string());
}

/// The null consumer imports and releases, and the observer has to see that the
/// release came from outside the producer's own callbacks. If this ever reports
/// otherwise, the workers are measuring nothing and every result above is
/// vacuous.
#[test]
fn the_null_consumer_releases_what_it_imports() {
    let dir = scratch("observer");
    let cases = make_cases(&dir, 2);

    let report = Run::execute(&config(&dir, cases, vec![null_worker()])).expect("run completes");
    let results = fs::read_to_string(&report.workers[0].results_path).expect("results");

    let mut seen = 0;
    for line in results.lines().skip(1).filter(|l| !l.is_empty()) {
        let value: serde_json::Value = serde_json::from_str(line).expect("result line");
        let observer = value.get("observer").expect("observer block");
        assert_eq!(observer["violations"], 0);
        assert_eq!(
            observer["outstanding"], 0,
            "the reconstruction's allocations balance"
        );
        assert_eq!(observer["released_by_consumer"], true);
        assert_eq!(
            observer["harness_released"], false,
            "nothing was left for us to clean up"
        );
        seen += 1;
    }
    assert_eq!(seen, 2);
}

/// A digest that cannot be computed is reported as such, with its reason, and
/// never as an absent field. The shared fixture carries a dictionary, which the
/// v0 digest refuses rather than guessing at (`docs/digest.md`), so the null
/// worker's `sent` half must come back as `sent_error` naming the refusal. And
/// the null consumer hands nothing back, so there is no `received` in either
/// form: a copy of `sent` would claim a round trip that never happened.
#[test]
fn an_undigestable_case_reports_why_and_the_null_consumer_claims_nothing_back() {
    let dir = scratch("digest");
    let cases = make_cases(&dir, 1);

    let report = Run::execute(&config(&dir, cases, vec![null_worker()])).expect("run completes");
    let results = fs::read_to_string(&report.workers[0].results_path).expect("results");
    let line = results.lines().nth(1).expect("one result line");
    let value: serde_json::Value = serde_json::from_str(line).expect("result line");

    assert_eq!(
        value["status"], "accepted",
        "a digest failure is not a case failure"
    );
    let digest = value.get("digest").expect("digest object");
    assert_eq!(digest["ver"], 1);
    assert!(
        digest.get("sent").is_none(),
        "no digest was computed: {digest}"
    );
    let reason = digest["sent_error"]
        .as_str()
        .expect("sent_error is a string");
    assert!(!reason.is_empty(), "the refusal carries its reason");
    assert!(
        digest.get("received").is_none(),
        "nothing came back: {digest}"
    );
    assert!(
        digest.get("received_error").is_none(),
        "nothing came back: {digest}"
    );
}
