//! Codex CLI owns authentication; Telescode supplies its MCP server per run.
use crate::{agent, bridge::BridgeError, mcp::ServerCommand};
use std::ffi::OsString;
use std::path::PathBuf;
use std::process::Stdio;
use std::time::Duration;

const EXE: &str = if cfg!(windows) { "codex.exe" } else { "codex" };

pub fn find() -> Option<PathBuf> {
    if let Some(path) = std::env::var_os("TELESCODE_CODEX").filter(|p| !p.is_empty()) {
        return Some(path.into());
    }
    let names: &[&str] = if cfg!(windows) {
        &[EXE, "codex.cmd"]
    } else {
        &[EXE]
    };
    let mut paths: Vec<PathBuf> = std::env::var_os("PATH")
        .iter()
        .flat_map(std::env::split_paths)
        .flat_map(|dir| names.iter().map(move |name| dir.join(name)))
        .collect();
    if cfg!(windows) {
        if let Some(appdata) = std::env::var_os("APPDATA") {
            paths.push(PathBuf::from(appdata).join("npm/codex.cmd"));
        }
    }
    if let Some(home) = std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from)
    {
        for dir in [".local/bin", ".cargo/bin", ".npm-global/bin"] {
            paths.push(home.join(dir).join(EXE));
        }
        paths.extend(
            agent::newest_first(&home.join(".nvm/versions/node"), "v")
                .into_iter()
                .map(|dir| dir.join("bin").join(EXE)),
        );
        let platform = match (std::env::consts::OS, std::env::consts::ARCH) {
            ("macos", "aarch64") => "darwin-arm64",
            ("macos", _) => "darwin-x86_64",
            ("windows", "aarch64") => "windows-aarch64",
            ("windows", _) => "windows-x86_64",
            ("linux", "aarch64") => "linux-aarch64",
            _ => "linux-x86_64",
        };
        for editor in [".vscode", ".vscode-insiders", ".cursor", ".windsurf"] {
            paths.extend(
                agent::newest_first(&home.join(editor).join("extensions"), "openai.chatgpt-")
                    .into_iter()
                    .map(|dir| dir.join("bin").join(platform).join(EXE)),
            );
        }
    }
    paths.extend([
        PathBuf::from("/opt/homebrew/bin/codex"),
        PathBuf::from("/usr/local/bin/codex"),
    ]);
    paths.into_iter().find(|path| path.is_file())
}

pub fn not_installed() -> BridgeError {
    BridgeError::TourGenerationFailed {
        message:
            "Codex is not installed. Install Codex CLI, or set TELESCODE_CODEX to its executable."
                .into(),
    }
}

pub fn status() -> agent::Status {
    let Some(path) = find() else {
        return agent::Status {
            installed: false,
            signed_in: false,
        };
    };
    let output = agent::command(&path)
        .args(["login", "status"])
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status();
    agent::Status {
        installed: true,
        signed_in: output.is_ok_and(|status| status.success()),
    }
}

pub fn sign_in() -> Result<(), BridgeError> {
    let mut cmd = agent::command(&find().ok_or_else(not_installed)?);
    cmd.arg("login");
    agent::run(cmd, Duration::from_secs(10 * 60), "Signing in to Codex")?;
    if status().signed_in {
        Ok(())
    } else {
        Err(BridgeError::TourGenerationFailed {
            message: "Codex is still signed out.".into(),
        })
    }
}

pub fn generation_args(server: &ServerCommand, tools: &[&str]) -> Vec<OsString> {
    let quote = |value: &str| serde_json::to_string(value).expect("string serializes");
    let env = server
        .env
        .iter()
        .map(|(key, value)| format!("{}={}", quote(key), quote(&value.to_string_lossy())))
        .collect::<Vec<_>>()
        .join(",");
    let args = server
        .args
        .iter()
        .map(|arg| quote(&arg.to_string_lossy()))
        .collect::<Vec<_>>()
        .join(",");
    let tools = tools
        .iter()
        .map(|tool| quote(tool))
        .collect::<Vec<_>>()
        .join(",");
    let config = format!("mcp_servers.telescode={{command={},args=[{args}],env={{{env}}},enabled_tools=[{tools}],required=true}}", quote(&server.program.to_string_lossy()));
    let mut args: Vec<OsString> = [
        "exec",
        "--ignore-user-config",
        "--ephemeral",
        "--skip-git-repo-check",
        "--sandbox",
        "read-only",
    ]
    .into_iter()
    .map(Into::into)
    .collect();
    for setting in [
        "approval_policy=\"never\"",
        "web_search=\"disabled\"",
        "features.shell_tool=false",
        "features.apps=false",
        "features.view_image=false",
        "features.image_generation=false",
        "features.browser_use=false",
        "features.computer_use=false",
        "features.hooks=false",
        &config,
    ] {
        args.extend(["-c".into(), setting.into()]);
    }
    args
}
