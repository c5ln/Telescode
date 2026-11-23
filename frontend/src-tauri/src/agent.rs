//! Shared CLI lifecycle and the two supported agents.
use crate::{bridge::BridgeError, claude, codex, mcp};
use crate::metrics::RunMetrics;
use serde::{Deserialize, Serialize};
use std::ffi::OsString;
use std::io::Read;
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Deserialize, Serialize)]
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

/// A run the app follows: its tool calls go to the app, and the app can
/// cancel it by its progress ID (`agent_cancel`) while it is registered.
pub struct Followed {
    id: String,
    on_tool_call: OnToolCall,
    cancelled: Arc<AtomicBool>,
}

impl Drop for Followed {
    fn drop(&mut self) {
        followed().lock().unwrap_or_else(|e| e.into_inner()).remove(&self.id);
    }
}

/// The cancel flags of the runs being followed, by progress ID.
fn followed() -> &'static Mutex<HashMap<String, Arc<AtomicBool>>> {
    static RUNS: OnceLock<Mutex<HashMap<String, Arc<AtomicBool>>>> = OnceLock::new();
    RUNS.get_or_init(Default::default)
}

/// Registers the run `id`, sending its tool calls to `on_tool_call`.
pub fn follow(id: String, on_tool_call: OnToolCall) -> Followed {
    let cancelled = Arc::new(AtomicBool::new(false));
    followed().lock().unwrap_or_else(|e| e.into_inner()).insert(id.clone(), cancelled.clone());
    Followed { id, on_tool_call, cancelled }
}

/// Stops the followed run `id`; a run that already ended is left alone.
pub fn cancel(id: &str) {
    if let Some(flag) = followed().lock().unwrap_or_else(|e| e.into_inner()).get(id) {
        flag.store(true, Ordering::SeqCst);
    }
}

/// Sends each tool call to the app as an `agent-progress` event tagged with
/// `progress_id`, which the caller chose so it can tell its own run apart and
/// cancel it. Without an ID nothing is followed and the agent runs as before.
pub fn emit_progress<R: tauri::Runtime>(app: &tauri::AppHandle<R>, progress_id: Option<String>) -> Option<Followed> {
    use tauri::Emitter;
    let id = progress_id?;
    let app = app.clone();
    let tag = id.clone();
    Some(follow(id, Arc::new(move |call: ToolCall| {
        let _ = app.emit("agent-progress", serde_json::json!({ "progressId": tag, "tool": call.tool, "nodeId": call.node_id }));
    })))
}

/// IPC entry point: `invoke("agent_cancel", { progressId })`. Stops the
/// tour, quiz or learning path being written under that progress ID; the
/// run then fails with `agent_cancelled`.
#[tauri::command]
pub fn agent_cancel(progress_id: String) {
    cancel(&progress_id)
}

/// Which model writes: the agent's own default, or a lighter, faster one for
/// bounded, server-validated writing: a step's two quiz questions and the
/// learning path's material.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Model {
    Default,
    Light,
}

/// Claude Code's light model, by its alias so it follows Claude Code's updates.
const CLAUDE_LIGHT_MODEL: &str = "sonnet";
/// Codex's light model and the reasoning effort it runs at.
const CODEX_LIGHT_MODEL: &str = "gpt-6-luna";
const CODEX_LIGHT_EFFORT: &str = "medium";

/// The arguments that pick `model` for `provider`; none for the default.
pub fn model_args(provider: Provider, model: Model) -> Vec<OsString> {
    match (provider, model) {
        (_, Model::Default) => Vec::new(),
        (Provider::Claude, Model::Light) => vec!["--model".into(), CLAUDE_LIGHT_MODEL.into()],
        (Provider::Codex, Model::Light) => vec![
            "-m".into(),
            CODEX_LIGHT_MODEL.into(),
            "-c".into(),
            format!("model_reasoning_effort=\"{CODEX_LIGHT_EFFORT}\"").into(),
        ],
    }
}

/// What one agent run used, read from its JSON event stream. Input read from
/// the prompt cache is counted apart from new input, the same way for both agents.
#[derive(Clone, Debug, Default, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Usage {
    pub input_tokens: u64,
    pub cached_input_tokens: u64,
    pub output_tokens: u64,
    /// What Claude Code reports the run cost; Codex reports none.
    pub cost_usd: Option<f64>,
}

