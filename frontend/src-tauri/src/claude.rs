//! Claude Code, the agent that writes tutorials.
//!
//! Telescode never signs in to Anthropic itself: Claude Code owns the
//! sign-in, and the shell only asks it whether it is signed in and, when the
//! user asks for a tutorial, starts its browser sign-in. Nothing here runs
//! until then.

use std::ffi::OsString;
use serde_json::{json, Value};
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::time::Duration;

use crate::bridge::BridgeError;
use crate::agent::{command, run, newest_first, Status};
#[cfg(test)]
use crate::agent::version;

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

/// `loggedIn` from `claude auth status --json`; anything unreadable counts as signed out.
fn signed_in(json: &[u8]) -> bool {
    serde_json::from_slice::<serde_json::Value>(json).is_ok_and(|v| v["loggedIn"] == true)
}

pub fn status() -> Status {
    let Some(claude) = find() else { return Status { installed: false, signed_in: false } };
    let output = command(&claude).args(["auth", "status", "--json"]).stderr(Stdio::null()).output();
    Status { installed: true, signed_in: output.is_ok_and(|o| signed_in(&o.stdout)) }
}

pub fn sign_in() -> Result<(), BridgeError> {
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

#[cfg(test)]
mod tests {
    use super::*;

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

/// The `claude` arguments that attach only this server and allow only
/// `tools` from it (print mode denies everything else).
pub fn config_args(server: &crate::mcp::ServerCommand, tools: &[&str]) -> Vec<OsString> {
    let env: serde_json::Map<String, Value> = server
        .env
        .iter()
        .map(|(k, v)| (k.clone(), Value::from(v.to_string_lossy().into_owned())))
        .collect();
    let config = json!({
        "mcpServers": {
            "telescode": {
                "type": "stdio",
                "command": server.program.to_string_lossy(),
                "args": server.args.iter().map(|a| a.to_string_lossy()).collect::<Vec<_>>(),
                "env": env,
            }
        }
    });
    let allowed = tools.iter().map(|t| format!("mcp__telescode__{t}")).collect::<Vec<_>>().join(",");
    vec![
        "--mcp-config".into(),
        config.to_string().into(),
        "--strict-mcp-config".into(),
        "--allowedTools".into(),
        allowed.into(),
    ]
}
