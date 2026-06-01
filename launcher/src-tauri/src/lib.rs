use std::process::{Child, Command};
use std::sync::Mutex;
use std::path::PathBuf;
use std::fs;
use tauri::{State, Manager};

// Thread-safe state to manage active subprocesses
struct ActiveSession {
    proxy_child: Option<Child>,
    dolphin_child: Option<Child>,
}

struct AppState(Mutex<ActiveSession>);

#[derive(serde::Serialize, serde::Deserialize, Clone)]
struct SystemStatus {
    proxy_found: bool,
    dolphin_found: bool,
    session_active: bool,
}

#[derive(serde::Serialize, serde::Deserialize, Clone)]
struct SessionPayload {
    iso_path: String,
    dolphin_path: String,
    proxy_path: String,
    is_pitching_team: bool,
    server_ip: String,
}

// Intercepts and initializes Dolphin isolation settings
fn configure_isolated_dolphin(dolphin_dir: &PathBuf) -> Result<(), String> {
    // 1. Create portable.txt to force Dolphin into portable mode
    let portable_file = dolphin_dir.join("portable.txt");
    if !portable_file.exists() {
        fs::write(&portable_file, "").map_err(|e| format!("Failed to create portable.txt: {}", e))?;
    }

    // 2. Create User/Config directory
    let config_dir = dolphin_dir.join("User/Config");
    fs::create_dir_all(&config_dir).map_err(|e| format!("Failed to create isolated User/Config folder: {}", e))?;

    // 3. Write default Dolphin.ini for low-latency netplay configurations
    let ini_file = config_dir.join("Dolphin.ini");
    let ini_content = "[Interface]\n\
                       ConfirmStop = False\n\
                       UsePanicHandlers = False\n\
                       [Core]\n\
                       GFXBackend = Vulkan\n\
                       EnableCheats = True\n\
                       [NetPlay]\n\
                       ConnectPort = 5556\n\
                       ChunkedCode = True\n";
    
    fs::write(&ini_file, ini_content).map_err(|e| format!("Failed to write isolated Dolphin.ini: {}", e))?;

    Ok(())
}

#[tauri::command]
fn check_system_status(
    state: State<'_, AppState>,
    dolphin_path: String,
    proxy_path: String
) -> SystemStatus {
    let session = state.0.lock().unwrap();
    
    let d_path = PathBuf::from(&dolphin_path);
    let p_path = PathBuf::from(&proxy_path);

    SystemStatus {
        proxy_found: p_path.exists(),
        dolphin_found: d_path.exists(),
        session_active: session.proxy_child.is_some() || session.dolphin_child.is_some(),
    }
}

#[tauri::command]
fn terminate_active_session(state: State<'_, AppState>) -> Result<String, String> {
    let mut session = state.0.lock().unwrap();
    let mut terminated = false;

    // Terminate Dolphin Emulator
    if let Some(mut child) = session.dolphin_child.take() {
        let _ = child.kill();
        let _ = child.wait();
        terminated = true;
    }

    // Terminate C++ Bluetooth Proxy
    if let Some(mut child) = session.proxy_child.take() {
        let _ = child.kill();
        let _ = child.wait();
        terminated = true;
    }

    if terminated {
        Ok("Active Netplay session successfully terminated.".into())
    } else {
        Ok("No active session was running.".into())
    }
}

#[tauri::command]
fn launch_netplay_session(
    app: tauri::AppHandle,
    state: State<'_, AppState>,
    payload: SessionPayload
) -> Result<String, String> {
    let mut session = state.0.lock().unwrap();

    // Prevent double launching
    if session.proxy_child.is_some() || session.dolphin_child.is_some() {
        return Err("A Netplay session is already active. Terminate it first.".into());
    }

    let d_path = PathBuf::from(&payload.dolphin_path);
    let p_path = PathBuf::from(&payload.proxy_path);
    let iso = PathBuf::from(&payload.iso_path);

    if !d_path.exists() {
        return Err(format!("Dolphin executable not found at: {}", payload.dolphin_path));
    }
    if !p_path.exists() {
        return Err(format!("Proxy executable not found at: {}", payload.proxy_path));
    }
    if !iso.exists() {
        return Err(format!("Game ISO/WBFS not found at: {}", payload.iso_path));
    }

    // 1. Configure isolated portable environment in Dolphin's directory
    let dolphin_dir = d_path.parent().ok_or("Invalid Dolphin executable path")?.to_path_buf();
    configure_isolated_dolphin(&dolphin_dir)?;

    // 2. Spawn the High-Frequency C++ Bluetooth Proxy in background
    let proxy_child = Command::new(&p_path)
        .current_dir(p_path.parent().unwrap_or(&p_path))
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .spawn()
        .map_err(|e| format!("Failed to spawn Bluetooth Proxy: {}", e))?;

    session.proxy_child = Some(proxy_child);

    // 3. Spawn the custom Dolphin Emulator Fork, launching directly into the game ISO
    // Pass -e to boot the ISO directly
    let dolphin_child = Command::new(&d_path)
        .arg("-e")
        .arg(&iso)
        .spawn()
        .map_err(|e| {
            // Cleanup proxy if Dolphin fails to spawn
            if let Some(mut child) = session.proxy_child.take() {
                let _ = child.kill();
            }
            format!("Failed to launch Dolphin Emulator: {}", e)
        })?;

    session.dolphin_child = Some(dolphin_child);

    // 4. Spawn background monitoring thread to automatically clean up proxy when game exits
    let app_clone = app.clone();
    std::thread::spawn(move || {
        // Sleep to yield CPU
        std::thread::sleep(std::chrono::milliseconds(1000));
        
        let state_handle = app_clone.state::<AppState>();
        
        loop {
            std::thread::sleep(std::chrono::milliseconds(500));
            let mut s = state_handle.0.lock().unwrap();
            
            // Check if Dolphin has exited
            if let Some(ref mut d_child) = s.dolphin_child {
                match d_child.try_wait() {
                    Ok(Some(_status)) => {
                        println!("[Launcher Daemon] Dolphin Emulator exited. Shutting down proxy...");
                        s.dolphin_child = None;
                        
                        // Terminate Proxy
                        if let Some(mut p_child) = s.proxy_child.take() {
                            let _ = p_child.kill();
                            let _ = p_child.wait();
                        }
                        
                        // Emit event to frontend notifying session ended
                        let _ = app_clone.emit("netplay-session-ended", ());
                        break;
                    }
                    Ok(None) => {
                        // Process is still running
                    }
                    Err(e) => {
                        eprintln!("[Launcher Daemon] Error waiting on Dolphin: {}", e);
                        break;
                    }
                }
            } else {
                break;
            }
        }
    });

    Ok("Netplay session launched successfully. Proxy active at 1000Hz.".into())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .manage(AppState(Mutex::new(ActiveSession {
            proxy_child: None,
            dolphin_child: None,
        })))
        .invoke_handler(tauri::generate_handler![
            check_system_status,
            launch_netplay_session,
            terminate_active_session
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
