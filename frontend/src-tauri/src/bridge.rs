//! The one place the desktop shell talks to the C++ core.
//!
//! The frontend asks for a whole operation (`graph`, `sequence` or `analyze` on
//! one database) and gets back the core's JSON exactly as TelescodeHeadless
//! printed it. Nothing here reads the database or interprets the analysis: the
//! checks below only decide whether it is safe to hand a path to the core.
//!
//! The core is run read-only (`--algo` is never passed), so a request can never
//! create or modify a database.

use serde::Serialize;
use std::fs::File;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::Command;

/// Name of the sidecar as it sits next to the app executable. Tauri copies
/// `binaries/TelescodeHeadless-<target-triple>` there at build time and strips
/// the triple.
const SIDECAR_NAME: &str = "TelescodeHeadless";

/// Overrides the sidecar location, for running against a CMake build directly.
const SIDECAR_ENV: &str = "TELESCODE_HEADLESS";

/// First 16 bytes of every SQLite 3 database file.
const SQLITE_MAGIC: &[u8; 16] = b"SQLite format 3\0";

/// The read-only operations the frontend may request. Deserialized from a
/// fixed set of strings, so the webview cannot smuggle in another subcommand.
#[derive(Debug, Clone, Copy, serde::Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Operation {
    Graph,
    Sequence,
    Analyze,
}

impl Operation {
    fn subcommand(self) -> &'static str {
        match self {
            Operation::Graph => "graph",
            Operation::Sequence => "sequence",
            Operation::Analyze => "analyze",
        }
    }
}

/// Every failure the bridge reports. Serialized as
/// `{ "code": "db_not_found", "message": "...", ... }` so the frontend can branch on
/// `code` rather than parsing prose.
#[derive(Debug, Serialize, PartialEq)]
#[serde(tag = "code", rename_all = "snake_case", rename_all_fields = "camelCase")]
pub enum BridgeError {
    InvalidArgument { message: String },
    DbNotFound { message: String, path: String },
    DbNotAFile { message: String, path: String },
    DbInvalid { message: String, path: String },
    SidecarMissing { message: String, path: String },
    SidecarSpawnFailed { message: String },
    SidecarFailed { message: String, exit_code: Option<i32>, stderr: String },
    OutputNotUtf8 { message: String },
    Internal { message: String },
}

/// Checks that `raw` names an existing SQLite file and returns its absolute
/// path. Absolute, so it can never be mistaken for a `-flag` by the core's
/// argument parser.
pub fn validate_db_path(raw: &str) -> Result<PathBuf, BridgeError> {
    let trimmed = raw.trim();
    if trimmed.is_empty() {
        return Err(BridgeError::InvalidArgument {
            message: "A database path is required.".into(),
        });
    }

    let path = std::path::absolute(trimmed).map_err(|e| BridgeError::InvalidArgument {
        message: format!("Cannot resolve database path: {e}"),
    })?;
    let shown = path.display().to_string();

    let meta = match std::fs::metadata(&path) {
        Ok(m) => m,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            return Err(BridgeError::DbNotFound {
                message: format!("Database not found: {shown}"),
                path: shown,
            })
        }
        Err(e) => {
            return Err(BridgeError::DbInvalid {
                message: format!("Cannot read database {shown}: {e}"),
                path: shown,
            })
        }
    };
    if !meta.is_file() {
        return Err(BridgeError::DbNotAFile {
            message: format!("Not a file: {shown}"),
            path: shown,
        });
    }

    // The core reports a non-SQLite file as an empty analysis rather than an
    // error, so catch that here before it reaches the UI looking like a result.
    let mut header = [0u8; 16];
    let read_ok = File::open(&path)
        .and_then(|mut f| f.read_exact(&mut header))
        .is_ok();
    if !read_ok || &header != SQLITE_MAGIC {
        return Err(BridgeError::DbInvalid {
            message: format!("Not a SQLite database: {shown}"),
            path: shown,
        });
    }

    Ok(path)
}

/// Where the sidecar is expected: `$TELESCODE_HEADLESS` if set, otherwise next
/// to the running executable.
pub fn locate_sidecar() -> Result<PathBuf, BridgeError> {
    let path = match std::env::var_os(SIDECAR_ENV) {
        Some(p) if !p.is_empty() => PathBuf::from(p),
        _ => {
            let exe = std::env::current_exe().map_err(|e| BridgeError::Internal {
                message: format!("Cannot locate the application executable: {e}"),
            })?;
            let dir = exe.parent().unwrap_or(Path::new("."));
            dir.join(format!("{SIDECAR_NAME}{}", std::env::consts::EXE_SUFFIX))
        }
    };

    check_sidecar(path)
}

fn check_sidecar(path: PathBuf) -> Result<PathBuf, BridgeError> {
    if path.is_file() {
        Ok(path)
    } else {
        let shown = path.display().to_string();
        Err(BridgeError::SidecarMissing {
            message: format!("TelescodeHeadless not found at {shown}"),
            path: shown,
        })
    }
}

