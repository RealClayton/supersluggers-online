use std::process::{Child, Command};
use std::sync::Mutex;
use std::path::PathBuf;
use std::fs;
use tauri::{State, Manager, Emitter};

// Thread-safe state to manage active subprocesses and connection states
struct ActiveSession {
    dolphin_child: Option<Child>,
    is_waiting_for_host: bool,
}

struct AppState(Mutex<ActiveSession>);

#[derive(serde::Serialize, serde::Deserialize, Clone)]
struct SystemStatus {
    dolphin_found: bool,
    rom_found: bool,
    proxy_found: bool,
    session_active: bool,
    is_waiting_for_host: bool,
}

#[derive(serde::Serialize, serde::Deserialize, Clone)]
struct SessionPayload {
    iso_path: String,
    dolphin_path: String,
    proxy_path: String,
    is_host: bool,         // true = host, false = client
    remote_ip: String,     // IP of the other player (for state sync)
    sync_port: u16,        // UDP port for game state sync (default 5556)
}

/// Configures Dolphin in portable mode with Real Wiimote support.
/// Dynamically assigns physical and network controller mapping based on Host/Client role.
fn configure_isolated_dolphin(dolphin_dir: &PathBuf, rom_dir: &str, payload: &SessionPayload) -> Result<(), String> {
    // 1. Create portable.txt to force Dolphin into portable mode
    let portable_file = dolphin_dir.join("portable.txt");
    if !portable_file.exists() {
        fs::write(&portable_file, "").map_err(|e| format!("Failed to create portable.txt: {}", e))?;
    }

    // 2. Create User/Config directory
    let config_dir = dolphin_dir.join("User/Config");
    fs::create_dir_all(&config_dir).map_err(|e| format!("Failed to create User/Config: {}", e))?;

    // 3. Write Dolphin.ini
    //    - WiimoteContinuousScanning = True: auto-detects Wii Remotes
    //      regardless of whether they connect via DolphinBar or standard BT
    let role = if payload.is_host { "host" } else { "client" };
    let ini_content = format!(
        "[Interface]\n\
         ConfirmStop = False\n\
         UsePanicHandlers = False\n\
         AllowOnlyOneInstance = False\n\
         [Core]\n\
         GFXBackend = D3D11\n\
         EnableCheats = True\n\
         WiimoteContinuousScanning = True\n\
         CPUThread = False\n\
         [Display]\n\
         AspectRatio = 1\n\
         [Wii]\n\
         Widescreen = True\n\
         SensorBarPosition = 1\n\
         [General]\n\
         ISOPaths = 1\n\
         ISOPath0 = {}\n\
         [GameStateSync]\n\
         Enabled = True\n\
         Role = {}\n\
         RemoteIP = {}\n\
         Port = {}\n",
        rom_dir, role, payload.remote_ip, payload.sync_port
    );
    let ini_file = config_dir.join("Dolphin.ini");
    fs::write(&ini_file, ini_content).map_err(|e| format!("Failed to write Dolphin.ini: {}", e))?;

    // 4. Write WiimoteNew.ini — Source = 2 (Real Wiimote), Source = 1 (Emulated Wiimote)
    //    - Host (Home Team): controls Wiimote1 natively (Source = 2), Wiimote2 is synced over Net (Source = 1)
    //    - Client (Away Team): Wiimote1 is synced over Net (Source = 1), controls Wiimote2 natively (Source = 2)
    let (wiimote1_source, wiimote2_source) = if payload.is_host {
        (2, 1)
    } else {
        (1, 2)
    };
    let wiimote_ini = format!(
        "[Wiimote1]\n\
         Source = {}\n\
         [Wiimote2]\n\
         Source = {}\n\
         [Wiimote3]\n\
         Source = 0\n\
         [Wiimote4]\n\
         Source = 0\n",
        wiimote1_source, wiimote2_source
    );
    let wiimote_file = config_dir.join("WiimoteNew.ini");
    fs::write(&wiimote_file, wiimote_ini).map_err(|e| format!("Failed to write WiimoteNew.ini: {}", e))?;

    Ok(())
}

