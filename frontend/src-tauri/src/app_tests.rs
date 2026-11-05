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
fn learning_commands_are_registered_and_validate_paths_before_starting_work() {
    let (_app, window) = window();
    let error = ipc(&window, "start_learning_session", json!({"dbPath":"", "goal":{
        "goalId":"state", "goalRevision":1, "rawQuery":"Understand state", "retrievalQuery":"state",
        "targetConcepts":[{"conceptId":"state","level":"easy","weight":1}],"budget":4
    }})).unwrap_err();
    assert_eq!(error["code"], "invalid_argument", "{error}");
}

fn learning_fixture() -> (std::path::PathBuf, std::path::PathBuf) {
    use std::time::{SystemTime, UNIX_EPOCH};
    let dir = std::env::temp_dir().join(format!("telescode-learning-ipc-{}-{}", std::process::id(), SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos()));
    let source = dir.join("source with spaces"); std::fs::create_dir_all(&source).unwrap();
    std::fs::write(source.join("zygote.py"), "class Zygote:\n    def __init__(self, omen):\n        self.omen = omen\n").unwrap();
    let db = dir.join("learning.db");
    let headless = crate::bridge::locate_sidecar().unwrap();
    for args in [vec!["scan".as_ref(), source.as_os_str(), db.as_os_str()], vec!["algo".as_ref(), db.as_os_str()]] {
        let output = std::process::Command::new(&headless).args(args).output().unwrap();
        assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    }
    std::fs::write(dir.join("learning.db.source"), source.to_str().unwrap()).unwrap();
    std::env::set_var("TELESCODE_TOUR_OUTPUT", dir.join("artifacts"));
    (dir, db)
}

fn learning_goal() -> Value {
    json!({"goalId":"zygote-state","goalRevision":1,"rawQuery":"Explain why Zygote keeps omen on each instance", "retrievalQuery":"Zygote omen",
        "targetConcepts":[{"conceptId":"zygote-state","level":"easy","weight":1}],"budget":4})
}

#[test]
#[ignore = "Uses local CPU sidecars and installed MCP runtime; run alone with --ignored --exact"]
fn learning_session_round_trip_through_registered_ipc() {
    let (_dir, db) = learning_fixture();
    let (_app, window) = window();
    let started = ipc(&window, "start_learning_session", json!({"dbPath":db,"goal":learning_goal()})).unwrap();
    assert_eq!(started["session"]["status"], "preparing");
    assert_eq!(started["session"]["goal"]["retrievalQuery"], "Zygote omen");
    let id = started["session"]["id"].as_str().unwrap();
    let job = crate::mcp::call("request_learning_preparation", json!({"sessionId":id,"revision":started["session"]["revision"],"provider":"codex","ownerPid":std::process::id()})).unwrap();
    crate::mcp::call("get_learning_preparation_context", json!({"jobId":job["id"]})).unwrap();
    let q = |suffix: &str| json!({"type":"multiple_choice","difficulty":"easy","concept":"Instance state",
        "question":format!("Why preserve omen on the instance? {suffix}"), "answer":"So later instance operations can access the supplied value",
        "distractors":["To send a network request","To change all instances at once"],"explanation":"The initializer stores the supplied value in self.omen.","evidenceNodeIds":["file:zygote.py"]});
    crate::mcp::call("save_learning_curriculum", json!({"jobId":job["id"],"curriculum":{"version":1,
        "concepts":[{"id":"zygote-state","name":"Instance state","definition":"Explain how the initializer preserves instance-specific input","version":1}],
        "items":[{"id":"read-state","kind":"read","nodeId":"file:zygote.py","conceptId":"zygote-state","level":"easy","caption":"Follow the initializer assignment.","questions":[q("one instance"),q("two instances")]}],"relations":[]}})).unwrap();
    let ready = ipc(&window, "get_learning_session", json!({"sessionId":id})).unwrap();
    assert_eq!(ready["session"]["status"], "active");
    assert!(ready["question"].get("answer").is_none());
    let answer = json!({"sessionId":id,"revision":ready["session"]["revision"],"stopId":ready["session"]["current"]["id"],
        "submissionId":"ipc-answer","questionId":ready["question"]["id"],"selectedIndex":0});
    let result = ipc(&window, "record_learning_response", json!({"answer":answer})).unwrap();
    assert_eq!(result["feedback"]["correct"], ready["question"]["choices"][0] == result["feedback"]["answer"]);
    assert_eq!(ipc(&window,"record_learning_response",json!({"answer":answer})).unwrap(),result);
    assert_eq!(result["view"]["session"]["current"]["id"],ready["session"]["current"]["id"]);
    let stopped = ipc(&window,"advance_learning_session",json!({"input":{"sessionId":id,"revision":result["view"]["session"]["revision"],
        "stopId":ready["session"]["current"]["id"],"outcome":"stop"}})).unwrap();
    assert_eq!(stopped["session"]["status"],"stopped");
    std::env::remove_var("TELESCODE_TOUR_OUTPUT");
}