/// Runs one operation to completion and returns the core's stdout.
pub fn run(sidecar: &Path, op: Operation, db_path: &str) -> Result<String, BridgeError> {
    let db = validate_db_path(db_path)?;

    let mut cmd = Command::new(sidecar);
    cmd.arg(op.subcommand()).arg(&db);
    #[cfg(windows)]
    {
        // Keep a console window from flashing up for every request.
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }

    let out = cmd.output().map_err(|e| BridgeError::SidecarSpawnFailed {
        message: format!("Could not start TelescodeHeadless: {e}"),
    })?;

    if !out.status.success() {
        let stderr = String::from_utf8_lossy(&out.stderr).trim().to_string();
        let message = if stderr.is_empty() {
            format!("TelescodeHeadless exited with {}", out.status)
        } else {
            stderr.clone()
        };
        return Err(BridgeError::SidecarFailed {
            message,
            exit_code: out.status.code(),
            stderr,
        });
    }

    String::from_utf8(out.stdout).map_err(|_| BridgeError::OutputNotUtf8 {
        message: "TelescodeHeadless produced output that is not UTF-8.".into(),
    })
}

/// IPC entry point: `invoke("run_headless", { op, dbPath })`.
///
/// Runs on a blocking worker so a slow analysis never stalls the UI thread.
#[tauri::command]
pub async fn run_headless(op: Operation, db_path: String) -> Result<String, BridgeError> {
    tauri::async_runtime::spawn_blocking(move || {
        let sidecar = locate_sidecar()?;
        run(&sidecar, op, &db_path)
    })
    .await
    .map_err(|e| BridgeError::Internal {
        message: format!("Bridge worker failed: {e}"),
    })?
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn temp_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("telescode-bridge-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn empty_path_is_rejected() {
        assert!(matches!(validate_db_path("  "), Err(BridgeError::InvalidArgument { .. })));
    }

    #[test]
    fn missing_file_is_not_created() {
        let dir = temp_dir("missing");
        let db = dir.join("nope.db");
        let err = validate_db_path(db.to_str().unwrap()).unwrap_err();
        assert!(matches!(err, BridgeError::DbNotFound { .. }));
        assert!(!db.exists());
    }

    #[test]
    fn directory_is_rejected() {
        let dir = temp_dir("dir");
        let err = validate_db_path(dir.to_str().unwrap()).unwrap_err();
        assert!(matches!(err, BridgeError::DbNotAFile { .. }));
    }

    #[test]
    fn non_sqlite_file_is_rejected() {
        let dir = temp_dir("garbage");
        let db = dir.join("bad.db");
        std::fs::File::create(&db).unwrap().write_all(b"not a database at all").unwrap();
        let err = validate_db_path(db.to_str().unwrap()).unwrap_err();
        assert!(matches!(err, BridgeError::DbInvalid { .. }));
    }

    #[test]
    fn sqlite_header_is_accepted_and_made_absolute() {
        let dir = temp_dir("ok");
        let db = dir.join("ok.db");
        let mut bytes = SQLITE_MAGIC.to_vec();
        bytes.extend_from_slice(&[0u8; 84]);
        std::fs::write(&db, bytes).unwrap();
        let p = validate_db_path(db.to_str().unwrap()).unwrap();
        assert!(p.is_absolute());
    }

    #[test]
    fn errors_serialize_with_a_code() {
        let e = BridgeError::SidecarFailed {
            message: "boom".into(),
            exit_code: Some(1),
            stderr: "boom".into(),
        };
        let v = serde_json::to_value(&e).unwrap();
        assert_eq!(v["code"], "sidecar_failed");
        assert_eq!(v["exitCode"], 1);
    }

    #[test]
    fn missing_sidecar_is_reported() {
        let dir = temp_dir("nosidecar");
        let err = check_sidecar(dir.join("absent.exe")).unwrap_err();
        assert!(matches!(err, BridgeError::SidecarMissing { .. }));
    }

    // Runs the real TelescodeHeadless when both variables are set, e.g.
    //   TELESCODE_HEADLESS=<build>/Release/TelescodeHeadless.exe
    //   TELESCODE_TEST_DB=<a scanned database>
    // and is a no-op otherwise, so `cargo test` works without a C++ build.
    #[test]
    fn real_sidecar_round_trip() {
        let (Some(bin), Some(db)) = (
            std::env::var_os(SIDECAR_ENV),
            std::env::var_os("TELESCODE_TEST_DB"),
        ) else {
            return;
        };
        let bin = PathBuf::from(bin);
        let db = db.into_string().unwrap();

        let out = run(&bin, Operation::Analyze, &db).unwrap();
        let v: serde_json::Value = serde_json::from_str(&out).unwrap();
        assert_eq!(v["schemaVersion"], 1);
        assert!(v["totals"]["fileCount"].is_number());

        let missing = format!("{db}.does-not-exist");
        let err = run(&bin, Operation::Analyze, &missing).unwrap_err();
        assert!(matches!(err, BridgeError::DbNotFound { .. }));
        assert!(!Path::new(&missing).exists());
    }
}
