//! Spawning a worker and finding out how it ended.

use std::ffi::OsString;
use std::fs::File;
use std::io;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use serde::Serialize;

/// A consumer to run, named for the report.
#[derive(Debug, Clone)]
pub struct WorkerSpec {
    /// Report name, and the stem of this worker's artifacts in the run directory.
    pub name: String,
    /// The program to run. It must speak `docs/worker-protocol.md`.
    pub program: PathBuf,
    /// Arguments before the protocol's own `--cases` / `--results`.
    pub args: Vec<OsString>,
    /// Environment for this worker alone, on top of the coordinator's own.
    ///
    /// Consumers are configured by environment far more than by argument --
    /// `ASAN_OPTIONS`, a library path, a thread count -- and setting those on
    /// the coordinator would apply them to every worker in the run, which
    /// would make one consumer's configuration another's.
    pub env: Vec<(OsString, OsString)>,
}

impl WorkerSpec {
    pub fn new(name: impl Into<String>, program: impl Into<PathBuf>) -> Self {
        Self {
            name: name.into(),
            program: program.into(),
            args: Vec::new(),
            env: Vec::new(),
        }
    }

    pub fn with_env(mut self, key: impl Into<OsString>, value: impl Into<OsString>) -> Self {
        self.env.push((key.into(), value.into()));
        self
    }
}

/// How a worker process ended.
///
/// A crash and a clean refusal have to be different things here. If they
/// collapse into "non-zero", a consumer that documents a limitation and exits
/// 1 becomes indistinguishable from one that faulted, and the compatibility
/// entry becomes a bug report.
#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Termination {
    /// Ran to completion with this exit code. Zero is the only clean one.
    Exited { code: i32 },
    /// Killed by a signal (POSIX). `11` is SIGSEGV.
    Signal { signal: i32 },
    /// Ended by a Windows exception. `0xC0000005` is an access violation --
    /// the same fault a SIGSEGV reports, under the other operating system's
    /// name for it.
    Exception { code: u32 },
    /// Still running when the wall clock ran out; the coordinator killed it.
    TimedOut { after_ms: u128 },
    /// Never started. A missing binary is a run configuration error, not a
    /// finding about a consumer, and the two must not read alike.
    SpawnFailed { error: String },
}

impl Termination {
    /// True when the process ended the way a healthy worker ends.
    pub fn is_clean(&self) -> bool {
        matches!(self, Termination::Exited { code: 0 })
    }

    /// True when the process was destroyed rather than returning.
    ///
    /// A non-zero exit is *not* abnormal in this sense: the protocol gives exit
    /// 1 to a worker that could not read its arguments, which is a bad
    /// invocation and not a crash.
    pub fn is_abnormal(&self) -> bool {
        matches!(
            self,
            Termination::Signal { .. }
                | Termination::Exception { .. }
                | Termination::TimedOut { .. }
        )
    }

    /// A short phrase for the human summary.
    pub fn summary(&self) -> String {
        match self {
            Termination::Exited { code: 0 } => "completed".to_string(),
            Termination::Exited { code } => format!("exit {code}"),
            Termination::Signal { signal } => format!("signal {signal}{}", signal_name(*signal)),
            Termination::Exception { code } => format!("exception 0x{code:08X}"),
            Termination::TimedOut { after_ms } => format!("timed out after {after_ms} ms"),
            Termination::SpawnFailed { error } => format!("could not start: {error}"),
        }
    }
}

fn signal_name(signal: i32) -> &'static str {
    match signal {
        4 => " (SIGILL)",
        6 => " (SIGABRT)",
        8 => " (SIGFPE)",
        9 => " (SIGKILL)",
        11 => " (SIGSEGV)",
        _ => "",
    }
}

/// What supervising one worker produced.
pub struct Supervised {
    pub termination: Termination,
    pub duration: Duration,
}

/// Runs one worker to completion, to its death, or to the timeout.
///
/// stdout and stderr go to files rather than pipes on purpose. A sanitizer
/// report can be large, and a parent that polls for exit while a child blocks
/// writing into a full pipe deadlocks -- turning the timeout path into the
/// normal path and every crash into a hang.
pub fn supervise(
    spec: &WorkerSpec,
    cases: &Path,
    results: &Path,
    stdout: &Path,
    stderr: &Path,
    timeout: Duration,
    working_dir: &Path,
) -> io::Result<Supervised> {
    let started = Instant::now();

    let mut command = Command::new(&spec.program);
    command
        .args(&spec.args)
        .envs(spec.env.iter().map(|(k, v)| (k, v)))
        .arg("--cases")
        .arg(cases)
        .arg("--results")
        .arg(results)
        .current_dir(working_dir)
        .stdin(Stdio::null())
        .stdout(Stdio::from(File::create(stdout)?))
        .stderr(Stdio::from(File::create(stderr)?));

    let mut child = match command.spawn() {
        Ok(child) => child,
        Err(e) => {
            return Ok(Supervised {
                termination: Termination::SpawnFailed {
                    error: e.to_string(),
                },
                duration: started.elapsed(),
            });
        }
    };

    // Polling rather than a waiting thread: the child has to stay reachable so
    // it can be killed on timeout, and moving it into a thread to call wait()
    // gives that up. 20 ms is far below any consumer's runtime and far above
    // the cost of the check.
    let status = loop {
        match child.try_wait()? {
            Some(status) => break Some(status),
            None if started.elapsed() >= timeout => {
                // Best effort: a process that is already gone gives an error
                // here, which is not a failure of the run.
                let _ = child.kill();
                let _ = child.wait();
                break None;
            }
            None => std::thread::sleep(Duration::from_millis(20)),
        }
    };

    let duration = started.elapsed();
    let termination = match status {
        None => Termination::TimedOut {
            after_ms: duration.as_millis(),
        },
        Some(status) => classify(&status),
    };
    Ok(Supervised {
        termination,
        duration,
    })
}

#[cfg(unix)]
fn classify(status: &std::process::ExitStatus) -> Termination {
    use std::os::unix::process::ExitStatusExt;
    if let Some(signal) = status.signal() {
        return Termination::Signal { signal };
    }
    Termination::Exited {
        code: status.code().unwrap_or(-1),
    }
}

#[cfg(windows)]
fn classify(status: &std::process::ExitStatus) -> Termination {
    // Windows has no signals. A fault surfaces as the exception code in the
    // exit status: 0xC0000005 for an access violation, which is the same event
    // POSIX reports as SIGSEGV.
    //
    // The range is the narrow one on purpose. Every fault code that actually
    // reaches a parent here -- access violation, illegal instruction
    // (0xC000001D), stack overflow (0xC00000FD), heap corruption (0xC0000374),
    // fail-fast (0xC0000409) -- lies in 0xC0000000..=0xC000FFFF. Testing the
    // whole NTSTATUS error space instead would catch `exit(-1)`, which arrives
    // as 0xFFFFFFFF, and turn a consumer that cleanly refuses into one that
    // crashed. Those two must not read alike.
    let code = status.code().unwrap_or(-1);
    let unsigned = code as u32;
    if (0xC000_0000..=0xC000_FFFF).contains(&unsigned) {
        return Termination::Exception { code: unsigned };
    }
    Termination::Exited { code }
}

#[cfg(not(any(unix, windows)))]
fn classify(status: &std::process::ExitStatus) -> Termination {
    Termination::Exited {
        code: status.code().unwrap_or(-1),
    }
}