impl Usage {
    /// Adds what one event line reports: Claude Code's closing `result`, or a
    /// Codex `turn.completed`. Other lines change nothing.
    pub fn read(&mut self, line: &str) {
        let Ok(event) = serde_json::from_str::<serde_json::Value>(line.trim()) else { return };
        let count = |usage: &serde_json::Value, key: &str| usage.get(key).and_then(|v| v.as_u64()).unwrap_or(0);
        match event.get("type").and_then(|t| t.as_str()) {
            Some("result") => {
                let Some(usage) = event.get("usage") else { return };
                self.input_tokens += count(usage, "input_tokens") + count(usage, "cache_creation_input_tokens");
                self.cached_input_tokens += count(usage, "cache_read_input_tokens");
                self.output_tokens += count(usage, "output_tokens");
                if let Some(cost) = event.get("total_cost_usd").and_then(|c| c.as_f64()) {
                    self.cost_usd = Some(self.cost_usd.unwrap_or(0.0) + cost);
                }
            }
            Some("turn.completed") => {
                let Some(usage) = event.get("usage") else { return };
                let cached = count(usage, "cached_input_tokens");
                self.input_tokens += count(usage, "input_tokens").saturating_sub(cached);
                self.cached_input_tokens += cached;
                self.output_tokens += count(usage, "output_tokens");
            }
            _ => {}
        }
    }
}

/// What a run writes, as the usage log names it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum Work {
    Tour,
    Quiz,
    /// A learning session's first preparation.
    Learning,
    /// The questions written while the learner answers.
    LearningMaterial,
}

/// What a run was for, so its usage log line can be matched with the tour
/// step, learning session or job it wrote for. IDs only, never paths.
#[derive(Clone, Debug, Default, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Correlation {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub session_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub job_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tour_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub stop_index: Option<u32>,
}

/// One finished run, as the usage log keeps it.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct UsageRecord {
    pub at_ms: u128,
    /// Tells this run apart from every other, also across app restarts.
    pub run_id: String,
    pub work: Work,
    pub provider: Provider,
    pub model: Model,
    pub duration_ms: u128,
    pub ok: bool,
    /// The failure's `BridgeError` code; never its message, which may hold
    /// CLI output and paths.
    pub error_code: Option<String>,
    /// The measuring scenario this run belongs to (`TELESCODE_RUN_SCENARIO`).
    pub scenario: Option<String>,
    pub correlation: Correlation,
    pub usage: Usage,
    pub metrics: RunMetrics,
}

/// Names the measuring scenario runs belong to, e.g. `baseline-quiz-cold`,
/// so before-and-after runs can be compared in the usage log.
pub const SCENARIO_ENV: &str = "TELESCODE_RUN_SCENARIO";

/// `raw` as a scenario name: letters, digits, `.`, `_` and `-`, at most 64.
/// Anything else is dropped rather than cleaned, so no path gets into the log.
fn scenario(raw: Option<String>) -> Option<String> {
    let name = raw?.trim().to_owned();
    let valid = !name.is_empty() && name.len() <= 64 && name.chars().all(|c| c.is_ascii_alphanumeric() || "._-".contains(c));
    valid.then_some(name)
}

/// A new run ID: start time, process and a counter, so IDs never repeat.
fn new_run_id() -> String {
    static NEXT: AtomicU64 = AtomicU64::new(0);
    format!("{:x}-{:x}-{}", now_ms(), std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed))
}

fn now_ms() -> u128 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis()).unwrap_or(0)
}

/// The usage log, one JSON line per run, next to the server's saved tours and quizzes.
pub const USAGE_LOG: &str = "agent-usage.jsonl";

/// Appends `record` to the usage log in `dir`.
pub fn record_usage(dir: &Path, record: &UsageRecord) -> std::io::Result<()> {
    use std::io::Write;
    std::fs::create_dir_all(dir)?;
    let mut file = std::fs::OpenOptions::new().create(true).append(true).open(dir.join(USAGE_LOG))?;
    writeln!(file, "{}", serde_json::to_string(record).map_err(std::io::Error::other)?)
}

/// Where the server keeps its output: TELESCODE_TOUR_OUTPUT, or mcp/artifacts as server.ts does.
fn output_dir() -> Option<PathBuf> {
    match std::env::var_os("TELESCODE_TOUR_OUTPUT").filter(|p| !p.is_empty()) {
        Some(dir) => Some(PathBuf::from(dir)),
        None => mcp::mcp_dir().ok().map(|dir| dir.join("artifacts")),
    }
}

