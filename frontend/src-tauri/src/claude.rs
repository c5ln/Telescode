//! Claude Code, the agent that writes tutorials.
//!
//! Telescode never signs in to Anthropic itself: Claude Code owns the
//! sign-in, and the shell only asks it whether it is signed in and, when the
//! user asks for a tutorial, starts its browser sign-in. Nothing here runs
//! until then.

use serde::Serialize;
use std::ffi::OsString;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use crate::bridge::BridgeError;

/// The Claude Code executable, when the one found automatically is not the one to use.
pub const CLAUDE_ENV: &str = "TELESCODE_CLAUDE";

/// Longest the browser sign-in may take.
const SIGN_IN_TIMEOUT: Duration = Duration::from_secs(10 * 60);

const EXE: &str = if cfg!(windows) { "claude.exe" } else { "claude" };

/// Where Claude Code is: `$TELESCODE_CLAUDE`, else the first `claude` on
/// PATH, else the places its installers and editor extensions put it. Desktop
/// apps started from the Finder do not see the shell's PATH, so the usual
/// locations are searched too.
pub fn find() -> Option<PathBuf> {
    if let Some(p) = std::env::var_os(CLAUDE_ENV).filter(|p| !p.is_empty()) {
        return Some(PathBuf::from(p));
    }
    let home = std::env::var_os("HOME").or_else(|| std::env::var_os("USERPROFILE")).map(PathBuf::from);
    candidates(std::env::var_os("PATH"), home.as_deref()).into_iter().find(|p| p.is_file())
}

/// Every place `find` looks, in order. Separate from the file checks so the
/// order can be tested.
fn candidates(path: Option<OsString>, home: Option<&Path>) -> Vec<PathBuf> {
    let mut out: Vec<PathBuf> = path.iter().flat_map(std::env::split_paths).map(|dir| dir.join(EXE)).collect();
    if let Some(home) = home {
        // The native installer, the older local install, npm and Homebrew.
        out.push(home.join(".local/bin").join(EXE));
        out.push(home.join(".claude/local").join(EXE));
        out.push(home.join(".npm-global/bin").join(EXE));
        out.extend(newest_first(&home.join(".nvm/versions/node"), "v").into_iter().map(|d| d.join("bin").join(EXE)));
        // The binary bundled with the editor extensions, newest version first.
        for editor in [".vscode", ".vscode-insiders", ".cursor", ".windsurf"] {
            let extensions = home.join(editor).join("extensions");
            out.extend(
                newest_first(&extensions, "anthropic.claude-code-")
                    .into_iter()
                    .map(|d| d.join("resources/native-binary").join(EXE)),
            );
        }
    }
    if !cfg!(windows) {
        out.push(PathBuf::from("/opt/homebrew/bin/claude"));
        out.push(PathBuf::from("/usr/local/bin/claude"));
    }
    out
}

