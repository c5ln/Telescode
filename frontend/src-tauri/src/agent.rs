//! Shared CLI lifecycle and the two supported agents.
use crate::{bridge::BridgeError, claude, codex, mcp};
use serde::{Deserialize, Serialize};
use std::ffi::OsString;
use std::io::Read;
use std::sync::Arc;
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

/// One Telescode tool the agent called, read from its event stream: what the
/// app shows as the agent's progress. Nothing is asked of the agent for it.
#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ToolCall {
    /// The tool's name without the `mcp__telescode__` prefix, e.g. `inspect_node`.
    pub tool: String,
    /// The node it was about, when it names one.
    pub node_id: Option<String>,
}

/// Where tool calls go as they happen.
pub type OnToolCall = Arc<dyn Fn(ToolCall) + Send + Sync>;

/// Sends each tool call to the app as an `agent-progress` event tagged with
/// `progress_id`, which the caller chose so it can tell its own run apart.
/// Without an ID nothing is followed and the agent runs as before.
pub fn emit_progress<R: tauri::Runtime>(app: &tauri::AppHandle<R>, progress_id: Option<String>) -> Option<OnToolCall> {
    use tauri::Emitter;
    let id = progress_id?;
    let app = app.clone();
    Some(Arc::new(move |call: ToolCall| {
        let _ = app.emit("agent-progress", serde_json::json!({ "progressId": id, "tool": call.tool, "nodeId": call.node_id }));
    }))
}

pub fn generate(
    provider: Provider,
    prompt: &str,
    tools: &[&str],
    timeout: Duration,
    on_tool_call: Option<OnToolCall>,
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
    let mut args = generation_args(provider, prompt, &server, tools);
    if on_tool_call.is_some() {
        // Print each event as a JSON line, so tool calls can be followed as they happen.
        match provider {
            Provider::Claude => args.extend(["--output-format".into(), "stream-json".into(), "--verbose".into()]),
            Provider::Codex => args.insert(1, "--json".into()),
        }
    }
    cmd.args(args);
    let on_line = on_tool_call.map(|report| -> OnLine {
        Arc::new(move |line: &str| {
            for call in tool_calls(line) {
                report(call)
            }
        })
    });
    run_streaming(cmd, timeout, name, on_line)
}

