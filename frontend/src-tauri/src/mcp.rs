//! The desktop shell's connection to the Telescode MCP server (`mcp/`).
//!
//! The shell starts the server as a child process and speaks the MCP stdio
//! transport to it: newline-delimited JSON-RPC on stdin/stdout. It is a
//! client like any agent, so tours are read and written only through the
//! server's own tools.

use serde_json::{json, Value};
use std::ffi::OsString;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};

use crate::bridge::{locate_sidecar, BridgeError};

/// Where the server lives (the directory holding `src/server.ts`).
const MCP_DIR_ENV: &str = "TELESCODE_MCP_DIR";
/// The Node.js executable that runs it. Desktop apps started from the Finder
/// do not see the shell's PATH, so this is how to point at a specific one.
const NODE_ENV: &str = "TELESCODE_NODE";

const PROTOCOL_VERSION: &str = "2025-06-18";

/// `$TELESCODE_MCP_DIR` if set. Development builds fall back to `mcp/` in
/// this repository.
pub fn mcp_dir() -> Result<PathBuf, BridgeError> {
    let dir = match std::env::var_os(MCP_DIR_ENV) {
        Some(p) if !p.is_empty() => PathBuf::from(p),
        _ if cfg!(debug_assertions) => Path::new(env!("CARGO_MANIFEST_DIR")).join("../../mcp"),
        _ => {
            return Err(BridgeError::ToursUnavailable {
                message: format!("Set {MCP_DIR_ENV} to the Telescode MCP server directory."),
            })
        }
    };
    let dir = std::path::absolute(&dir).unwrap_or(dir);
    if !dir.join("node_modules/tsx").is_dir() {
        return Err(BridgeError::ToursUnavailable {
            message: format!(
                "The Telescode MCP server at {} is not installed. Run `npm --prefix mcp install`.",
                dir.display()
            ),
        });
    }
    Ok(dir)
}

pub fn node() -> OsString {
    std::env::var_os(NODE_ENV).filter(|p| !p.is_empty()).unwrap_or_else(|| "node".into())
}

/// How to start the server: the program, its arguments and environment.
/// Absolute paths only, so it runs from any working directory, as it must
/// when an agent starts it.
pub struct ServerCommand {
    pub program: OsString,
    pub args: Vec<OsString>,
    pub env: Vec<(String, OsString)>,
}

pub fn server_command() -> Result<ServerCommand, BridgeError> {
    let dir = mcp_dir()?;
    // `--import tsx` resolves from the working directory; the loader's own
    // path does not.
    let loader = dir.join("node_modules/tsx/dist/loader.mjs");
    let mut env = Vec::new();
    // The server analyzes a database the same way the app does, with the
    // same core, so both see the same snapshot.
    if let Ok(sidecar) = locate_sidecar() {
        env.push(("TELESCODE_HEADLESS".to_string(), sidecar.into_os_string()));
    }
    if let Some(output) = std::env::var_os("TELESCODE_TOUR_OUTPUT").filter(|p| !p.is_empty()) {
        env.push(("TELESCODE_TOUR_OUTPUT".to_string(), output));
    }
    Ok(ServerCommand {
        program: node(),
        args: vec!["--import".into(), loader.into_os_string(), dir.join("src/server.ts").into_os_string()],
        env,
    })
}

/// A running server and the client end of its stdio.
pub struct McpServer {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
    next_id: u64,
}

impl McpServer {
    /// Starts the server and completes the MCP initialization handshake.
    pub fn start(command: &ServerCommand) -> Result<Self, BridgeError> {
        let mut cmd = Command::new(&command.program);
        cmd.args(&command.args)
            .envs(command.env.iter().map(|(k, v)| (k, v)))
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            const CREATE_NO_WINDOW: u32 = 0x0800_0000;
            cmd.creation_flags(CREATE_NO_WINDOW);
        }
        let mut child = cmd.spawn().map_err(|e| BridgeError::ToursUnavailable {
            message: format!("Could not start the Telescode MCP server with {:?}: {e}", command.program),
        })?;
        let stdin = child.stdin.take().expect("piped stdin");
        let stdout = BufReader::new(child.stdout.take().expect("piped stdout"));
        let mut server = McpServer { child, stdin, stdout, next_id: 1 };

