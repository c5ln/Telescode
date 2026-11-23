//! CPU planning lives in MCP; the selected logged-in CLI only prepares source-grounded material.
use serde_json::{json, Value};
use std::time::{Duration, Instant};
use crate::{agent, mcp};
use crate::bridge::{validate_db_path, BridgeError};

const PREPARATION_TOOLS: &[&str] = &["get_learning_preparation_context", "save_learning_curriculum"];
const MATERIAL_TOOLS: &[&str] = &["get_learning_material_context", "save_learning_material"];
/// How long writing the next questions may take; they are a few questions, not a curriculum.
const MATERIAL_TIMEOUT: Duration = Duration::from_secs(5 * 60);

/// What the agent is asked for in a session's first preparation: a small
/// start, as the context's `rules.start` sets it. Higher levels are written
/// later, while the learner answers.
fn preparation_prompt(job_id: &str) -> String {
    format!("Prepare Telescode adaptive learning job {job_id}. Call get_learning_preparation_context with exactly this job ID, inspect its grounded code excerpts and immutable goal, then save_learning_curriculum with the same job ID. Start small, as the context's rules.start says: a concept map of at most rules.start.maxConcepts concepts with their relations, and actions at the rules.start.levels only, with two questions in all, both about the goal concepts in rules.start.concepts. The higher levels are written later, while the learner answers. Follow the server's adaptive learning instructions. Use the existing concept definitions exactly. Save only material; never answer or advance a learner session. Reply with the job ID.")
}

/// What the material agent is asked to do for one job.
fn material_prompt(job_id: &str) -> String {
    format!("Write Telescode learning material for job {job_id}. Call get_learning_material_context with exactly this job ID, then save_learning_material with the same job ID: one action for each asked concept and level, with two new questions each where the source supports them, grounded in that level's evidence and different from the questions already written. Write only the asked levels. Save only material; never answer or advance a learner session. Reply with the job ID.")
}
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
/// `learner_id` keeps what this learner was already asked apart from other
/// learners'; without it the server's default local learner is used.
pub async fn start_learning_session(db_path: String, goal: Value, learner_id: Option<String>) -> Result<Value, BridgeError> {
    blocking(move || {
        let db = validate_db_path(&db_path)?;
        let mut input = json!({"path":db,"goal":goal});
        if let Some(learner) = learner_id { input["learnerId"] = json!(learner); }
        mcp::call("start_learning_session", input)
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
/// `curriculum` is material written in advance (the example repositories
/// ship one for their suggested goal): it is saved as the job's result
/// instead of running an agent.
/// With a `progressId`, each tool the agent calls is sent as an `agent-progress` event.
pub async fn prepare_learning_session<R: tauri::Runtime>(
    app: tauri::AppHandle<R>,
    session_id: String,
    revision: u64,
    provider: Option<agent::Provider>,
    curriculum: Option<Value>,
    progress_id: Option<String>,
) -> Result<Value, BridgeError> {
    let followed = agent::emit_progress(&app, progress_id);
    blocking(move || {
        // Includes reservation so queued duplicates observe the first worker's saved materials.
        let serial = PREPARATION.lock().unwrap_or_else(|e| e.into_inner());
        let provider = provider.unwrap_or_default();
        // Material the app brings (an example's) is saved as is, never replaced by a cached curriculum.
        let job = mcp::call("request_learning_preparation", json!({"sessionId":session_id,"revision":revision,"provider":provider,"ownerPid":std::process::id(),"useCache":curriculum.is_none()}))?;
        let job_id = job["id"].as_str().unwrap_or_default();
        if let (true, Some(bundled)) = (job["shouldRun"] == true, curriculum.as_ref()) {
            // The server takes the version after the session's current one,
            // which is higher when a retry supplements earlier material.
            let mut material = bundled.clone();
            material["version"] = json!(job["baseVersion"].as_u64().unwrap_or(0) + 1);
            let saved = mcp::call("save_learning_curriculum", json!({"jobId":job_id,"curriculum":material}));
            let completion = match &saved {
                Err(e) => json!({"jobId":job_id,"error":bounded_error(&format!("{e:?}"))}),
                Ok(_) => json!({"jobId":job_id}),
            };
            mcp::call("finish_learning_preparation", completion)?;
            saved?;
        } else if job["shouldRun"] == true {
            let prompt = preparation_prompt(job_id);
            let correlation = agent::Correlation { session_id: Some(session_id.clone()), job_id: Some(job_id.to_owned()), ..Default::default() };
            let result = agent::generate(provider, agent::Work::Learning, agent::Model::Light, correlation, &prompt, PREPARATION_TOOLS, Duration::from_secs(10 * 60), followed);
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

#[tauri::command]
/// Writes what the next step of an ongoing session may need, while the
/// learner answers: the server works out the levels either answer leads to,
/// adds cached material, and the selected agent (its light model) writes the
/// rest. Resolves with the session as it then is.
/// With a `progressId`, each tool the agent calls is sent as an `agent-progress` event.
pub async fn prepare_learning_material<R: tauri::Runtime>(
    app: tauri::AppHandle<R>,
    session_id: String,
    provider: Option<agent::Provider>,
    progress_id: Option<String>,
) -> Result<Value, BridgeError> {
    let followed = agent::emit_progress(&app, progress_id);
    blocking(move || {
        let provider = provider.unwrap_or_default();
        let job = mcp::call("request_learning_material", json!({"sessionId":session_id,"provider":provider,"ownerPid":std::process::id()}))?;
        if job["shouldRun"] == true {
            let job_id = job["id"].as_str().unwrap_or_default().to_string();
            let correlation = agent::Correlation { session_id: Some(session_id.clone()), job_id: Some(job_id.clone()), ..Default::default() };
            let result = agent::generate(provider, agent::Work::LearningMaterial, agent::Model::Light, correlation, &material_prompt(&job_id), MATERIAL_TOOLS, MATERIAL_TIMEOUT, followed);
            let completion = match result.as_ref().err() {
                Some(e) => json!({"jobId":job_id,"error":bounded_error(&format!("{e:?}"))}),
                None => json!({"jobId":job_id}),
            };
            mcp::call("finish_learning_material", completion)?;
            result?;
        } else if job["status"] == "failed" {
            return Err(BridgeError::TourGenerationFailed { message: job["error"].as_str().unwrap_or("AI material budget exhausted").into() });
        }
        mcp::call("get_learning_session", json!({"sessionId":session_id}))
    }).await
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_first_preparation_asks_for_a_small_concept_map_and_two_starting_questions() {
        let prompt = preparation_prompt("job-3");
        assert!(prompt.contains("job-3"));
        assert!(prompt.contains("get_learning_preparation_context") && prompt.contains("save_learning_curriculum"));
        assert!(prompt.contains("rules.start"));
        assert!(prompt.contains("two questions"));
        // Both about the goal concept, which is what the planner works toward.
        assert!(prompt.contains("rules.start.concepts"));
        // The rest is written while the learner answers, not up front.
        assert!(!prompt.contains("at least two independent scenario questions per target"));
    }

    #[test]
    fn the_material_agent_gets_only_its_job_and_the_two_material_tools() {
        let prompt = material_prompt("job-7");
        assert!(prompt.contains("job-7"));
        assert!(prompt.contains("get_learning_material_context") && prompt.contains("save_learning_material"));
        assert!(prompt.contains("only the asked"));
        assert_eq!(MATERIAL_TOOLS, ["get_learning_material_context", "save_learning_material"]);
    }

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
