//! Shared CLI lifecycle and the two supported agents.
use crate::{bridge::BridgeError, claude, codex, mcp};
use serde::{Deserialize, Serialize};
use std::ffi::OsString;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Provider {
    #[default]
    Claude,
    Codex,
}

#[derive(Debug, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct Status {
    pub installed: bool,
    pub signed_in: bool,
}

pub fn generate(
    provider: Provider,
    prompt: &str,
    tools: &[&str],
    timeout: Duration,
) -> Result<(), BridgeError> {
    let server = mcp::server_command()?;
    let (path, name) = match provider {
        Provider::Claude => (
            claude::find().ok_or_else(claude::not_installed)?,
            "Claude Code",
        ),
        Provider::Codex => (codex::find().ok_or_else(codex::not_installed)?, "Codex"),
    };
    let mut cmd = command(&path);
    cmd.args(generation_args(provider, prompt, &server, tools));
    run(cmd, timeout, name)
}

pub fn generation_args(
    provider: Provider,
    prompt: &str,
    server: &mcp::ServerCommand,
    tools: &[&str],
) -> Vec<OsString> {
    match provider {
        Provider::Claude => {
            let mut args = vec!["-p".into(), prompt.into()];
            args.extend(claude::config_args(server, tools));
            args
        }
        Provider::Codex => {
            let mut args = codex::generation_args(server, tools);
            args.push(prompt.into());
            args
        }
    }
}

#[tauri::command]
pub async fn agent_status(provider: Provider) -> Result<Status, BridgeError> {
    tauri::async_runtime::spawn_blocking(move || match provider {
        Provider::Claude => claude::status(),
        Provider::Codex => codex::status(),
    })
    .await
    .map_err(|e| BridgeError::Internal {
        message: format!("Agent worker failed: {e}"),
    })
}

#[tauri::command]
pub async fn agent_sign_in(provider: Provider) -> Result<(), BridgeError> {
    tauri::async_runtime::spawn_blocking(move || match provider {
        Provider::Claude => claude::sign_in(),
        Provider::Codex => codex::sign_in(),
    })
    .await
    .map_err(|e| BridgeError::Internal {
        message: format!("Agent worker failed: {e}"),
    })?
}

/// Subdirectories of `dir` named `<prefix><version>...`, highest version first.
pub(crate) fn newest_first(dir: &Path, prefix: &str) -> Vec<PathBuf> {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut found: Vec<(Vec<u64>, PathBuf)> = entries
        .flatten()
        .filter_map(|e| {
            let name = e.file_name().to_string_lossy().into_owned();
            let rest = name.strip_prefix(prefix)?;
            Some((version(rest), e.path()))
        })
        .collect();
    found.sort_by(|a, b| b.0.cmp(&a.0));
    found.into_iter().map(|(_, p)| p).collect()
}

/// The leading `1.2.3` of a name, as numbers, so 2.1.10 sorts above 2.1.9.
pub(crate) fn version(s: &str) -> Vec<u64> {
    s.split(|c: char| !c.is_ascii_digit() && c != '.')
        .next()
        .unwrap_or_default()
        .split('.')
        .map_while(|n| n.parse().ok())
        .collect()
}

pub fn command(executable: &Path) -> Command {
    let mut cmd = Command::new(executable);
    // Away from any project, so no project settings or files come into play.
    cmd.current_dir(std::env::temp_dir()).stdin(Stdio::null());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }
    cmd
}

/// Runs `cmd` to completion, stopping it after `timeout`.
pub fn run(mut cmd: Command, timeout: Duration, what: &str) -> Result<(), BridgeError> {
    let mut child = cmd
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| BridgeError::TourGenerationFailed {
            message: format!("Could not start {what}: {e}"),
        })?;
    // CLI errors may appear on stdout. Drain both pipes while it runs so
    // verbose output cannot fill a pipe and prevent the process from exiting.
    let stdout = capture_tail(child.stdout.take().expect("piped stdout"));
    let stderr = capture_tail(child.stderr.take().expect("piped stderr"));
    let started = Instant::now();
    let status = loop {
        if let Some(status) = child.try_wait().map_err(|e| BridgeError::Internal {
            message: format!("Lost track of {what}: {e}"),
        })? {
            break status;
        }
        if started.elapsed() > timeout {
            let _ = child.kill();
            let _ = child.wait();
            return Err(BridgeError::TourGenerationFailed {
                message: format!("{what} did not finish in time."),
            });
        }
        std::thread::sleep(Duration::from_millis(250));
    };
    let stdout = stdout.join().unwrap_or_default();
    let stderr = stderr.join().unwrap_or_default();
    if status.success() {
        return Ok(());
    }
    let details = [stderr, stdout]
        .into_iter()
        .filter(|s| !s.is_empty())
        .collect::<Vec<_>>()
        .join("\n");
    Err(BridgeError::TourGenerationFailed {
        message: if details.is_empty() {
            format!("{what} failed ({status}).")
        } else {
            format!("{what} failed ({status}): {details}")
        },
    })
}

