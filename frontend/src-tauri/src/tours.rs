//! Code tours, for the tutorial mode.
//!
//! Tours are kept by the Telescode MCP server (`mcp/`). The shell lists them
//! through the server's `list_tours` tool, and has one written by running
//! Claude Code headless with that server attached: the server never calls a
//! model itself, so an agent writes the captions, with the user's own Claude
//! Code sign-in.

use serde_json::{json, Value};
use std::ffi::OsString;
use std::path::Path;
use std::time::Duration;

use crate::bridge::{validate_db_path, BridgeError};
use crate::{claude, mcp};

/// Longest a generation may run before it is stopped.
const GENERATION_TIMEOUT: Duration = Duration::from_secs(15 * 60);

/// The only tools the agent may use: reading the analysis and saving a draft.
/// Frame previews and video rendering are left out; tutorials need neither.
const AGENT_TOOLS: &[&str] = &[
    "open_project",
    "get_project_overview",
    "search_nodes",
    "get_reading_tour_order",
    "inspect_node",
    "create_tour_draft",
    "revise_tour_draft",
    "get_tour_status",
];

/// What the agent is asked to do. The server's onboarding instructions,
/// which it receives on connecting, say how to write the tour.
fn generation_prompt(db: &Path) -> String {
    format!(
        "Create a Telescode onboarding tutorial for the repository analyzed in the database {db}.\n\
         Use the telescode MCP tools: open_project with exactly that path, then get_project_overview, \
         get_reading_tour_order and inspect_node, and save the tour with create_tour_draft. \
         Follow the server's onboarding instructions for the reading order and captions, \
         and write the title and captions in English.\n\
         Only save the draft. Do not preview frames, render a video, or wait for approval: \
         the user plays the draft as an interactive tutorial. Reply with the tour ID.",
        db = db.display()
    )
}

/// The arguments for `claude`: print mode, only the Telescode server, and
/// only its drafting tools allowed (print mode denies everything else).
fn claude_args(db: &Path, server: &mcp::ServerCommand) -> Vec<OsString> {
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
    let allowed = AGENT_TOOLS.iter().map(|t| format!("mcp__telescode__{t}")).collect::<Vec<_>>().join(",");
    vec![
        "-p".into(),
        generation_prompt(db).into(),
        "--mcp-config".into(),
        config.to_string().into(),
        "--strict-mcp-config".into(),
        "--allowedTools".into(),
        allowed.into(),
    ]
}

/// Runs Claude Code until it has saved a tour for `db_path`, or fails.
fn generate(db_path: &str) -> Result<(), BridgeError> {
    let db = validate_db_path(db_path)?;
    let server = mcp::server_command()?;
    let mut cmd = claude::command(&claude::find().ok_or_else(claude::not_installed)?);
    cmd.args(claude_args(&db, &server));
    claude::run(cmd, GENERATION_TIMEOUT, "Claude Code")
}

/// IPC entry point: `invoke("list_tours")`. Every tour the server has saved;
/// the frontend picks the one for the open repository.
#[tauri::command]
pub async fn list_tours() -> Result<Value, BridgeError> {
    tauri::async_runtime::spawn_blocking(|| mcp::call("list_tours", json!({})))
        .await
        .map_err(|e| BridgeError::Internal {
            message: format!("Tour worker failed: {e}"),
        })?
}

/// IPC entry point: `invoke("generate_tour", { dbPath })`. Resolves once
/// Claude Code has finished; the new tour is then in `list_tours`.
#[tauri::command]
pub async fn generate_tour(db_path: String) -> Result<(), BridgeError> {
    tauri::async_runtime::spawn_blocking(move || generate(&db_path))
        .await
        .map_err(|e| BridgeError::Internal {
            message: format!("Tour worker failed: {e}"),
        })?
}

#[cfg(test)]
mod tests {
    use super::*;

    fn server() -> mcp::ServerCommand {
        mcp::ServerCommand {
            program: "node".into(),
            args: vec!["--import".into(), "/t/mcp/node_modules/tsx/dist/loader.mjs".into(), "/t/mcp/src/server.ts".into()],
            env: vec![("TELESCODE_HEADLESS".into(), "/t/TelescodeHeadless".into())],
        }
    }

    #[test]
    fn claude_gets_only_the_telescode_server_and_its_drafting_tools() {
        let args = claude_args(Path::new("/t/repo.db"), &server());
        let args: Vec<String> = args.iter().map(|a| a.to_string_lossy().into_owned()).collect();
        assert_eq!(args[0], "-p");
        assert!(args[1].contains("/t/repo.db") && args[1].contains("create_tour_draft"));
        assert!(args.contains(&"--strict-mcp-config".to_string()));

        let config: Value = serde_json::from_str(&args[args.iter().position(|a| a == "--mcp-config").unwrap() + 1]).unwrap();
        let telescode = &config["mcpServers"]["telescode"];
        assert_eq!(telescode["command"], "node");
        assert_eq!(telescode["args"][2], "/t/mcp/src/server.ts");
        assert_eq!(telescode["env"]["TELESCODE_HEADLESS"], "/t/TelescodeHeadless");

        let allowed = &args[args.iter().position(|a| a == "--allowedTools").unwrap() + 1];
        assert!(allowed.contains("mcp__telescode__create_tour_draft"));
        assert!(!allowed.contains("render_approved_tour") && !allowed.contains("preview_tour_frame"));
    }

    // Has Claude Code write a real tour when TELESCODE_TEST_CLAUDE=1 and
    // TELESCODE_TEST_DB names an analyzed database (plus what the server needs:
    // Node.js and `npm --prefix mcp install`). A no-op otherwise: it uses the
    // developer's Claude Code sign-in and takes minutes.
    #[test]
    fn real_generation() {
        let (Some(_), Some(db)) = (std::env::var_os("TELESCODE_TEST_CLAUDE"), std::env::var_os("TELESCODE_TEST_DB")) else {
            return;
        };
        let db = db.into_string().unwrap();
        let before = mcp::call("list_tours", json!({})).unwrap().as_array().unwrap().len();
        generate(&db).unwrap();
        let tours = mcp::call("list_tours", json!({})).unwrap();
        assert_eq!(tours.as_array().unwrap().len(), before + 1);
    }
}
