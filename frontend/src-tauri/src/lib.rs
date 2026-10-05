mod bridge;
mod claude;
mod mcp;
mod repository;
mod tours;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
  tauri::Builder::default()
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
    .invoke_handler(tauri::generate_handler![bridge::run_headless, repository::open_repository, tours::list_tours, tours::generate_tour, claude::claude_status, claude::claude_sign_in])
    .run(tauri::generate_context!())
    .expect("error while running tauri application");
}