/// Helper function to start the background thread that monitors Dolphin exit.
fn start_monitoring_thread(app: tauri::AppHandle) {
    std::thread::spawn(move || {
        std::thread::sleep(std::time::Duration::from_millis(1000));
        
        let state_handle = app.state::<AppState>();
        
        loop {
            std::thread::sleep(std::time::Duration::from_millis(500));
            let mut s = state_handle.0.lock().unwrap();
            
            if let Some(ref mut d_child) = s.dolphin_child {
                match d_child.try_wait() {
                    Ok(Some(_)) => {
                        println!("[Launcher] Dolphin exited. Cleaning up...");
                        s.dolphin_child = None;
                        let _ = app.emit("netplay-session-ended", ());
                        break;
                    }
                    Ok(None) => {}
                    Err(e) => {
                        eprintln!("[Launcher] Error monitoring Dolphin: {}", e);
                        break;
                    }
                }
            } else {
                break;
            }
        }
    });
}

/// Start a background UDP listener on port 5558 for client mode.
/// Blocks until host sends the "BOOT_GAME" sync token or a cancel token is received.
fn run_client_listener(app: tauri::AppHandle, payload: SessionPayload) {
    std::thread::spawn(move || {
        println!("[Launcher] Client UDP listener starting on port 5558...");
        let socket = match std::net::UdpSocket::bind("0.0.0.0:5558") {
            Ok(s) => s,
            Err(e) => {
                eprintln!("[Launcher] Client failed to bind UDP port 5558: {}", e);
                let state_handle = app.state::<AppState>();
                if let Ok(mut session) = state_handle.0.lock() {
                    session.is_waiting_for_host = false;
                }
                return;
            }
        };

        // Send a tiny PING to Remote IP on port 5558 to register our address on a UDP relay server (if present)
        let server_addr = format!("{}:5558", payload.remote_ip);
        let _ = socket.send_to(b"PING", &server_addr);

        let mut buf = [0u8; 1024];
        loop {
            match socket.recv_from(&mut buf) {
                Ok((amt, src)) => {
                    let msg = String::from_utf8_lossy(&buf[..amt]);
                    let msg_trimmed = msg.trim();
                    if msg_trimmed == "CANCEL" {
                        println!("[Launcher] Connection search canceled cleanly.");
                        break;
                    }
                    if msg_trimmed == "BOOT_GAME" {
                        println!("[Launcher] Received BOOT_GAME from Host at {}", src);
                        
                        let state_handle = app.state::<AppState>();
                        {
                            let mut session = state_handle.0.lock().unwrap();
                            if !session.is_waiting_for_host {
                                println!("[Launcher] Received boot token, but we are no longer waiting.");
                                break;
                            }
                            session.is_waiting_for_host = false;
                        }

                        // Sync-launch Dolphin for Client
                        let d_path = PathBuf::from(&payload.dolphin_path);
                        let iso = PathBuf::from(&payload.iso_path);
                        let rom_dir = iso.parent()
                            .map(|p| p.to_string_lossy().to_string())
                            .unwrap_or_default();
                        
                        let dolphin_dir = match d_path.parent() {
                            Some(p) => p.to_path_buf(),
                            None => {
                                eprintln!("[Launcher] Invalid Dolphin directory path.");
                                return;
                            }
                        };

                        if let Err(e) = configure_isolated_dolphin(&dolphin_dir, &rom_dir, &payload) {
                            eprintln!("[Launcher] Client configuration failed: {}", e);
                            return;
                        }

                        match Command::new(&d_path).arg("-e").arg(&payload.iso_path).spawn() {
                            Ok(dolphin_child) => {
                                println!("[Launcher] Dolphin sync-launched successfully for Client!");
                                {
                                    let mut session = state_handle.0.lock().unwrap();
                                    session.dolphin_child = Some(dolphin_child);
                                }
                                let _ = app.emit("netplay-session-started", ());
                                start_monitoring_thread(app.clone());
                            }
                            Err(e) => {
                                eprintln!("[Launcher] Failed to launch Dolphin: {}", e);
                            }
                        }
                        break;
                    }
                }
                Err(e) => {
                    // Ignore connection reset (ICMP Port Unreachable) errors on Windows
                    if e.kind() == std::io::ErrorKind::ConnectionReset {
                        continue;
                    }
                    eprintln!("[Launcher] UDP read error: {}", e);
                    break;
                }
            }
        }
    });
}

