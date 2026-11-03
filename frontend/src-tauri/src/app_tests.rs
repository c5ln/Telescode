//! Tests cross the actual desktop IPC handler, rather than launching a second
//! implementation of the Codex command line. Only the webview runtime is fake.
use serde_json::{json, Value};

fn ipc(
    window: &tauri::WebviewWindow<tauri::test::MockRuntime>,
    command: &str,
    body: Value,
) -> Result<Value, Value> {
    tauri::test::get_ipc_response(
        window,
        tauri::webview::InvokeRequest {
            cmd: command.into(),
            callback: tauri::ipc::CallbackFn(0),
            error: tauri::ipc::CallbackFn(1),
            url: if cfg!(windows) {
                "http://tauri.localhost"
            } else {
                "tauri://localhost"
            }
            .parse()
            .unwrap(),
            body: tauri::ipc::InvokeBody::Json(body),
            headers: Default::default(),
            invoke_key: tauri::test::INVOKE_KEY.into(),
        },
    )
    .map(|response| response.deserialize().unwrap())
}

fn window() -> (
    tauri::App<tauri::test::MockRuntime>,
    tauri::WebviewWindow<tauri::test::MockRuntime>,
) {
    let app = crate::with_commands(tauri::test::mock_builder())
        .build(tauri::test::mock_context(tauri::test::noop_assets()))
        .unwrap();
    let window = tauri::WebviewWindowBuilder::new(&app, "main", Default::default())
        .build()
        .unwrap();
    (app, window)
}

#[test]
fn ipc_rejects_unknown_providers_and_invalid_generation_paths() {
    let (_app, window) = window();
    assert!(ipc(&window, "agent_status", json!({"provider":"unknown"})).is_err());
    let error = ipc(
        &window,
        "generate_tour",
        json!({"provider":"codex", "dbPath":""}),
    )
    .unwrap_err();
    assert_eq!(error["code"], "invalid_argument");
}

#[test]
#[ignore = "Uses the saved Codex account and model quota; run alone with --ignored --exact"]
fn codex_generates_and_grades_through_app_ipc() {
    use std::time::{SystemTime, UNIX_EPOCH};
    let dir = std::env::temp_dir().join(format!(
        "telescode-app-codex-{}-{}",
        std::process::id(),
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    let source = dir.join("source with spaces");
    std::fs::create_dir_all(&source).unwrap();
    std::fs::write(source.join("helper.py"), "def transform(value):\n    if not value:\n        raise ValueError('value is required')\n    return value.strip().lower()\n").unwrap();
    std::fs::write(source.join("main.py"), "from helper import transform\n\nclass Application:\n    def run(self, value):\n        return transform(value)\n").unwrap();
    let db = dir.join("project with spaces.db");
    let headless = crate::bridge::locate_sidecar().unwrap();
    for args in [
        vec!["scan".as_ref(), source.as_os_str(), db.as_os_str()],
        vec!["algo".as_ref(), db.as_os_str()],
    ] {
        let output = std::process::Command::new(&headless)
            .args(args)
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "{}",
            String::from_utf8_lossy(&output.stderr)
        );
    }
    std::fs::write(
        dir.join("project with spaces.db.source"),
        source.to_str().unwrap(),
    )
    .unwrap();
    let output = dir.join("artifacts");
    // This ignored test runs alone: isolate the actual app's MCP artifact store.
    std::env::set_var("TELESCODE_TOUR_OUTPUT", &output);
    println!("App IPC artifacts: {}", dir.display());
    let (_app, window) = window();
    let status = ipc(&window, "agent_status", json!({"provider":"codex"})).unwrap();
    assert_eq!(
        status,
        json!({"installed":true,"signedIn":true}),
        "Sign in with Codex CLI first"
    );
    ipc(
        &window,
        "generate_tour",
        json!({"provider":"codex","dbPath":db}),
    )
    .unwrap();
    let tours = ipc(&window, "list_tours", json!({})).unwrap();
    assert_eq!(tours.as_array().unwrap().len(), 1);
    let tour = &tours[0];
    assert_eq!(tour["state"], "draft");
    assert!(tour["sourceSnapshotHash"].is_string());
    let id = tour["plan"]["id"].as_str().unwrap();
    let status = crate::mcp::call("get_tour_status", json!({"tourId":id})).unwrap();
    assert!(status["error"].is_null(), "{status}");
    let stops = tour["plan"]["stops"].as_array().unwrap();
    let index = stops
        .iter()
        .position(|stop| stop["nodeId"].as_str().unwrap().starts_with("file:"))
        .expect("A ranked file stop");
    let target = json!({"dbPath":db,"tourId":id,"tourRevision":tour["plan"]["revision"],"stopIndex":index,"nodeId":stops[index]["nodeId"]});
    ipc(
        &window,
        "generate_quiz",
        json!({"provider":"codex","target":target}),
    )
    .unwrap();
    let sets = ipc(&window, "list_quizzes", json!({})).unwrap();
    assert_eq!(sets.as_array().unwrap().len(), 1);
    let set = &sets[0];
    for field in ["tourId", "tourRevision", "stopIndex", "nodeId"] {
        assert_eq!(set[field], target[field]);
    }
    let questions = set["questions"].as_array().unwrap();
    assert!(!questions.is_empty());
    let question = &questions[0];
    let choice = question["choices"]
        .as_array()
        .unwrap()
        .iter()
        .position(|choice| choice == &question["answer"])
        .unwrap();
    let feedback = ipc(&window, "record_quiz_response", json!({"quizSetId":set["id"],"quizId":question["id"],"selectedIndex":choice,"responseTimeMs":1000})).unwrap();
    assert_eq!(feedback["feedback"]["correct"], true);
    std::env::remove_var("TELESCODE_TOUR_OUTPUT");
}
