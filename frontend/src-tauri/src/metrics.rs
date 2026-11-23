//! Where one agent run spends its time, read from the same JSON event stream
//! as its usage: when the CLI was ready, when the model first answered, each
//! Telescode tool call with how long it took and how much it sent back, and
//! how many questions the saves that went through carried.
//!
//! Only what the stream reports is kept. What it does not report is `None`
//! (`null` in the usage log), never estimated. Nothing the agent read or wrote
//! is kept, only names, timings and sizes, so the log holds no source, prompt
//! or path.
use serde::Serialize;
use serde_json::Value;
use std::collections::{HashMap, HashSet};
use std::time::Instant;

/// The tools that save generated questions; a save that went through carries
/// questions the server accepted.
const SAVE_TOOLS: &[&str] = &["create_quiz", "save_learning_curriculum", "save_learning_material"];

/// One tool call the agent made.
#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ToolMetric {
    /// Without the `mcp__telescode__` prefix, as in `agent::ToolCall`.
    pub tool: String,
    /// When the call was seen, from the start of the run.
    pub start_ms: u64,
    /// Until its result was seen; `None` when no result was.
    pub duration_ms: Option<u64>,
    /// Size of the arguments the agent sent, as JSON.
    pub args_bytes: Option<u64>,
    /// Size of what the tool sent back.
    pub result_bytes: Option<u64>,
    /// Whether the tool succeeded; `None` until its result is seen.
    pub ok: Option<bool>,
    /// Questions in the arguments of a save tool; `None` for other tools.
    #[serde(skip)]
    questions: Option<u64>,
}

/// The phases and calls of one run.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RunMetrics {
    /// From the start of the run (just before the CLI is spawned) to its first
    /// event line.
    pub first_event_ms: Option<u64>,
    /// To the CLI saying it is ready to work: Claude Code's `system` `init`
    /// (sent once its MCP servers are connected), Codex's `turn.started`.
    pub ready_ms: Option<u64>,
    /// To the model's first output: an assistant message (Claude Code), or
    /// any item other than a tool call (Codex: reasoning, a message).
    pub first_model_output_ms: Option<u64>,
    /// To the first tool call.
    pub first_tool_call_ms: Option<u64>,
    /// Model requests made: distinct assistant message IDs for Claude Code.
    /// Codex does not report them.
    pub model_calls: Option<u64>,
    /// Time Claude Code reports spending in the API; Codex reports none.
    pub api_duration_ms: Option<u64>,
    pub tool_calls: Vec<ToolMetric>,
    /// Questions in the save calls that went through; see `saved_questions`.
    pub saved_questions: Option<u64>,
    /// Whether the stream reported what the run used: Claude Code's `result`,
    /// a Codex `turn.completed`. Without it the usage counts are not known.
    pub usage_reported: bool,
    #[serde(skip)]
    started: Instant,
    #[serde(skip)]
    messages: HashSet<String>,
    /// Open tool calls, by the ID the stream gives them, to their index.
    #[serde(skip)]
    open: HashMap<String, usize>,
}

impl RunMetrics {
    pub fn new(started: Instant) -> Self {
        Self {
            first_event_ms: None,
            ready_ms: None,
            first_model_output_ms: None,
            first_tool_call_ms: None,
            model_calls: None,
            api_duration_ms: None,
            tool_calls: Vec::new(),
            saved_questions: None,
            usage_reported: false,
            started,
            messages: HashSet::new(),
            open: HashMap::new(),
        }
    }

    /// Reads one event line seen now.
    pub fn read(&mut self, line: &str) {
        let at = self.started.elapsed().as_millis() as u64;
        self.read_at(line, at)
    }

