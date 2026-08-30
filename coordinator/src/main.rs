//! `abi-coordinator` -- run a case set past several consumers, one process each.
//!
//! Exits 0 whenever the *run* completed, including when a consumer crashed:
//! that is the result, not a failure. It exits non-zero only when a worker
//! could not be started, because then nothing was measured.

use std::env;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::Duration;

use abi_coordinator::{Run, RunConfig, WorkerSpec};

const USAGE: &str = "\
usage: abi-coordinator --worker <name>=<program> [--worker ...]
                       --cases <dir|list-file> [--limit <n>]
                       --out <run-dir> [--timeout-secs <n>]

  --worker   a consumer to run, in its own process. Repeatable.
  --cases    a directory of .abicase files, or a text file listing them
             one per line (`#` comments allowed).
  --limit    use only the first n cases, in sorted order.
  --out      run directory: case list, result streams, captured output, run.json
  --timeout-secs  wall clock per worker (default 300)

see docs/worker-protocol.md
";

fn main() -> ExitCode {
    match real_main() {
        Ok(code) => code,
        Err(message) => {
            eprintln!("error: {message}");
            ExitCode::from(2)
        }
    }
}

fn real_main() -> Result<ExitCode, String> {
    let args: Vec<String> = env::args().skip(1).collect();
    if args.is_empty() {
        print!("{USAGE}");
        return Ok(ExitCode::from(2));
    }

    let mut workers: Vec<WorkerSpec> = Vec::new();
    let mut cases_arg: Option<PathBuf> = None;
    let mut out: Option<PathBuf> = None;
    let mut limit: Option<usize> = None;
    let mut timeout = Duration::from_secs(300);

    let mut i = 0;
    while i < args.len() {
        let flag = args[i].as_str();
        let mut value = || -> Result<String, String> {
            i += 1;
            args.get(i)
                .cloned()
                .ok_or_else(|| format!("{flag} needs a value"))
        };
        match flag {
            "--worker" => {
                let spec = value()?;
                let (name, program) = spec
                    .split_once('=')
                    .ok_or_else(|| format!("--worker wants <name>=<program>, got {spec:?}"))?;
                workers.push(WorkerSpec::new(name, program));
            }
            "--cases" => cases_arg = Some(PathBuf::from(value()?)),
            "--out" => out = Some(PathBuf::from(value()?)),
            "--limit" => {
                limit = Some(
                    value()?
                        .parse::<usize>()
                        .map_err(|e| format!("--limit: {e}"))?,
                )
            }
            "--timeout-secs" => {
                let secs = value()?
                    .parse::<u64>()
                    .map_err(|e| format!("--timeout-secs: {e}"))?;
                timeout = Duration::from_secs(secs);
            }
            "-h" | "--help" => {
                print!("{USAGE}");
                return Ok(ExitCode::SUCCESS);
            }
            other => return Err(format!("unknown argument {other:?}\n\n{USAGE}")),
        }
        i += 1;
    }

    let cases_arg = cases_arg.ok_or("--cases is required")?;
    let out = out.ok_or("--out is required")?;
    if workers.is_empty() {
        return Err("at least one --worker is required".to_string());
    }

    let mut cases = load_cases(&cases_arg)?;
    if let Some(limit) = limit {
        cases.truncate(limit);
    }
    if cases.is_empty() {
        return Err(format!("no cases found at {}", cases_arg.display()));
    }

    let working_dir = env::current_dir().map_err(|e| e.to_string())?;
    let config = RunConfig {
        cases,
        workers,
        run_dir: out,
        timeout,
        working_dir,
    };

    let report = Run::execute(&config).map_err(|e| e.to_string())?;

    println!(
        "{} case(s) x {} worker(s) -> {}",
        report.cases_assigned,
        report.workers.len(),
        report.run_dir.display()
    );
    for worker in &report.workers {
        println!("{}", worker.summary_line());
    }

    // A crash gets its own line. Buried in a table it is the easiest outcome to
    // miss, and it is the one the project exists to find.
    let crashed: Vec<_> = report.crashed().collect();
    if !crashed.is_empty() {
        println!("\nconsumer crashes (recorded, not fatal to the run):");
        for worker in crashed {
            println!(
                "  {}: {} | suspect: {}",
                worker.name,
                worker.termination.summary(),
                worker
                    .suspect
                    .as_deref()
                    .unwrap_or("none -- every case reported")
            );
        }
    }

    if report.complete() {
        Ok(ExitCode::SUCCESS)
    } else {
        eprintln!(
            "\nrun incomplete: a worker could not be started, so nothing was measured for it"
        );
        Ok(ExitCode::FAILURE)
    }
}

/// A directory of `.abicase` files, or a text file listing paths.
fn load_cases(path: &Path) -> Result<Vec<PathBuf>, String> {
    if path.is_dir() {
        let mut found: Vec<PathBuf> = std::fs::read_dir(path)
            .map_err(|e| format!("{}: {e}", path.display()))?
            .filter_map(Result::ok)
            .map(|entry| entry.path())
            .filter(|p| p.extension().is_some_and(|e| e == "abicase"))
            .collect();
        // Sorted, so --limit takes the same subset on every host and two runs
        // of the same command are comparable.
        found.sort();
        return Ok(found);
    }

    let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(text
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
        .map(PathBuf::from)
        .collect())
}