/// Keep bounded error context while draining arbitrarily long process output.
fn capture_tail(mut reader: impl Read + Send + 'static) -> std::thread::JoinHandle<String> {
    std::thread::spawn(move || {
        const LIMIT: usize = 8192;
        let mut tail = Vec::new();
        let mut buffer = [0u8; LIMIT];
        loop {
            match reader.read(&mut buffer) {
                Ok(0) => break,
                Ok(n) => {
                    tail.extend_from_slice(&buffer[..n]);
                    if tail.len() > LIMIT {
                        tail.drain(..tail.len() - LIMIT);
                    }
                }
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                Err(_) => break,
            }
        }
        String::from_utf8_lossy(&tail).trim().to_owned()
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[cfg(unix)]
    fn shell(script: &str) -> Command {
        let mut cmd = Command::new("/bin/sh");
        cmd.args(["-c", script]);
        cmd
    }

    #[cfg(unix)]
    #[test]
    fn failure_reports_diagnostics_from_stdout() {
        let error = run(
            shell("printf 'Account quota reached'; exit 1"),
            Duration::from_secs(3),
            "Claude Code",
        )
        .unwrap_err();
        let BridgeError::TourGenerationFailed { message } = error else {
            panic!("unexpected error")
        };
        assert!(message.contains("Account quota reached"), "{message}");
        assert!(message.contains("exit status: 1"), "{message}");
    }

    #[cfg(unix)]
    #[test]
    fn failure_reports_both_output_streams() {
        let error = run(
            shell("printf 'Model request rejected'; printf 'Backend details' >&2; exit 1"),
            Duration::from_secs(3),
            "Claude Code",
        )
        .unwrap_err();
        let BridgeError::TourGenerationFailed { message } = error else {
            panic!("unexpected error")
        };
        assert!(message.contains("Model request rejected"), "{message}");
        assert!(message.contains("Backend details"), "{message}");
    }

    #[cfg(unix)]
    #[test]
    fn drains_large_output_without_deadlock_and_keeps_bounded_diagnostics() {
        let script = "i=0; while [ \"$i\" -lt 8192 ]; do printf 'stdout chunk %s\\n' \"$i\"; printf 'stderr chunk %s\\n' \"$i\" >&2; i=$((i+1)); done; printf 'final diagnostic' >&2; exit 1";
        let error = run(shell(script), Duration::from_secs(3), "Claude Code").unwrap_err();
        let BridgeError::TourGenerationFailed { message } = error else {
            panic!("unexpected error")
        };
        assert!(message.contains("final diagnostic"), "{message}");
        assert!(!message.contains("did not finish in time"), "{message}");
        assert!(
            message.len() < 20_000,
            "diagnostic length: {}",
            message.len()
        );
    }

    #[cfg(unix)]
    #[test]
    fn successful_command_with_output_still_succeeds() {
        assert!(run(
            shell("printf 'A saved tour'; printf 'Progress' >&2"),
            Duration::from_secs(3),
            "Claude Code"
        )
        .is_ok());
    }

    #[cfg(unix)]
    #[test]
    fn command_timeout_stops_the_child() {
        let started = Instant::now();
        let error = run(
            shell("exec sleep 10"),
            Duration::from_millis(50),
            "Claude Code",
        )
        .unwrap_err();
        let BridgeError::TourGenerationFailed { message } = error else {
            panic!("unexpected error")
        };
        assert!(message.contains("did not finish in time"), "{message}");
        assert!(started.elapsed() < Duration::from_secs(2));
    }
}