        server.request(
            "initialize",
            json!({
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": { "name": "telescode-desktop", "version": env!("CARGO_PKG_VERSION") },
            }),
        )?;
        server.send(&json!({ "jsonrpc": "2.0", "method": "notifications/initialized" }))?;
        Ok(server)
    }

    /// Calls a tool and returns the JSON its text content carries.
    pub fn call_tool(&mut self, name: &str, arguments: Value) -> Result<Value, BridgeError> {
        let result = self.request("tools/call", json!({ "name": name, "arguments": arguments }))?;
        tool_output(name, &result)
    }

    fn send(&mut self, message: &Value) -> Result<(), BridgeError> {
        let mut line = message.to_string();
        line.push('\n');
        self.stdin
            .write_all(line.as_bytes())
            .and_then(|_| self.stdin.flush())
            .map_err(|e| lost(format!("Cannot write to the Telescode MCP server: {e}")))
    }

    fn request(&mut self, method: &str, params: Value) -> Result<Value, BridgeError> {
        let id = self.next_id;
        self.next_id += 1;
        self.send(&json!({ "jsonrpc": "2.0", "id": id, "method": method, "params": params }))?;
        loop {
            let mut line = String::new();
            let read = self
                .stdout
                .read_line(&mut line)
                .map_err(|e| lost(format!("Cannot read from the Telescode MCP server: {e}")))?;
            if read == 0 {
                return Err(lost("The Telescode MCP server stopped.".into()));
            }
            // Notifications and requests from the server carry no matching id.
            let Ok(message) = serde_json::from_str::<Value>(&line) else { continue };
            if message.get("id").and_then(Value::as_u64) != Some(id) || message.get("method").is_some() {
                continue;
            }
            if let Some(error) = message.get("error") {
                return Err(BridgeError::ToursUnavailable {
                    message: format!("The Telescode MCP server refused {method}: {}", error["message"]),
                });
            }
            return Ok(message.get("result").cloned().unwrap_or(Value::Null));
        }
    }
}

impl Drop for McpServer {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn lost(message: String) -> BridgeError {
    BridgeError::ToursUnavailable { message }
}

/// The JSON a tool returned, or its error. Telescode tools answer with one
/// text item holding JSON, and with `isError` and a message on failure.
pub fn tool_output(name: &str, result: &Value) -> Result<Value, BridgeError> {
    let text = result["content"]
        .as_array()
        .and_then(|items| items.iter().find(|c| c["type"] == "text"))
        .and_then(|c| c["text"].as_str())
        .unwrap_or_default();
    if result["isError"] == true {
        return Err(BridgeError::ToursUnavailable {
            message: format!("Telescode MCP tool {name} failed: {text}"),
        });
    }
    serde_json::from_str(text).map_err(|_| BridgeError::ToursUnavailable {
        message: format!("Telescode MCP tool {name} returned an unexpected result."),
    })
}

/// The one server the shell keeps, started on first use and again after it
/// stops.
static SERVER: std::sync::Mutex<Option<McpServer>> = std::sync::Mutex::new(None);

/// Calls a tool on the shell's server. Blocking: run it off the UI thread.
pub fn call(name: &str, arguments: Value) -> Result<Value, BridgeError> {
    let mut slot = SERVER.lock().unwrap_or_else(|e| e.into_inner());
    if slot.is_none() {
        *slot = Some(McpServer::start(&server_command()?)?);
    }
    let result = slot.as_mut().expect("started").call_tool(name, arguments.clone());
    match result {
        // A server that went away is started again once.
        Err(_) if slot.as_mut().is_some_and(|s| s.child.try_wait().ok().flatten().is_some()) => {
            *slot = Some(McpServer::start(&server_command()?)?);
            slot.as_mut().expect("started").call_tool(name, arguments)
        }
        other => other,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tool_output_is_the_json_in_the_text_item() {
        let result = json!({ "content": [{ "type": "text", "text": "[{\"plan\":{}}]" }] });
        assert_eq!(tool_output("list_tours", &result).unwrap(), json!([{ "plan": {} }]));
    }

    #[test]
    fn tool_errors_are_reported_with_their_message() {
        let result = json!({ "isError": true, "content": [{ "type": "text", "text": "Error: Tour not found" }] });
        let err = tool_output("get_tour_status", &result).unwrap_err();
        let v = serde_json::to_value(&err).unwrap();
        assert_eq!(v["code"], "tours_unavailable");
        assert!(v["message"].as_str().unwrap().contains("Tour not found"));
    }

    // Starts the real server when TELESCODE_TEST_MCP=1 and `npm --prefix mcp
    // install` has run (and Node.js is on PATH or in TELESCODE_NODE), and is a
    // no-op otherwise, so `cargo test` works without Node.
    #[test]
    fn real_server_round_trip() {
        if std::env::var_os("TELESCODE_TEST_MCP").is_none() {
            return;
        }
        let output = std::env::temp_dir().join(format!("telescode-mcp-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&output);
        let mut command = server_command().unwrap();
        command.env.push(("TELESCODE_TOUR_OUTPUT".into(), output.clone().into_os_string()));
        let mut server = McpServer::start(&command).unwrap();

        assert_eq!(server.call_tool("list_tours", json!({})).unwrap(), json!([]));
        let fixture = Path::new(env!("CARGO_MANIFEST_DIR")).join("../src/graph/fixtures/sherlock.graph.json");
        let project = server.call_tool("open_project", json!({ "path": fixture })).unwrap();
        let stops = json!([{ "nodeId": "root", "caption": "Overview", "transitionMs": 0, "holdMs": 2000 }]);
        server
            .call_tool(
                "create_tour_draft",
                json!({ "projectId": project["projectId"], "title": "Test", "language": "en", "stops": stops }),
            )
            .unwrap();
        let tours = server.call_tool("list_tours", json!({})).unwrap();
        assert_eq!(tours[0]["plan"]["title"], "Test");
        assert_eq!(tours[0]["snapshotHash"], project["snapshotHash"]);
        assert!(server.call_tool("get_tour_status", json!({ "tourId": "missing" })).is_err());
    }
}