/// Subdirectories of `dir` named `<prefix><version>...`, highest version first.
fn newest_first(dir: &Path, prefix: &str) -> Vec<PathBuf> {
    let Ok(entries) = std::fs::read_dir(dir) else { return Vec::new() };
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
fn version(s: &str) -> Vec<u64> {
    s.split(|c: char| !c.is_ascii_digit() && c != '.')
        .next()
        .unwrap_or_default()
        .split('.')
        .map_while(|n| n.parse().ok())
        .collect()
}

pub fn command(claude: &Path) -> Command {
    let mut cmd = Command::new(claude);
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
    let mut child = cmd.stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().map_err(|e| {
        BridgeError::TourGenerationFailed { message: format!("Could not start Claude Code: {e}") }
    })?;
    // Claude reports API failures on stdout. Drain both pipes while it runs so
    // verbose output cannot fill a pipe and prevent the process from exiting.
    let stdout = capture_tail(child.stdout.take().expect("piped stdout"));
    let stderr = capture_tail(child.stderr.take().expect("piped stderr"));
    let started = Instant::now();
    let status = loop {
        if let Some(status) = child.try_wait().map_err(|e| BridgeError::Internal {
            message: format!("Lost track of Claude Code: {e}"),
        })? {
            break status;
        }
        if started.elapsed() > timeout {
            let _ = child.kill();
            let _ = child.wait();
            return Err(BridgeError::TourGenerationFailed { message: format!("{what} did not finish in time.") });
        }
        std::thread::sleep(Duration::from_millis(250));
    };
    let stdout = stdout.join().unwrap_or_default();
    let stderr = stderr.join().unwrap_or_default();
    if status.success() {
        return Ok(());
    }
    let details = [stderr, stdout].into_iter().filter(|s| !s.is_empty()).collect::<Vec<_>>().join("\n");
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

/// Whether Claude Code can write a tutorial right now.
#[derive(Debug, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct Status {
    installed: bool,
    signed_in: bool,
}

/// `loggedIn` from `claude auth status --json`; anything unreadable counts as signed out.
fn signed_in(json: &[u8]) -> bool {
    serde_json::from_slice::<serde_json::Value>(json).is_ok_and(|v| v["loggedIn"] == true)
}

fn status() -> Status {
    let Some(claude) = find() else { return Status { installed: false, signed_in: false } };
    let output = command(&claude).args(["auth", "status", "--json"]).stderr(Stdio::null()).output();
    Status { installed: true, signed_in: output.is_ok_and(|o| signed_in(&o.stdout)) }
}

fn sign_in() -> Result<(), BridgeError> {
    let claude = find().ok_or_else(not_installed)?;
    let mut cmd = command(&claude);
    // A Claude subscription, not API billing: the same sign-in as `claude` itself.
    cmd.args(["auth", "login", "--claudeai"]);
    run(cmd, SIGN_IN_TIMEOUT, "Signing in to Claude")?;
    if status().signed_in {
        Ok(())
    } else {
        Err(BridgeError::TourGenerationFailed { message: "Claude Code is still signed out.".into() })
    }
}

pub fn not_installed() -> BridgeError {
    BridgeError::TourGenerationFailed {
        message: format!("Claude Code is not installed. Install it, or set {CLAUDE_ENV} to the claude executable."),
    }
}

/// IPC entry point: `invoke("claude_status")`. Called only when a tutorial
/// is asked for and none exists.
#[tauri::command]
pub async fn claude_status() -> Result<Status, BridgeError> {
    tauri::async_runtime::spawn_blocking(status).await.map_err(|e| BridgeError::Internal {
        message: format!("Claude Code worker failed: {e}"),
    })
}

/// IPC entry point: `invoke("claude_sign_in")`. Opens Claude Code's sign-in
/// in the browser and resolves once the user is signed in.
#[tauri::command]
pub async fn claude_sign_in() -> Result<(), BridgeError> {
    tauri::async_runtime::spawn_blocking(sign_in).await.map_err(|e| BridgeError::Internal {
        message: format!("Claude Code worker failed: {e}"),
    })?
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
        let error = run(shell("printf 'Account quota reached'; exit 1"), Duration::from_secs(3), "Claude Code").unwrap_err();
        let BridgeError::TourGenerationFailed { message } = error else { panic!("unexpected error") };
        assert!(message.contains("Account quota reached"), "{message}");
        assert!(message.contains("exit status: 1"), "{message}");
    }

    #[cfg(unix)]
    #[test]
    fn failure_reports_both_output_streams() {
        let error = run(shell("printf 'Model request rejected'; printf 'Backend details' >&2; exit 1"), Duration::from_secs(3), "Claude Code").unwrap_err();
        let BridgeError::TourGenerationFailed { message } = error else { panic!("unexpected error") };
        assert!(message.contains("Model request rejected"), "{message}");
        assert!(message.contains("Backend details"), "{message}");
    }

    #[cfg(unix)]
    #[test]
    fn drains_large_output_without_deadlock_and_keeps_bounded_diagnostics() {
        let script = "i=0; while [ \"$i\" -lt 8192 ]; do printf 'stdout chunk %s\\n' \"$i\"; printf 'stderr chunk %s\\n' \"$i\" >&2; i=$((i+1)); done; printf 'final diagnostic' >&2; exit 1";
        let error = run(shell(script), Duration::from_secs(3), "Claude Code").unwrap_err();
        let BridgeError::TourGenerationFailed { message } = error else { panic!("unexpected error") };
        assert!(message.contains("final diagnostic"), "{message}");
        assert!(!message.contains("did not finish in time"), "{message}");
        assert!(message.len() < 20_000, "diagnostic length: {}", message.len());
    }

    #[cfg(unix)]
    #[test]
    fn successful_command_with_output_still_succeeds() {
        assert!(run(shell("printf 'A saved tour'; printf 'Progress' >&2"), Duration::from_secs(3), "Claude Code").is_ok());
    }

    #[cfg(unix)]
    #[test]
    fn command_timeout_stops_the_child() {
        let started = Instant::now();
        let error = run(shell("exec sleep 10"), Duration::from_millis(50), "Claude Code").unwrap_err();
        let BridgeError::TourGenerationFailed { message } = error else { panic!("unexpected error") };
        assert!(message.contains("did not finish in time"), "{message}");
        assert!(started.elapsed() < Duration::from_secs(2));
    }

    #[test]
    fn looks_on_path_first_then_installers_then_the_newest_extension() {
        let home = std::env::temp_dir().join(format!("telescode-claude-home-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&home);
        for v in ["2.1.9-darwin-arm64", "2.1.10-darwin-arm64"] {
            std::fs::create_dir_all(home.join(".vscode/extensions").join(format!("anthropic.claude-code-{v}"))).unwrap();
        }
        std::fs::create_dir_all(home.join(".vscode/extensions/ms-python.python-1.0.0")).unwrap();

        let path = std::env::join_paths(["/a", "/b"]).unwrap();
        let found = candidates(Some(path), Some(&home));
        assert_eq!(found[0], Path::new("/a").join(EXE));
        assert_eq!(found[1], Path::new("/b").join(EXE));
        assert_eq!(found[2], home.join(".local/bin").join(EXE));
        let extensions: Vec<_> = found.iter().filter(|p| p.to_string_lossy().contains("claude-code-")).collect();
        assert_eq!(extensions.len(), 2);
        assert!(extensions[0].to_string_lossy().contains("2.1.10"));
    }

    #[test]
    fn versions_compare_by_number() {
        assert!(version("2.1.10-darwin-arm64") > version("2.1.9-darwin-arm64"));
        assert_eq!(version("v22.4.1"), Vec::<u64>::new()); // the prefix is stripped before this
        assert_eq!(version("22.4.1"), vec![22, 4, 1]);
    }

    #[test]
    fn reads_signed_in_from_auth_status() {
        assert!(signed_in(br#"{"loggedIn": true, "authMethod": "claude.ai"}"#));
        assert!(!signed_in(br#"{"loggedIn": false, "authMethod": "none"}"#));
        assert!(!signed_in(b"not json"));
    }

    // Checks the real Claude Code on this machine when TELESCODE_TEST_CLAUDE=1:
    // it is found without help, and its sign-in state is read. With
    // CLAUDE_CONFIG_DIR pointing at an empty folder it must read as signed out.
    #[test]
    fn real_status() {
        if std::env::var_os("TELESCODE_TEST_CLAUDE").is_none() {
            return;
        }
        let status = status();
        assert!(status.installed, "Claude Code was not found");
        let expect_signed_out = std::env::var_os("CLAUDE_CONFIG_DIR").is_some();
        assert_eq!(status.signed_in, !expect_signed_out);
    }
}
