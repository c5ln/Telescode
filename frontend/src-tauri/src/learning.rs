//! CPU planning lives in MCP; the selected logged-in CLI only prepares source-grounded material.
use serde_json::{json, Value};
use std::time::{Duration, Instant};
use crate::{agent, mcp};
use crate::bridge::{validate_db_path, BridgeError};

const PREPARATION_TOOLS: &[&str] = &["get_learning_preparation_context", "save_learning_curriculum"];
static PREPARATION: std::sync::Mutex<()> = std::sync::Mutex::new(());
/** `finish_learning_preparation` accepts at most 2000 characters (JS UTF-16 units). */
const FINISH_ERROR_LIMIT: usize = 2000;
/** Bytes bound UTF-16 units, so cutting at a byte limit on a char boundary also fits the tool's limit. */
fn bounded_error(error: &str) -> String {
    if error.len() <= FINISH_ERROR_LIMIT { return error.to_string(); }
    let mut end = FINISH_ERROR_LIMIT;
    while !error.is_char_boundary(end) { end -= 1; }
    error[..end].to_string()
}
async fn blocking<T: Send + 'static>(work: impl FnOnce() -> Result<T, BridgeError> + Send + 'static) -> Result<T, BridgeError> {
    tauri::async_runtime::spawn_blocking(work).await.map_err(|e| BridgeError::Internal { message: format!("Learning worker failed: {e}") })?
}

#[tauri::command]
pub async fn start_learning_session(db_path: String, goal: Value) -> Result<Value, BridgeError> {
    blocking(move || {
        let db = validate_db_path(&db_path)?;
        mcp::call("start_learning_session", json!({"path":db,"goal":goal}))
    }).await
}
#[tauri::command]
pub async fn get_learning_session(session_id: String) -> Result<Value, BridgeError> {
    blocking(move || mcp::call("get_learning_session", json!({"sessionId":session_id}))).await
}
#[tauri::command]
pub async fn record_learning_response(answer: Value) -> Result<Value, BridgeError> {
    blocking(move || mcp::call("record_learning_response", answer)).await
}
#[tauri::command]
pub async fn advance_learning_session(input: Value) -> Result<Value, BridgeError> {
    blocking(move || mcp::call("advance_learning_session", input)).await
}
#[tauri::command]
pub async fn prepare_learning_session(session_id: String, revision: u64, provider: Option<agent::Provider>) -> Result<Value, BridgeError> {
    blocking(move || {
        // Includes reservation so queued duplicates observe the first worker's saved materials.
        let serial = PREPARATION.lock().unwrap_or_else(|e| e.into_inner());
        let provider = provider.unwrap_or_default();
        let job = mcp::call("request_learning_preparation", json!({"sessionId":session_id,"revision":revision,"provider":provider,"ownerPid":std::process::id()}))?;
        let job_id = job["id"].as_str().unwrap_or_default();
        if job["shouldRun"] == true {
            let prompt = format!("Prepare Telescode adaptive learning job {job_id}. Call get_learning_preparation_context with exactly this job ID, inspect its grounded code excerpts and immutable goal, then save_learning_curriculum with the same job ID. Follow the server's adaptive learning instructions. Prepare at least two independent scenario questions per target when supported. Use the requested target levels and existing concept definitions exactly. Save only material; never answer or advance a learner session. Reply with the job ID.");
            let result = agent::generate(provider, &prompt, PREPARATION_TOOLS, Duration::from_secs(10 * 60));
            let error = result.as_ref().err().map(|e| bounded_error(&format!("{e:?}")));
            let completion = match error {
                Some(error) => json!({"jobId":job_id,"error":error}),
                None => json!({"jobId":job_id}),
            };
            mcp::call("finish_learning_preparation", completion)?;
            result?;
        } else if job["status"] == "failed" {
            return Err(BridgeError::TourGenerationFailed { message: job["error"].as_str().unwrap_or("AI preparation budget exhausted").into() });
        }
        // Waiting on another app instance's live worker must not block this app's other sessions.
        drop(serial);
        let started = Instant::now();
        loop {
            let view = mcp::call("get_learning_session", json!({"sessionId":session_id}))?;
            if view["session"]["status"] != "preparing" { return Ok(view); }
            if started.elapsed() > Duration::from_secs(10 * 60) {
                return Err(BridgeError::TourGenerationFailed { message: "Learning preparation did not complete before its deadline.".into() });
            }
            std::thread::sleep(Duration::from_millis(250));
        }
    }).await
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn worker_errors_fit_the_finish_tool_limit_without_splitting_a_character() {
        // The MCP tool rejects errors over 2000 characters; a rejected report strands the job as running.
        let long = format!("{}{}", "x".repeat(1999), "한글🙂".repeat(3000));
        let bounded = bounded_error(&long);
        assert!(bounded.len() <= FINISH_ERROR_LIMIT, "{} bytes", bounded.len());
        assert!(bounded.encode_utf16().count() <= FINISH_ERROR_LIMIT);
        assert!(long.starts_with(&bounded));
        assert_eq!(bounded_error("short failure"), "short failure");
    }
}
