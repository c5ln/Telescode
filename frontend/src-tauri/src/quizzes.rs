//! Comprehension quizzes on a tutorial step.
//!
//! Like tours, quizzes are kept by the Telescode MCP server (`mcp/`), and
//! written by Claude Code running headless with that server attached: the
//! server builds the graph context each question may use and refuses
//! questions that cite anything outside it. Answers are graded and recorded
//! by the server too; the shell only passes them on.

use serde::Deserialize;
use serde_json::{json, Value};
use std::ffi::OsString;
use std::path::Path;
use std::time::Duration;

use crate::bridge::{validate_db_path, BridgeError};
use crate::{claude, mcp};

/// Longest a generation may run before it is stopped.
const GENERATION_TIMEOUT: Duration = Duration::from_secs(10 * 60);

/// Reading the analysis, the quiz context, and saving the quiz. Never
/// `record_quiz_response`: the agent does not answer quizzes.
const AGENT_TOOLS: &[&str] = &["open_project", "inspect_node", "get_quiz_context", "create_quiz"];

/// The tour step a quiz is for.
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct QuizTarget {
    db_path: String,
    tour_id: String,
    tour_revision: u32,
    stop_index: u32,
    node_id: String,
}

/// What the agent is asked to do. The server's quiz instructions, which it
/// receives on connecting, say how to build the context and write questions.
fn generation_prompt(db: &Path, target: &QuizTarget) -> String {
    format!(
        "Create Telescode comprehension quiz questions for stop {stop} (node {node}) of tour {tour} \
         revision {revision}, for the repository analyzed in the database {db}.\n\
         Use the telescode MCP tools: open_project with exactly that path, then get_quiz_context for \
         the node at each difficulty (easy, medium, hard), and save with create_quiz. Write two \
         interview-style questions per difficulty where the source supports them: easy at a newbie \
         level, medium at a junior level, hard at a senior level, about what the code does and \
         why, never counts or metrics. Follow the server's quiz instructions on question style and \
         grounding, and write the questions in English.\n\
         Only save the quiz. Reply with the quiz set ID.",
        stop = target.stop_index,
        node = target.node_id,
        tour = target.tour_id,
        revision = target.tour_revision,
        db = db.display()
    )
}

/// The arguments for `claude`: print mode, only the Telescode server, and
/// only its quiz tools allowed.
fn claude_args(db: &Path, target: &QuizTarget, server: &mcp::ServerCommand) -> Vec<OsString> {
    let mut args = mcp::claude_config_args(server, AGENT_TOOLS);
    args.splice(0..0, ["-p".into(), generation_prompt(db, target).into()]);
    args
}

fn generate(target: &QuizTarget) -> Result<(), BridgeError> {
    let db = validate_db_path(&target.db_path)?;
    let server = mcp::server_command()?;
    let mut cmd = claude::command(&claude::find().ok_or_else(claude::not_installed)?);
    cmd.args(claude_args(&db, target, &server));
    claude::run(cmd, GENERATION_TIMEOUT, "Claude Code")
}

async fn blocking<T: Send + 'static>(f: impl FnOnce() -> Result<T, BridgeError> + Send + 'static) -> Result<T, BridgeError> {
    tauri::async_runtime::spawn_blocking(f).await.map_err(|e| BridgeError::Internal {
        message: format!("Quiz worker failed: {e}"),
    })?
}

/// IPC entry point: `invoke("list_quizzes")`. Every saved quiz set; the
/// frontend picks the one for the current tour step.
#[tauri::command]
pub async fn list_quizzes() -> Result<Value, BridgeError> {
    blocking(|| mcp::call("list_quizzes", json!({}))).await
}

/// IPC entry point: `invoke("generate_quiz", { target })`. Resolves once
/// Claude Code has finished; the new quiz is then in `list_quizzes`.
#[tauri::command]
pub async fn generate_quiz(target: QuizTarget) -> Result<(), BridgeError> {
    blocking(move || generate(&target)).await
}

/// IPC entry point: `invoke("record_quiz_response", { ... })`. The server
/// grades the answer, records it and returns the feedback.
#[tauri::command]
pub async fn record_quiz_response(
    quiz_set_id: String,
    quiz_id: String,
    selected_index: u32,
    response_time_ms: Option<f64>,
) -> Result<Value, BridgeError> {
    blocking(move || {
        mcp::call(
            "record_quiz_response",
            json!({
                "quizSetId": quiz_set_id,
                "quizId": quiz_id,
                "selectedIndex": selected_index,
                "responseTimeMs": response_time_ms,
            }),
        )
    })
    .await
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn claude_gets_only_the_quiz_tools_and_the_step_to_quiz_on() {
        let server = mcp::ServerCommand { program: "node".into(), args: vec!["/t/mcp/src/server.ts".into()], env: vec![] };
        let target = QuizTarget {
            db_path: "/t/repo.db".into(),
            tour_id: "tour-1".into(),
            tour_revision: 2,
            stop_index: 3,
            node_id: "file:a.py".into(),
        };
        let args: Vec<String> =
            claude_args(Path::new("/t/repo.db"), &target, &server).iter().map(|a| a.to_string_lossy().into_owned()).collect();
        assert_eq!(args[0], "-p");
        for part in ["/t/repo.db", "tour-1", "revision 2", "stop 3", "file:a.py", "create_quiz"] {
            assert!(args[1].contains(part), "prompt lacks {part}");
        }
        assert!(args.contains(&"--strict-mcp-config".to_string()));
        let allowed = &args[args.iter().position(|a| a == "--allowedTools").unwrap() + 1];
        assert!(allowed.contains("mcp__telescode__get_quiz_context") && allowed.contains("mcp__telescode__create_quiz"));
        assert!(!allowed.contains("record_quiz_response") && !allowed.contains("create_tour_draft"));
    }
}