    /// Reads one event line seen `at` milliseconds into the run.
    pub fn read_at(&mut self, line: &str, at: u64) {
        let Ok(event) = serde_json::from_str::<Value>(line.trim()) else { return };
        let Some(kind) = event.get("type").and_then(Value::as_str) else { return };
        self.first_event_ms.get_or_insert(at);
        match kind {
            // Claude Code
            "system" if event.get("subtype").and_then(Value::as_str) == Some("init") => {
                self.ready_ms.get_or_insert(at);
            }
            "assistant" => {
                self.first_model_output_ms.get_or_insert(at);
                let message = event.get("message");
                // Claude Code sends each content block of a message as its own event.
                if let Some(id) = message.and_then(|m| m.get("id")).and_then(Value::as_str) {
                    self.messages.insert(id.to_owned());
                    self.model_calls = Some(self.messages.len() as u64);
                }
                for block in content(message) {
                    if block.get("type").and_then(Value::as_str) != Some("tool_use") {
                        continue;
                    }
                    let name = block.get("name").and_then(Value::as_str).unwrap_or_default();
                    let id = block.get("id").and_then(Value::as_str).unwrap_or_default();
                    self.start(id, tool_name(name), block.get("input"), at);
                }
            }
            "user" => {
                for block in content(event.get("message")) {
                    if block.get("type").and_then(Value::as_str) != Some("tool_result") {
                        continue;
                    }
                    let id = block.get("tool_use_id").and_then(Value::as_str).unwrap_or_default();
                    let failed = block.get("is_error").and_then(Value::as_bool).unwrap_or(false);
                    self.finish(id, block.get("content"), !failed, at);
                }
            }
            "result" => {
                self.usage_reported |= event.get("usage").is_some();
                if let Some(ms) = event.get("duration_api_ms").and_then(Value::as_u64) {
                    self.api_duration_ms = Some(ms);
                }
            }
            // Codex
            "turn.started" => {
                self.ready_ms.get_or_insert(at);
            }
            "turn.completed" => self.usage_reported |= event.get("usage").is_some(),
            "item.started" | "item.updated" | "item.completed" => {
                let Some(item) = event.get("item") else { return };
                if item.get("type").and_then(Value::as_str) != Some("mcp_tool_call") {
                    self.first_model_output_ms.get_or_insert(at);
                    return;
                }
                let id = item.get("id").and_then(Value::as_str).unwrap_or_default();
                if !self.open.contains_key(id) {
                    let tool = item.get("tool").and_then(Value::as_str).unwrap_or_default();
                    self.start(id, tool, item.get("arguments"), at);
                }
                if kind == "item.completed" {
                    let ok = item.get("status").and_then(Value::as_str) == Some("completed")
                        && item.get("error").is_none_or(Value::is_null);
                    let result = item.get("result").filter(|r| !r.is_null()).or_else(|| item.get("error").filter(|e| !e.is_null()));
                    self.finish(id, result, ok, at);
                }
            }
            _ => {}
        }
    }

    fn start(&mut self, id: &str, tool: &str, args: Option<&Value>, at: u64) {
        self.first_tool_call_ms.get_or_insert(at);
        // Codex may send the arguments as a JSON string.
        let parsed = args.and_then(Value::as_str).and_then(|s| serde_json::from_str::<Value>(s).ok());
        let args = parsed.as_ref().or(args);
        let questions = SAVE_TOOLS.contains(&tool).then(|| args.map_or(0, count_questions));
        self.tool_calls.push(ToolMetric {
            tool: tool.to_owned(),
            start_ms: at,
            duration_ms: None,
            args_bytes: args.map(json_bytes),
            result_bytes: None,
            ok: None,
            questions,
        });
        if !id.is_empty() {
            self.open.insert(id.to_owned(), self.tool_calls.len() - 1);
        }
    }

    fn finish(&mut self, id: &str, result: Option<&Value>, ok: bool, at: u64) {
        let Some(index) = self.open.remove(id) else { return };
        let call = &mut self.tool_calls[index];
        call.duration_ms = Some(at.saturating_sub(call.start_ms));
        call.result_bytes = result.map(json_bytes);
        call.ok = Some(ok);
        self.saved_questions = self.count_saved();
    }

    /// Questions in the save calls that went through: exact for a quiz, which
    /// the server saves whole or not at all; an upper bound for learning
    /// material, from which the server may still leave out repeated
    /// questions. `None` while a save call's outcome is not known.
    fn count_saved(&self) -> Option<u64> {
        let saves = self.tool_calls.iter().filter(|c| c.questions.is_some());
        saves.map(|c| c.ok.map(|ok| if ok { c.questions.unwrap_or(0) } else { 0 })).sum()
    }

    /// Ends the run: the saved questions are known once every save call has
    /// its outcome; a run that made no save call saved none. A run with no
    /// event read (the CLI never started, or printed something else) is not
    /// known to have saved anything or nothing.
    pub fn finish_run(&mut self) {
        self.saved_questions = self.first_event_ms.and(self.count_saved());
    }
}

/// `message.content` as a list of blocks.
fn content(message: Option<&Value>) -> impl Iterator<Item = &Value> {
    message.and_then(|m| m.get("content")).and_then(Value::as_array).into_iter().flatten()
}

fn tool_name(name: &str) -> &str {
    name.strip_prefix("mcp__telescode__").unwrap_or(name)
}

fn json_bytes(value: &Value) -> u64 {
    match value {
        Value::String(s) => s.len() as u64,
        other => other.to_string().len() as u64,
    }
}