/// The Telescode tool calls in one line of an agent's JSON event stream:
/// Claude Code's `tool_use` blocks (`mcp__telescode__<tool>`) and Codex's
/// `mcp_tool_call` items (`server: "telescode"`). Other lines yield nothing.
pub fn tool_calls(line: &str) -> Vec<ToolCall> {
    fn visit(value: &serde_json::Value, out: &mut Vec<ToolCall>) {
        match value {
            serde_json::Value::Object(map) => {
                let claude = map.get("name").and_then(|n| n.as_str()).and_then(|n| n.strip_prefix("mcp__telescode__"));
                let codex = map
                    .get("tool")
                    .and_then(|t| t.as_str())
                    .filter(|_| map.get("server").and_then(|s| s.as_str()) == Some("telescode"));
                if let Some(tool) = claude.or(codex) {
                    let input = map.get("input").or_else(|| map.get("arguments"));
                    // Codex may send the arguments as a JSON string.
                    let parsed = input.and_then(|i| i.as_str()).and_then(|s| serde_json::from_str::<serde_json::Value>(s).ok());
                    let node_id = parsed
                        .as_ref()
                        .or(input)
                        .and_then(|i| i.get("nodeId"))
                        .and_then(|n| n.as_str())
                        .map(str::to_owned);
                    let call = ToolCall { tool: tool.to_owned(), node_id };
                    // Codex reports a call when it starts and again when it ends.
                    if out.last() != Some(&call) {
                        out.push(call);
                    }
                    return;
                }
                for v in map.values() {
                    visit(v, out)
                }
            }
            serde_json::Value::Array(items) => items.iter().for_each(|v| visit(v, out)),
            _ => {}
        }
    }
    let mut out = Vec::new();
    if let Ok(value) = serde_json::from_str::<serde_json::Value>(line.trim()) {
        visit(&value, &mut out)
    }
    out
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

/// Called with each line the process prints on stdout.
pub type OnLine = Arc<dyn Fn(&str) + Send + Sync>;

/// Runs `cmd` to completion, stopping it after `timeout`.
pub fn run(cmd: Command, timeout: Duration, what: &str) -> Result<(), BridgeError> {
    run_streaming(cmd, timeout, what, None)
}

/// `run`, handing each stdout line to `on_line` as it is printed.
pub fn run_streaming(mut cmd: Command, timeout: Duration, what: &str, on_line: Option<OnLine>) -> Result<(), BridgeError> {
    let mut child = cmd
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| BridgeError::TourGenerationFailed {
            message: format!("Could not start {what}: {e}"),
        })?;
    // CLI errors may appear on stdout. Drain both pipes while it runs so
    // verbose output cannot fill a pipe and prevent the process from exiting.
    let stdout = capture_tail(child.stdout.take().expect("piped stdout"), on_line);
    let stderr = capture_tail(child.stderr.take().expect("piped stderr"), None);
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
/// With `on_line`, also hands over each complete line as it arrives.
fn capture_tail(mut reader: impl Read + Send + 'static, on_line: Option<OnLine>) -> std::thread::JoinHandle<String> {
    std::thread::spawn(move || {
        const LIMIT: usize = 8192;
        let mut tail = Vec::new();
        let mut buffer = [0u8; LIMIT];
        let mut line = Vec::new();
        loop {
            match reader.read(&mut buffer) {
                Ok(0) => break,
                Ok(n) => {
                    if let Some(on_line) = &on_line {
                        for &byte in &buffer[..n] {
                            if byte == b'\n' {
                                on_line(&String::from_utf8_lossy(&line));
                                line.clear();
                            } else {
                                line.push(byte);
                            }
                        }
                    }
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

    #[test]
    fn tool_calls_are_read_from_claude_code_events() {
        let line = r#"{"type":"assistant","message":{"content":[{"type":"text","text":"Reading."},{"type":"tool_use","id":"t1","name":"mcp__telescode__inspect_node","input":{"projectId":"p","nodeId":"file:src/requests/sessions.py"}}]}}"#;
        assert_eq!(tool_calls(line), vec![ToolCall { tool: "inspect_node".into(), node_id: Some("file:src/requests/sessions.py".into()) }]);
        // Results, text and other servers are not tool calls.
        assert!(tool_calls(r#"{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"..."}]}}"#).is_empty());
        assert!(tool_calls(r#"{"type":"assistant","message":{"content":[{"type":"tool_use","name":"Bash","input":{}}]}}"#).is_empty());
        assert!(tool_calls("not json").is_empty());
    }

    #[test]
    fn tool_calls_are_read_from_codex_events() {
        let started = r#"{"type":"item.started","item":{"type":"mcp_tool_call","server":"telescode","tool":"get_reading_tour_order","arguments":"{\"projectId\":\"p\"}"}}"#;
        assert_eq!(tool_calls(started), vec![ToolCall { tool: "get_reading_tour_order".into(), node_id: None }]);
        let with_node = r#"{"type":"item.completed","item":{"type":"mcp_tool_call","server":"telescode","tool":"inspect_node","arguments":{"nodeId":"class:a.py::A"}}}"#;
        assert_eq!(tool_calls(with_node)[0].node_id.as_deref(), Some("class:a.py::A"));
        assert!(tool_calls(r#"{"item":{"type":"mcp_tool_call","server":"other","tool":"inspect_node"}}"#).is_empty());
    }

    #[cfg(unix)]
    #[test]
    fn stdout_lines_are_handed_over_as_they_are_printed() {
        let lines = Arc::new(std::sync::Mutex::new(Vec::new()));
        let seen = lines.clone();
        let on_line: OnLine = Arc::new(move |line: &str| seen.lock().unwrap().push(line.to_owned()));
        run_streaming(shell("printf 'one\\ntwo\\n'"), Duration::from_secs(3), "test", Some(on_line)).unwrap();
        assert_eq!(*lines.lock().unwrap(), vec!["one", "two"]);
    }
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