/// Handles each event line: its tool calls go to the follower, when there is
/// one, what it reports using is added to `usage`, and its timing to `metrics`.
fn line_handler(followed: Option<&Followed>, usage: Arc<Mutex<Usage>>, metrics: Arc<Mutex<RunMetrics>>) -> OnLine {
    let report = followed.map(|f| f.on_tool_call.clone());
    Arc::new(move |line: &str| {
        if let Some(report) = &report {
            for call in tool_calls(line) {
                report(call)
            }
        }
        usage.lock().unwrap_or_else(|e| e.into_inner()).read(line);
        metrics.lock().unwrap_or_else(|e| e.into_inner()).read(line);
    })
}

/// `args` with the agent printing each event as a JSON line, which is how its
/// tool calls are followed and its usage read.
fn with_events(provider: Provider, mut args: Vec<OsString>) -> Vec<OsString> {
    match provider {
        Provider::Claude => args.extend(["--output-format".into(), "stream-json".into(), "--verbose".into()]),
        Provider::Codex => args.insert(1, "--json".into()),
    }
    args
}

/// `args` with the arguments for `model`, before Codex's trailing prompt
/// (Claude Code takes options anywhere).
fn with_model(provider: Provider, mut args: Vec<OsString>, model: Model) -> Vec<OsString> {
    let at = args.len().saturating_sub(usize::from(provider == Provider::Codex));
    args.splice(at..at, model_args(provider, model));
    args
}

/// Runs the agent once and records what it used in the usage log; `work`
/// names what it writes there, and `correlation` what it wrote for.
#[allow(clippy::too_many_arguments)]
pub fn generate(
    provider: Provider,
    work: Work,
    model: Model,
    correlation: Correlation,
    prompt: &str,
    tools: &[&str],
    timeout: Duration,
    followed: Option<Followed>,
) -> Result<(), BridgeError> {
    // In tests no real model is called unless asked for, within a budget.
    #[cfg(test)]
    let admission = crate::llm_budget::admit().map_err(|message| BridgeError::TourGenerationFailed { message })?;
    let server = mcp::server_command()?;
    let (path, name) = match provider {
        Provider::Claude => (
            claude::find().ok_or_else(claude::not_installed)?,
            "Claude Code",
        ),
        Provider::Codex => (codex::find().ok_or_else(codex::not_installed)?, "Codex"),
    };
    let mut cmd = command(&path);
    cmd.args(with_events(provider, with_model(provider, generation_args(provider, prompt, &server, tools), model)));
    let run = UsageRecord {
        at_ms: 0,
        run_id: new_run_id(),
        work,
        provider,
        model,
        duration_ms: 0,
        ok: false,
        error_code: None,
        scenario: scenario(std::env::var(SCENARIO_ENV).ok()),
        correlation,
        usage: Usage::default(),
        metrics: RunMetrics::new(Instant::now()),
    };
    let (result, record) = run_recorded(cmd, name, timeout, followed, run);
    #[cfg(test)]
    admission.settle(&record).map_err(|e| BridgeError::Internal { message: format!("Could not write the real LLM ledger: {e}") })?;
    if let Some(dir) = output_dir() {
        // Measuring is best effort; a log that cannot be written never fails the run.
        let _ = record_usage(&dir, &record);
    }
    result
}

/// Runs `cmd`, an agent printing JSON event lines, and fills in `run` with
/// what the run took and used.
fn run_recorded(
    cmd: Command,
    what: &str,
    timeout: Duration,
    followed: Option<Followed>,
    mut run: UsageRecord,
) -> (Result<(), BridgeError>, UsageRecord) {
    let usage = Arc::new(Mutex::new(Usage::default()));
    let started = Instant::now();
    let metrics = Arc::new(Mutex::new(RunMetrics::new(started)));
    let on_line = line_handler(followed.as_ref(), usage.clone(), metrics.clone());
    let result = run_streaming(cmd, timeout, what, Some(on_line), followed.as_ref().map(|f| &*f.cancelled));
    run.at_ms = now_ms();
    run.duration_ms = started.elapsed().as_millis();
    run.ok = result.is_ok();
    run.error_code = result.as_ref().err().and_then(|e| serde_json::to_value(e).ok()).and_then(|v| v["code"].as_str().map(str::to_owned));
    run.usage = usage.lock().unwrap_or_else(|e| e.into_inner()).clone();
    run.metrics = metrics.lock().unwrap_or_else(|e| e.into_inner()).clone();
    run.metrics.finish_run();
    (result, run)
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
    run_streaming(cmd, timeout, what, None, None)
}

