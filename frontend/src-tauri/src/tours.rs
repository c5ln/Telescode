//! Saved code tours, for the tutorial mode.
//!
//! The Telescode MCP server (`mcp/`) writes every tour an agent drafts to
//! `<output>/<tour-id>.draft.json`, together with the snapshot it was written
//! against. This module only reads those files: it hands the frontend each
//! tour's plan and snapshot hash, never the snapshot itself, and never writes.

use serde::Serialize;
use serde_json::Value;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use crate::bridge::BridgeError;

/// Where the MCP server keeps its tours. The same variable configures the server.
const TOURS_ENV: &str = "TELESCODE_TOUR_OUTPUT";

const DRAFT_SUFFIX: &str = ".draft.json";

/// One saved tour, without the snapshot it carries.
#[derive(Debug, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct SavedTour {
    plan: Value,
    /// SHA-256 of the snapshot the tour was written against.
    snapshot_hash: String,
    /// The draft's review state: `draft`, `approved`, `rendering`, `completed` or `failed`.
    state: String,
    /// Last write, in milliseconds since the Unix epoch.
    updated_ms: u64,
}

/// `$TELESCODE_TOUR_OUTPUT` if set. Development builds fall back to the MCP
/// server's own default, `mcp/artifacts` in this repository.
pub fn tour_dir() -> Result<PathBuf, BridgeError> {
    match std::env::var_os(TOURS_ENV) {
        Some(p) if !p.is_empty() => Ok(PathBuf::from(p)),
        _ if cfg!(debug_assertions) => Ok(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../mcp/artifacts")),
        _ => Err(BridgeError::ToursUnavailable {
            message: format!("Set {TOURS_ENV} to the Telescode MCP server's tour directory."),
        }),
    }
}

/// Every readable tour in `dir`. A missing directory has no tours; a file that
/// is not a tour draft is skipped rather than failing the whole list.
pub fn read_tours(dir: &Path) -> Result<Vec<SavedTour>, BridgeError> {
    let entries = match std::fs::read_dir(dir) {
        Ok(entries) => entries,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => {
            return Err(BridgeError::ToursUnavailable {
                message: format!("Cannot read tours in {}: {e}", dir.display()),
            })
        }
    };

    let mut tours = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        if !path.to_string_lossy().ends_with(DRAFT_SUFFIX) {
            continue;
        }
        if let Some(tour) = read_tour(&path) {
            tours.push(tour);
        }
    }
    Ok(tours)
}

fn read_tour(path: &Path) -> Option<SavedTour> {
    let text = std::fs::read_to_string(path).ok()?;
    let mut draft: Value = serde_json::from_str(&text).ok()?;
    let plan = draft.get_mut("plan").filter(|p| p.is_object())?.take();
    let snapshot_hash = draft.pointer("/project/hash")?.as_str()?.to_string();
    let state = draft.get("state").and_then(Value::as_str).unwrap_or("draft").to_string();
    let updated_ms = std::fs::metadata(path)
        .and_then(|m| m.modified())
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map_or(0, |d| d.as_millis() as u64);
    Some(SavedTour { plan, snapshot_hash, state, updated_ms })
}

/// IPC entry point: `invoke("list_tours")`.
#[tauri::command]
pub async fn list_tours() -> Result<Vec<SavedTour>, BridgeError> {
    tauri::async_runtime::spawn_blocking(|| read_tours(&tour_dir()?))
        .await
        .map_err(|e| BridgeError::Internal {
            message: format!("Tour worker failed: {e}"),
        })?
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("telescode-tours-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn missing_directory_has_no_tours() {
        let dir = temp_dir("missing").join("absent");
        assert_eq!(read_tours(&dir).unwrap(), Vec::new());
        assert!(!dir.exists());
    }

    #[test]
    fn reads_drafts_without_their_snapshot() {
        let dir = temp_dir("drafts");
        let draft = r#"{
            "plan": { "id": "t1", "title": "Tour", "stops": [] },
            "project": { "id": "p1", "hash": "abc", "snapshot": { "files": [] } },
            "reviewToken": "secret",
            "state": "approved"
        }"#;
        std::fs::write(dir.join("t1.draft.json"), draft).unwrap();
        std::fs::write(dir.join("broken.draft.json"), "{ not json").unwrap();
        std::fs::write(dir.join("notes.json"), r#"{ "plan": {} }"#).unwrap();

        let tours = read_tours(&dir).unwrap();
        assert_eq!(tours.len(), 1);
        let v = serde_json::to_value(&tours[0]).unwrap();
        assert_eq!(v["plan"]["id"], "t1");
        assert_eq!(v["snapshotHash"], "abc");
        assert_eq!(v["state"], "approved");
        assert!(v["updatedMs"].as_u64().unwrap() > 0);
        assert!(v.get("project").is_none() && v.get("reviewToken").is_none());
    }
}
