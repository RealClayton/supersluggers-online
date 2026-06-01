# Super Sluggers Online: Automated Netplay Launcher

This directory houses the **Tauri-based desktop launcher application** for *Mario Super Sluggers* Custom Netplay. It integrates the low-level C++ Bluetooth input proxy and custom Dolphin Emulator fork into a single-button startup pipeline.

---

## 1. Core Features & Architectural Design

```
                     ┌────────────────────────┐
                     │ Tauri Frontend (HTML)  │
                     └───────────┬────────────┘
                                 │ window.__TAURI__.core.invoke()
                                 ▼
                     ┌────────────────────────┐
                     │  Tauri Rust Backend    │
                     └─────┬────────────┬─────┘
                           │            │
         Spawns in BG      ▼            ▼      Spawns directly into ISO
   ┌─────────────────────────┐        ┌─────────────────────────┐
   │ C++ Bluetooth Proxy     │        │ Custom Dolphin Fork     │
   │ (SluggersProxy, 1000Hz) │        │ (Dolphin, Portable)     │
   └─────────────────────────┘        └───────────┬─────────────┘
                                                  │
                                                  ▼ Monitored in BG by Rust
                                      [Exits] ────> Sends Kill to Proxy
```

### A. Settings Isolation
To prevent corrupting or overwriting your global, everyday Dolphin settings, the launcher automatically creates a **Portable Isolation Profile** on launch:
1. Writes an empty `portable.txt` inside the custom Dolphin directory.
2. Initializes a local `User/Config/` folder.
3. Writes a custom `Dolphin.ini` with low-latency netplay parameters (forcing Vulkan rendering, connective netplay ports, chunked codes, and disabling panic popups).

### B. Background Process Daemon
When you click **Inject & Launch**, the Rust backend spawns the C++ Bluetooth Proxy (`SluggersProxy`) silently in the background and launches Dolphin directly into the game ISO. 
* A background Rust monitoring thread ticks every 500ms checking the status of Dolphin.
* The moment the user exits the game window, Rust detects the process teardown and automatically terminates (`std::process::Child::kill`) the background proxy, keeping the host system clean.

---

## 2. Compile & Run Instructions

When you clone this repository on your play environments (Windows or Linux), use the following steps to run the launcher.

### Prerequisites (All Platforms)
1. **Node.js:** Install Node.js (v18 or higher) from [nodejs.org](https://nodejs.org).
2. **Rust:** Install the Rust compiler from [rustup.rs](https://rustup.rs).

### A. Windows (Home Play Client)
1. Open PowerShell or Command Prompt.
2. Navigate to this directory:
   ```cmd
   cd launcher
   ```
3. Install frontend dependencies:
   ```cmd
   npm install
   ```
4. Install Tauri CLI globally (optional but helpful):
   ```cmd
   npm install -g @tauri-apps/cli
   ```
5. Run the developer build:
   ```cmd
   npm run tauri dev
   ```
   *Note: This compiles the Rust backend, opens a styled native app window, and supports live CSS/JS reloading.*
6. Build the final optimized `.exe` installer:
   ```cmd
   npm run tauri build
   ```

### B. Linux Mint (Server Setup)
1. Install Linux compilation dependencies:
   ```bash
   sudo apt-get install -y libwebkit2gtk-4.1-dev build-essential curl wget \
                           file libxdo-dev libssl-dev libayatana-appindicator3-dev \
                           librsvg2-dev
   ```
2. Navigate to this directory:
   ```bash
   cd launcher
   ```
3. Install dependencies:
   ```bash
   npm install
   ```
4. Run in dev mode:
   ```bash
   npm run tauri dev
   ```
5. Compile the final `.deb` and `.AppImage` packages:
   ```bash
   npm run tauri build
   ```