#[tauri::command]
fn check_system_status(
    state: State<'_, AppState>,
    dolphin_path: String,
    iso_path: String,
    proxy_path: String,
) -> SystemStatus {
    let session = state.0.lock().unwrap();
    
    SystemStatus {
        dolphin_found: PathBuf::from(&dolphin_path).exists(),
        rom_found: PathBuf::from(&iso_path).exists(),
        proxy_found: PathBuf::from(&proxy_path).exists(),
        session_active: session.dolphin_child.is_some(),
        is_waiting_for_host: session.is_waiting_for_host,
    }
}

#[tauri::command]
fn terminate_active_session(state: State<'_, AppState>) -> Result<String, String> {
    let mut session = state.0.lock().unwrap();

    if session.is_waiting_for_host {
        session.is_waiting_for_host = false;
        // Unblock UDP socket by sending a CANCEL signal to local port
        if let Ok(socket) = std::net::UdpSocket::bind("0.0.0.0:0") {
            let _ = socket.send_to(b"CANCEL", "127.0.0.1:5558");
        }
        return Ok("Connection search canceled.".into());
    }

    if let Some(mut child) = session.dolphin_child.take() {
        let _ = child.kill();
        let _ = child.wait();
        Ok("Session terminated.".into())
    } else {
        Ok("No active session.".into())
    }
}

#[tauri::command]
fn launch_netplay_session(
    app: tauri::AppHandle,
    state: State<'_, AppState>,
    payload: SessionPayload
) -> Result<String, String> {
    let mut session = state.0.lock().unwrap();

    if session.dolphin_child.is_some() || session.is_waiting_for_host {
        return Err("A session is already active. Terminate it first.".into());
    }

    let d_path = PathBuf::from(&payload.dolphin_path);
    let iso = PathBuf::from(&payload.iso_path);

    if !d_path.exists() {
        return Err(format!("Dolphin not found at: {}", payload.dolphin_path));
    }
    if !iso.exists() {
        return Err(format!("Game ROM not found at: {}", payload.iso_path));
    }

    if payload.is_host {
        // 1. Configure isolated portable Dolphin with Real Wiimote
        let rom_dir = iso.parent()
            .map(|p| p.to_string_lossy().to_string())
            .unwrap_or_default();
        let dolphin_dir = d_path.parent().ok_or("Invalid Dolphin path")?.to_path_buf();
        configure_isolated_dolphin(&dolphin_dir, &rom_dir, &payload)?;

        // 2. Host signaling: Broadcast "BOOT_GAME" to Client over UDP
        println!("[Launcher] Host sending BOOT_GAME signal to client at {}:5558...", payload.remote_ip);
        let socket = std::net::UdpSocket::bind("0.0.0.0:0")
            .map_err(|e| format!("Failed to bind local UDP socket: {}", e))?;
        let client_addr = format!("{}:5558", payload.remote_ip);
        for _ in 0..3 {
            let _ = socket.send_to(b"BOOT_GAME", &client_addr);
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        std::thread::sleep(std::time::Duration::from_millis(150));

        // 3. Spawn Dolphin standalone
        let dolphin_child = Command::new(&d_path)
            .arg("-e")
            .arg(&payload.iso_path)
            .spawn()
            .map_err(|e| format!("Failed to launch Dolphin: {}", e))?;

        session.dolphin_child = Some(dolphin_child);

        // 4. Background monitoring thread for cleanup
        start_monitoring_thread(app.clone());

        let _ = app.emit("netplay-session-started", ());

        Ok("Session launched as HOST. Client signaled successfully!".into())
    } else {
        // CLIENT: Start UDP receiver and set state to waiting
        session.is_waiting_for_host = true;
        run_client_listener(app.clone(), payload.clone());

        Ok("Waiting for Host to boot the match (Listening on UDP port 5558)...".into())
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .manage(AppState(Mutex::new(ActiveSession {
            dolphin_child: None,
            is_waiting_for_host: false,
        })))
        .invoke_handler(tauri::generate_handler![
            check_system_status,
            launch_netplay_session,
            terminate_active_session
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
