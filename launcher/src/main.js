// Import Tauri APIs
const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

document.addEventListener("DOMContentLoaded", () => {
    // DOM Elements
    const dashboard = document.getElementById("dashboard");
    const proxyIndicator = document.getElementById("proxy-indicator");
    const proxyStatusText = document.getElementById("proxy-status-text");
    const dolphinIndicator = document.getElementById("dolphin-indicator");
    const dolphinStatusText = document.getElementById("dolphin-status-text");
    
    const isoPathInput = document.getElementById("iso-path");
    const dolphinPathInput = document.getElementById("dolphin-path");
    const proxyPathInput = document.getElementById("proxy-path");
    const serverIpInput = document.getElementById("server-ip");
    
    const roleDefenseBtn = document.getElementById("role-defense");
    const roleOffenseBtn = document.getElementById("role-offense");
    
    const launchActionBtn = document.getElementById("btn-launch-action");
    const logConsole = document.getElementById("log-console");

    let isOffense = false;
    let sessionActive = false;

    // Helper: Add logs to scrolling console box
    function addLogLine(text, type = "info") {
        logConsole.style.display = "block";
        const line = document.createElement("div");
        line.className = `console-line ${type}`;
        line.textContent = `[${new Date().toLocaleTimeString()}] ${text}`;
        logConsole.appendChild(line);
        logConsole.scrollTop = logConsole.scrollHeight;
    }

    // Role selector toggles
    roleDefenseBtn.addEventListener("click", () => {
        roleDefenseBtn.classList.add("active");
        roleOffenseBtn.classList.remove("active");
        isOffense = false;
        addLogLine("Dynamic Netplay Role set to DEFENSE (Local Pitching authority active).");
    });

    roleOffenseBtn.addEventListener("click", () => {
        roleOffenseBtn.classList.add("active");
        roleDefenseBtn.classList.remove("active");
        isOffense = true;
        addLogLine("Dynamic Netplay Role set to OFFENSE (Local Batting/Fielding authority active).");
    });

    // Check system status in real-time
    async function updateSystemStatus() {
        try {
            const status = await invoke("check_system_status", {
                dolphinPath: dolphinPathInput.value,
                proxyPath: proxyPathInput.value
            });

            // Update Proxy Indicator
            if (status.proxy_found) {
                proxyIndicator.classList.add("active");
                proxyStatusText.textContent = "1000Hz service binary verified.";
                proxyStatusText.style.color = "#00ff87";
            } else {
                proxyIndicator.classList.remove("active");
                proxyStatusText.textContent = "Proxy binary missing in workspace.";
                proxyStatusText.style.color = "#ff2a5f";
            }

            // Update Dolphin Indicator
            if (status.dolphin_found) {
                dolphinIndicator.classList.add("active");
                dolphinStatusText.textContent = "Custom Dolphin Fork binary verified.";
                dolphinStatusText.style.color = "#00ff87";
            } else {
                dolphinIndicator.classList.remove("active");
                dolphinStatusText.textContent = "Dolphin Fork executable missing.";
                dolphinStatusText.style.color = "#ff2a5f";
            }

            // Synchronize active session buttons
            if (status.session_active && !sessionActive) {
                setSessionActiveMode();
            } else if (!status.session_active && sessionActive) {
                setSessionInactiveMode();
            }
        } catch (err) {
            console.error("Status check failure:", err);
        }
    }

    function setSessionActiveMode() {
        sessionActive = true;
        launchActionBtn.textContent = "Terminate Netplay Session";
        launchActionBtn.classList.add("active-session");
        dashboard.classList.add("running");
    }

    function setSessionInactiveMode() {
        sessionActive = false;
        launchActionBtn.textContent = "Inject & Launch Playball";
        launchActionBtn.classList.remove("active-session");
        dashboard.classList.remove("running");
    }

    // Launch Netplay Session trigger
    async function handleLaunchTrigger() {
        if (sessionActive) {
            // Terminate Session
            addLogLine("Initiating session shutdown sequence...", "info");
            try {
                const res = await invoke("terminate_active_session");
                addLogLine(res, "success");
                setSessionInactiveMode();
            } catch (err) {
                addLogLine(`Termination failure: ${err}`, "error");
            }
        } else {
            // Launch Session
            addLogLine("Configuring Dolphin isolation profiles...", "info");
            
            const payload = {
                iso_path: isoPathInput.value,
                dolphin_path: dolphinPathInput.value,
                proxy_path: proxyPathInput.value,
                is_pitching_team: !isOffense,
                server_ip: serverIpInput.value
            };

            try {
                addLogLine("Spawning background high-frequency C++ Proxy...", "info");
                const res = await invoke("launch_netplay_session", { payload });
                addLogLine(res, "success");
                addLogLine("Launching isolated custom Dolphin Fork into game...", "info");
                setSessionActiveMode();
            } catch (err) {
                addLogLine(`Launch aborted: ${err}`, "error");
            }
        }
    }

    launchActionBtn.addEventListener("click", handleLaunchTrigger);

    // Poll status checks every 1.5 seconds
    setInterval(updateSystemStatus, 1500);
    updateSystemStatus();

    // Listen for process exit events emitted from Rust background daemon thread
    listen("netplay-session-ended", () => {
        addLogLine("Dolphin Emulator process exited cleanly. Auto-terminating background Bluetooth proxy.", "success");
        setSessionInactiveMode();
    });
});