#[cfg(unix)]
#[test]
#[ignore = "Uses local CPU MCP and a fake external CLI process; run alone with --ignored --exact"]
fn concurrent_learning_preparation_joins_one_worker_through_registered_ipc() {
    use std::os::unix::fs::PermissionsExt;
    use std::time::{Duration, Instant};
    let (dir, db) = learning_fixture();
    let (app, first_window) = window();
    let second_window = tauri::WebviewWindowBuilder::new(&app,"second",Default::default()).build().unwrap();
    let started = ipc(&first_window,"start_learning_session",json!({"dbPath":db,"goal":learning_goal()})).unwrap();
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").canonicalize().unwrap();
    let node = crate::mcp::node();
    let loader = root.join("mcp/node_modules/tsx/dist/loader.mjs");
    let log = dir.join("worker-runs");
    let fake = dir.join("fake-codex");
    let worker = dir.join("fake-codex.mjs");
    let output = dir.join("artifacts");
    let script = format!(r##"import {{ appendFileSync }} from 'node:fs';
import {{ TourStore }} from '{root}/mcp/src/store.ts';
import {{ QuizStore }} from '{root}/mcp/src/quiz.ts';
import {{ LearningStore }} from '{root}/mcp/src/learning.ts';
const jobId = /job ([\w-]+)/.exec(process.argv.join(' '))[1];
appendFileSync({log}, 'worker\n');
const tours = new TourStore({output});
const learning = new LearningStore({output}, tours, new QuizStore({output}, tours));
await learning.jobContext(jobId);
await new Promise(resolve => setTimeout(resolve, 750));
await learning.saveJobCurriculum(jobId, {{version:1,concepts:[{{id:'zygote-state',name:'Instance state',definition:'Explain instance-specific omen storage',version:1}}],relations:[],items:[{{id:'read-omen',kind:'read',nodeId:'file:zygote.py',conceptId:'zygote-state',level:'easy',cost:1,caption:'Read the instance assignment.',effects:[{{conceptId:'zygote-state',level:'easy',learn:.6}}],questions:[{{type:'multiple_choice',difficulty:'easy',concept:'Instance state',question:'Why preserve omen on this instance?',answer:'To retain the supplied instance value',distractors:['To send a request','To mutate every instance'],explanation:'self.omen stores the supplied value.',evidenceNodeIds:['file:zygote.py']}}]}}]}});
learning.close();
"##, root=root.display(),log=serde_json::to_string(&log).unwrap(),output=serde_json::to_string(&output).unwrap());
    std::fs::write(&worker,script).unwrap();
    // A shell wrapper, because the kernel does not search PATH for a bare `node` shebang interpreter.
    std::fs::write(&fake,format!("#!/bin/sh\nexec '{}' --import='{}' '{}' \"$@\"\n",node.to_string_lossy(),loader.display(),worker.display())).unwrap();
    std::fs::set_permissions(&fake,std::fs::Permissions::from_mode(0o755)).unwrap();
    std::env::set_var("TELESCODE_CODEX",&fake);
    let input = json!({"sessionId":started["session"]["id"],"revision":started["session"]["revision"],"provider":"codex"});
    std::thread::scope(|scope| {
        let left = scope.spawn(|| ipc(&first_window,"prepare_learning_session",input.clone()));
        let waiting = Instant::now();
        while !log.exists() && waiting.elapsed() < Duration::from_secs(10) { std::thread::sleep(Duration::from_millis(20)); }
        if !log.exists() {
            panic!("fake worker must start before the second client joins; first prepare returned {:?}", left.join().unwrap());
        }
        let right = scope.spawn(|| ipc(&second_window,"prepare_learning_session",input.clone()));
        let a = left.join().unwrap().unwrap();
        let b = right.join().unwrap().unwrap();
        assert_eq!(a["session"]["status"],"active");
        assert_eq!(b["session"]["status"],"active");
        assert_eq!(a["session"]["id"],b["session"]["id"]);
    });
    assert_eq!(std::fs::read_to_string(&log).unwrap().lines().count(),1,"only one CLI process may prepare the joined job");
    std::env::remove_var("TELESCODE_CODEX");
    std::env::remove_var("TELESCODE_TOUR_OUTPUT");
}

#[test]
#[ignore = "Uses the logged-in Codex account and quota; run alone with --ignored --exact"]
fn codex_prepares_adaptive_learning_through_app_ipc() {
    let (dir, db) = learning_fixture();
    println!("Learning IPC artifacts: {}", dir.display());
    let (_app, window) = window();
    assert_eq!(ipc(&window,"agent_status",json!({"provider":"codex"})).unwrap(),json!({"installed":true,"signedIn":true}));
    let started = ipc(&window,"start_learning_session",json!({"dbPath":db,"goal":learning_goal()})).unwrap();
    let prepared = ipc(&window,"prepare_learning_session",json!({"sessionId":started["session"]["id"],"revision":started["session"]["revision"],"provider":"codex"})).unwrap();
    assert_eq!(prepared["session"]["status"],"active","{prepared}");
    assert!(prepared["question"]["choices"].is_array(),"{prepared}");
    assert!(prepared["question"].get("answer").is_none());
    let answer = json!({"sessionId":prepared["session"]["id"],"revision":prepared["session"]["revision"],"stopId":prepared["session"]["current"]["id"],
        "submissionId":"account-smoke-answer","questionId":prepared["question"]["id"],"selectedIndex":0});
    let response = ipc(&window,"record_learning_response",json!({"answer":answer})).unwrap();
    assert_eq!(response["feedback"]["correct"],prepared["question"]["choices"][0] == response["feedback"]["answer"]);
    assert_eq!(ipc(&window,"record_learning_response",json!({"answer":answer})).unwrap(),response);
    assert_eq!(response["view"]["session"]["current"]["id"],prepared["session"]["current"]["id"]);
    let next = ipc(&window,"advance_learning_session",json!({"input":{"sessionId":prepared["session"]["id"],"revision":response["view"]["session"]["revision"],
        "stopId":prepared["session"]["current"]["id"],"outcome":"continue"}})).unwrap();
    assert!(next["session"]["spent"].as_f64().unwrap() > 0.0);
    std::env::remove_var("TELESCODE_TOUR_OUTPUT");
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