/// Every question in `value`: the items of each `questions` list in it.
fn count_questions(value: &Value) -> u64 {
    match value {
        Value::Object(map) => map
            .iter()
            .map(|(key, v)| match (key.as_str(), v) {
                ("questions", Value::Array(items)) => items.len() as u64,
                _ => count_questions(v),
            })
            .sum(),
        Value::Array(items) => items.iter().map(count_questions).sum(),
        _ => 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn read(lines: &str) -> RunMetrics {
        let mut metrics = RunMetrics::new(Instant::now());
        for (i, line) in lines.lines().enumerate() {
            metrics.read_at(line, (i as u64 + 1) * 100);
        }
        metrics.finish_run();
        metrics
    }

    #[test]
    fn claude_code_phases_calls_and_saved_questions_are_read_from_its_stream() {
        let m = read(include_str!("../tests/fixtures/claude-quiz.jsonl"));
        assert_eq!(m.first_event_ms, Some(100));
        assert_eq!(m.ready_ms, Some(100));
        assert_eq!(m.first_model_output_ms, Some(200));
        assert_eq!(m.first_tool_call_ms, Some(200));
        // Two content blocks of one message are one model request.
        assert_eq!(m.model_calls, Some(3));
        assert_eq!(m.api_duration_ms, Some(9100));
        let tools: Vec<_> = m.tool_calls.iter().map(|c| (c.tool.as_str(), c.duration_ms, c.ok)).collect();
        assert_eq!(tools, [("get_quiz_context", Some(100), Some(true)), ("create_quiz", Some(100), Some(true))]);
        assert_eq!(m.tool_calls[0].result_bytes, Some("{\"nodes\":[1,2,3]}".len() as u64));
        assert!(m.tool_calls[1].args_bytes.unwrap() > 0);
        assert_eq!(m.saved_questions, Some(2));
        assert!(m.usage_reported);
    }

    #[test]
    fn codex_phases_and_calls_are_read_and_what_it_does_not_report_stays_unknown() {
        let m = read(include_str!("../tests/fixtures/codex-material.jsonl"));
        assert_eq!(m.first_event_ms, Some(100));
        assert_eq!(m.ready_ms, Some(200));
        assert_eq!(m.first_model_output_ms, Some(300));
        assert_eq!(m.first_tool_call_ms, Some(400));
        assert_eq!(m.model_calls, None);
        assert_eq!(m.api_duration_ms, None);
        // A call is one call though Codex reports it starting and ending.
        assert_eq!(m.tool_calls.len(), 2);
        assert_eq!(m.tool_calls[0].tool, "get_learning_material_context");
        assert_eq!(m.tool_calls[0].duration_ms, Some(100));
        assert!(m.tool_calls[0].result_bytes.unwrap() > 0);
        // Arguments sent as a JSON string are counted as JSON.
        assert_eq!(m.saved_questions, Some(4));
        assert!(m.usage_reported);
    }

    #[test]
    fn a_rejected_save_counts_no_questions_and_a_retried_one_counts_once() {
        let m = read(concat!(
            r#"{"type":"assistant","message":{"id":"m1","content":[{"type":"tool_use","id":"a","name":"mcp__telescode__create_quiz","input":{"questions":[{},{}]}}]}}"#, "\n",
            r#"{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"a","is_error":true,"content":"Question 1: evidence outside the easy context"}]}}"#, "\n",
            r#"{"type":"assistant","message":{"id":"m2","content":[{"type":"tool_use","id":"b","name":"mcp__telescode__create_quiz","input":{"questions":[{},{}]}}]}}"#, "\n",
            r#"{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"b","content":[{"type":"text","text":"{}"}]}]}}"#,
        ));
        assert_eq!(m.tool_calls.iter().map(|c| c.ok).collect::<Vec<_>>(), [Some(false), Some(true)]);
        assert_eq!(m.saved_questions, Some(2));
    }

    #[test]
    fn a_run_cut_off_mid_call_leaves_what_it_did_not_see_unknown() {
        let m = read(concat!(
            r#"{"type":"system","subtype":"init"}"#, "\n",
            r#"{"type":"assistant","message":{"id":"m1","content":[{"type":"tool_use","id":"a","name":"mcp__telescode__create_quiz","input":{"questions":[{}]}}]}}"#, "\n",
            "partial {\"type\":",
        ));
        assert_eq!(m.tool_calls[0].duration_ms, None);
        assert_eq!(m.tool_calls[0].ok, None);
        assert_eq!(m.saved_questions, None);
        assert!(!m.usage_reported);
        // A run that never saved saved nothing; one with no events knows nothing.
        assert_eq!(read(r#"{"type":"result","usage":{}}"#).saved_questions, Some(0));
        let silent = read("");
        assert_eq!((silent.first_event_ms, silent.ready_ms, silent.model_calls, silent.saved_questions), (None, None, None, None));
        // Output that is not the event stream tells nothing either.
        assert_eq!(read("Error: unknown option --output-format").saved_questions, None);
    }

    #[test]
    fn the_metrics_keep_no_source_prompt_or_path() {
        let m = read(concat!(
            r#"{"type":"assistant","message":{"id":"m1","content":[{"type":"text","text":"SECRET-REPLY"},{"type":"tool_use","id":"a","name":"mcp__telescode__open_project","input":{"path":"/home/someone/private/repo.db"}}]}}"#, "\n",
            r#"{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"a","content":"def secret_source(): pass"}]}}"#,
        ));
        let json = serde_json::to_string(&m).unwrap();
        for leak in ["SECRET-REPLY", "/home/someone", "secret_source"] {
            assert!(!json.contains(leak), "{leak} in {json}");
        }
    }
}