/// `run`, handing each stdout line to `on_line` as it is printed, and
/// stopping the process once `cancelled` is set.
pub fn run_streaming(
    mut cmd: Command,
    timeout: Duration,
    what: &str,
    on_line: Option<OnLine>,
    cancelled: Option<&AtomicBool>,
) -> Result<(), BridgeError> {
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
        if cancelled.is_some_and(|c| c.load(Ordering::SeqCst)) {
            let _ = child.kill();
            let _ = child.wait();
            return Err(BridgeError::AgentCancelled {
                message: format!("{what} was stopped before it finished."),
            });
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
        run_streaming(shell("printf 'one\\ntwo\\n'"), Duration::from_secs(3), "test", Some(on_line), None).unwrap();
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

    #[test]
    fn claude_usage_is_read_from_its_result_event() {
        let mut usage = Usage::default();
        usage.read(r#"{"type":"assistant","message":{"content":[]}}"#);
        usage.read(r#"{"type":"result","subtype":"success","duration_ms":4200,"total_cost_usd":0.031,"usage":{"input_tokens":12,"cache_creation_input_tokens":3000,"cache_read_input_tokens":40000,"output_tokens":850}}"#);
        // Input read from the prompt cache is kept apart from new input.
        assert_eq!(usage, Usage { input_tokens: 3012, cached_input_tokens: 40000, output_tokens: 850, cost_usd: Some(0.031) });
    }

    #[test]
    fn codex_usage_is_summed_over_its_turns() {
        let mut usage = Usage::default();
        for line in [
            r#"{"type":"thread.started","thread_id":"t"}"#,
            r#"{"type":"turn.completed","usage":{"input_tokens":1000,"cached_input_tokens":600,"output_tokens":200,"reasoning_output_tokens":50}}"#,
            "not json",
            r#"{"type":"turn.completed","usage":{"input_tokens":500,"cached_input_tokens":100,"output_tokens":80}}"#,
        ] {
            usage.read(line)
        }
        // Codex counts cached input inside input_tokens; it is kept apart here as for Claude.
        assert_eq!(usage, Usage { input_tokens: 800, cached_input_tokens: 700, output_tokens: 280, cost_usd: None });
    }

    #[test]
    fn each_run_is_appended_to_the_usage_log_as_one_json_line() {
        let dir = std::env::temp_dir().join(format!("telescode-usage-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let run = |work: Work, ok: bool| UsageRecord {
            at_ms: 1, run_id: "r1".into(), work, provider: Provider::Claude, model: Model::Light, duration_ms: 4200, ok,
            error_code: None, scenario: None, correlation: Correlation::default(),
            usage: Usage { input_tokens: 10, cached_input_tokens: 20, output_tokens: 30, cost_usd: Some(0.5) },
            metrics: RunMetrics::new(Instant::now()),
        };
        record_usage(&dir, &run(Work::Quiz, true)).unwrap();
        record_usage(&dir, &run(Work::LearningMaterial, false)).unwrap();
        let text = std::fs::read_to_string(dir.join(USAGE_LOG)).unwrap();
        let lines: Vec<serde_json::Value> = text.lines().map(|l| serde_json::from_str(l).unwrap()).collect();
        assert_eq!(lines.len(), 2);
        assert_eq!(lines[0]["work"], "quiz");
        assert_eq!(lines[0]["model"], "light");
        assert_eq!(lines[0]["provider"], "claude");
        assert_eq!(lines[0]["durationMs"], 4200);
        assert_eq!(lines[0]["usage"]["cachedInputTokens"], 20);
        assert_eq!(lines[1]["ok"], false);
        assert_eq!(lines[1]["work"], "learning-material");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn one_line_reports_its_tool_calls_and_counts_its_usage() {
        let calls = Arc::new(Mutex::new(Vec::new()));
        let seen = calls.clone();
        let tracked = follow("usage-line-test".into(), Arc::new(move |c| seen.lock().unwrap().push(c)));
        let usage = Arc::new(Mutex::new(Usage::default()));
        let metrics = Arc::new(Mutex::new(RunMetrics::new(Instant::now())));
        let on_line = line_handler(Some(&tracked), usage.clone(), metrics.clone());
        on_line(r#"{"type":"assistant","message":{"content":[{"type":"tool_use","name":"mcp__telescode__create_quiz","input":{}}]}}"#);
        on_line(r#"{"type":"result","usage":{"input_tokens":5,"output_tokens":7}}"#);
        assert_eq!(calls.lock().unwrap().iter().map(|c| c.tool.clone()).collect::<Vec<_>>(), ["create_quiz"]);
        assert_eq!(usage.lock().unwrap().output_tokens, 7);
        assert_eq!(metrics.lock().unwrap().tool_calls.len(), 1);
        // Without a follower, usage is still counted.
        let quiet = Arc::new(Mutex::new(Usage::default()));
        line_handler(None, quiet.clone(), metrics)(r#"{"type":"result","usage":{"input_tokens":5,"output_tokens":9}}"#);
        assert_eq!(quiet.lock().unwrap().output_tokens, 9);
    }

    /// A stand-in agent that prints `fixture` and exits with `code`.
    fn fake_agent(fixture: &str, code: i32) -> Command {
        let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures").join(fixture);
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            let mut cmd = Command::new("cmd");
            // As is: cmd does not read the quoting Rust gives its arguments.
            cmd.raw_arg(format!("/C type \"{}\" & exit /b {code}", path.display()));
            cmd
        }
        #[cfg(not(windows))]
        {
            let mut cmd = Command::new("/bin/sh");
            cmd.args(["-c", &format!("cat '{}'; exit {code}", path.display())]);
            cmd
        }
    }

    fn blank_run(correlation: Correlation) -> UsageRecord {
        UsageRecord {
            at_ms: 0, run_id: new_run_id(), work: Work::Quiz, provider: Provider::Claude, model: Model::Light, duration_ms: 0, ok: false,
            error_code: None, scenario: scenario(Some("baseline-quiz".into())), correlation, usage: Usage::default(),
            metrics: RunMetrics::new(Instant::now()),
        }
    }

    #[test]
    fn a_fake_agent_run_is_recorded_with_its_usage_phases_and_tool_calls() {
        let tracked = follow("record-test".into(), Arc::new(|_| {}));
        let correlation = Correlation { tour_id: Some("tour-1".into()), stop_index: Some(2), ..Default::default() };
        let (result, record) = run_recorded(fake_agent("claude-quiz.jsonl", 0), "Claude Code", Duration::from_secs(10), Some(tracked), blank_run(correlation));
        result.unwrap();
        assert!(record.ok && record.error_code.is_none());
        assert_eq!(record.usage, Usage { input_tokens: 2109, cached_input_tokens: 41000, output_tokens: 765, cost_usd: Some(0.021) });
        assert!(record.metrics.usage_reported);
        assert_eq!(record.metrics.model_calls, Some(3));
        assert_eq!(record.metrics.saved_questions, Some(2));
        assert_eq!(record.metrics.tool_calls.iter().map(|c| c.tool.as_str()).collect::<Vec<_>>(), ["get_quiz_context", "create_quiz"]);
        assert!(record.metrics.ready_ms.is_some() && record.metrics.first_tool_call_ms >= record.metrics.ready_ms);

        let line: serde_json::Value = serde_json::to_value(&record).unwrap();
        assert_eq!(line["scenario"], "baseline-quiz");
        assert_eq!(line["correlation"], serde_json::json!({"tourId":"tour-1","stopIndex":2}));
        assert_eq!(line["metrics"]["savedQuestions"], 2);
        assert_eq!(line["metrics"]["toolCalls"][1]["ok"], true);
        // The fields that were there before stay as they were.
        for key in ["atMs", "work", "provider", "model", "durationMs", "ok", "usage"] {
            assert!(line.get(key).is_some(), "{key}");
        }
    }

    #[test]
    fn a_failed_fake_run_keeps_its_error_code_and_what_it_reported() {
        let (result, record) = run_recorded(fake_agent("codex-material.jsonl", 3), "Codex", Duration::from_secs(10), None, blank_run(Correlation::default()));
        assert!(result.is_err());
        assert!(!record.ok);
        assert_eq!(record.error_code.as_deref(), Some("tour_generation_failed"));
        assert_eq!(record.usage.output_tokens, 1400);
        assert_eq!(record.metrics.model_calls, None);
        // The error message, with the CLI's output in it, stays out of the log.
        let text = serde_json::to_string(&record).unwrap();
        assert!(!text.contains("exit") && !text.contains("fixtures"), "{text}");
    }

    #[test]
    fn scenario_names_are_kept_only_when_they_cannot_hold_a_path() {
        assert_eq!(scenario(Some(" baseline-quiz.cold_1 ".into())).as_deref(), Some("baseline-quiz.cold_1"));
        for bad in ["", "C:\\Users\\me", "/home/me/repo", "a b", &"x".repeat(65)] {
            assert_eq!(scenario(Some(bad.into())), None, "{bad}");
        }
        assert_eq!(scenario(None), None);
    }

    #[test]
    fn every_run_gets_its_own_id() {
        let ids: std::collections::HashSet<_> = (0..100).map(|_| new_run_id()).collect();
        assert_eq!(ids.len(), 100);
    }

    #[test]
    fn tests_cannot_start_a_real_agent_unless_asked() {
        use crate::llm_budget::{MOCK_ENV, REAL_ENV};
        // When a developer runs the suite with real runs on, this one has nothing to show.
        if std::env::var_os(MOCK_ENV).is_some() || std::env::var_os(REAL_ENV).is_some() {
            return;
        }
        let error = generate(Provider::Claude, Work::Quiz, Model::Light, Correlation::default(), "x", &[], Duration::from_secs(1), None).unwrap_err();
        let BridgeError::TourGenerationFailed { message } = error else { panic!("unexpected error {error:?}") };
        assert!(message.contains(REAL_ENV), "{message}");
    }

    #[test]
    fn every_run_prints_json_events_so_its_usage_can_be_read() {
        let server = mcp::ServerCommand { program: "node".into(), args: vec!["/t/server.ts".into()], env: vec![] };
        let claude = with_events(Provider::Claude, generation_args(Provider::Claude, "x", &server, &[]));
        assert!(claude.windows(2).any(|w| w[0] == "--output-format" && w[1] == "stream-json"));
        assert!(claude.iter().any(|a| a == "--verbose"));
        let codex = with_events(Provider::Codex, generation_args(Provider::Codex, "x", &server, &[]));
        assert_eq!(codex[1], "--json");
        assert_eq!(codex.last().unwrap(), "x");
    }

    #[test]
    fn the_model_goes_before_codex_prompt() {
        let server = mcp::ServerCommand { program: "node".into(), args: vec!["/t/server.ts".into()], env: vec![] };
        let codex = with_model(Provider::Codex, generation_args(Provider::Codex, "Write a quiz.", &server, &["create_quiz"]), Model::Light);
        assert_eq!(codex.last().unwrap(), "Write a quiz.");
        let m = codex.iter().position(|a| a == "-m").unwrap();
        assert_eq!(codex[m + 1], "gpt-6-luna");
        let claude = with_model(Provider::Claude, generation_args(Provider::Claude, "Write a quiz.", &server, &["create_quiz"]), Model::Light);
        assert_eq!(&claude[..2], ["-p", "Write a quiz."]);
        assert!(claude.windows(2).any(|w| w[0] == "--model" && w[1] == "sonnet"));
        assert_eq!(with_model(Provider::Codex, generation_args(Provider::Codex, "x", &server, &[]), Model::Default), generation_args(Provider::Codex, "x", &server, &[]));
    }

    #[cfg(unix)]
    #[test]
    fn cancelling_a_followed_run_stops_the_child() {
        let tracked = follow("cancel-test".into(), Arc::new(|_| {}));
        let flag = tracked.cancelled.clone();
        let canceller = std::thread::spawn(|| {
            std::thread::sleep(Duration::from_millis(100));
            cancel("cancel-test");
        });
        let started = Instant::now();
        let error = run_streaming(shell("exec sleep 10"), Duration::from_secs(30), "Claude Code", None, Some(&flag)).unwrap_err();
        canceller.join().unwrap();
        assert!(matches!(error, BridgeError::AgentCancelled { .. }), "{error:?}");
        assert!(started.elapsed() < Duration::from_secs(2));
        // A finished run is forgotten, so a late cancel finds nothing.
        drop(tracked);
        assert!(!followed().lock().unwrap().contains_key("cancel-test"));
        cancel("cancel-test");
    }
}
