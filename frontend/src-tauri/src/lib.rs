mod agent;
mod codex;
mod bridge;
mod claude;
mod mcp;
mod quizzes;
mod repository;
mod tours;
mod learning;
#[cfg(test)]
mod app_tests;

// The desktop and IPC tests use exactly the same command registration.
fn with_commands<R: tauri::Runtime>(builder: tauri::Builder<R>) -> tauri::Builder<R> {
  builder.invoke_handler(tauri::generate_handler![bridge::run_headless, repository::open_repository, repository::open_folder, tours::list_tours, tours::generate_tour, agent::agent_status, agent::agent_sign_in, quizzes::list_quizzes, quizzes::generate_quiz, quizzes::record_quiz_response, learning::start_learning_session, learning::get_learning_session, learning::prepare_learning_session, learning::record_learning_response, learning::advance_learning_session])
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
  with_commands(tauri::Builder::default())
    .setup(|app| {
      if cfg!(debug_assertions) {
        app.handle().plugin(
          tauri_plugin_log::Builder::default()
            .level(log::LevelFilter::Info)
            .build(),
        )?;
      }
      Ok(())
    })
    .run(tauri::generate_context!())
    .expect("error while running tauri application");
}
