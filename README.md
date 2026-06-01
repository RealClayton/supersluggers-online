# Super Sluggers Online: Custom Netplay Architecture

[![Platform Support](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-blue.svg)](#)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](#)
[![Core Stack](https://img.shields.io/badge/stack-C%2B%2B17%20%7C%20Rust%20%7C%20Tauri-orange.svg)](#)

Achieve LAN-like online multiplayer for **Mario Super Sluggers** (Wii, ID: `RMBE01`) using physical Wii Remotes. This project bypasses standard Dolphin emulator Bluetooth passthrough limitations, eliminating fixed-frame netplay delay and network jitter desynchronization.

---

## 1. System Architecture Overview

The netplay architecture is split into four distinct layers that cooperate in real-time to manage inputs, emulate hardware, and sync gameplay states seamlessly over WAN.

```mermaid
graph TD
    subgraph Client A (Pitching/Fielding Team)
        A_Wiimote["Physical Wii Remote"] -->|Bluetooth 1000Hz| A_Proxy["Bluetooth Proxy (C++)"]
        A_Proxy -->|UDP Low Latency:5555| A_Dolphin["Custom Dolphin Fork"]
        A_Dolphin -->|Input Injection| A_Game["Mario Super Sluggers (main.dol)"]
        A_Game -->|Gecko State Hooks| A_Dolphin
    end
    
    subgraph Network (UDP Netplay)
        A_Dolphin <==>|Dynamic Netplay Protocol:5556| B_Dolphin["Custom Dolphin Fork"]
    end
    
    subgraph Client B (Batting/Running Team)
        B_Wiimote["Physical Wii Remote"] -->|Bluetooth 1000Hz| B_Proxy["Bluetooth Proxy (C++)"]
        B_Proxy -->|UDP Low Latency:5555| B_Dolphin["Custom Dolphin Fork"]
        B_Dolphin -->|Input Injection| B_Game["Mario Super Sluggers (main.dol)"]
        B_Game -->|Gecko State Hooks| B_Dolphin
    end
    
    A_Tauri["Tauri Launcher (Rust)"] -.->|Manages Lifecycle| A_Proxy
    A_Tauri -.->|Launches Isolated| A_Dolphin
    B_Tauri["Tauri Launcher (Rust)"] -.->|Manages Lifecycle| B_Proxy
    B_Tauri -.->|Launches Isolated| B_Dolphin
```

### Architectural Highlights

*   **1000Hz Hardware Polling:** Direct hardware interface using raw HID reports via `hidapi` to eliminate Bluetooth connection jitter.
*   **UDP Input Injection API:** Direct memory injection bypassing standard Bluetooth stacks inside Dolphin, feeding raw reports into Dolphin's emulated controller buffers on local UDP port `5555`.
*   **Adaptive Jitter Buffering:** An active buffer using linear interpolation (LERP) for motion parameters and Cubic Hermite Splines for IR cursor coordinates to smooth late network packets.
*   **Dynamic Host Authority:** Custom Netplay protocol that swaps input authority dynamically. Active batters and pitchers run with **0ms local input delay**, while the opponent client runs a smooth, jitter-buffered prediction model.
*   **Gecko State Hooks:** Custom assembly hooks intercepting game loops to write phase change tokens into a network register at `0x80002F00`.
*   **Isolated Desktop Launcher:** Tauri-based native shell managing automated daemon lifecycles and portable settings configurations.

---

## 2. Core Subsystems

### 2.1 High-Frequency C++ Bluetooth Proxy (`SluggersProxy`)
Located in [`proxy/`](file:///Volumes/Clay%20X10/02%20-%20Coding%20Projects/supersluggers-online/proxy), this background daemon connects to the physical Wii Remote (Vendor ID: `0x057e`, Product ID: `0x0306`) and polls reports at **1000Hz (1ms intervals)**.

*   **High-Precision Scheduling:** Combines standard OS thread sleeps with Arm/x86 assembly spinlocks (`yield` / `pause`) to maintain strict sub-millisecond timer resolution.
*   **Dual Compilation Modes:** Fully compiles with active `hidapi` bindings for physical hardware, or falls back to an integrated sinusoidal **Mock Physics Simulation Engine** for hardware-free development.
*   **Binary Packet Layout (30-byte UDP Payload):**
    ```cpp
    #pragma pack(push, 1)
    struct WiiRemoteReport {
        uint64_t timestamp_us;     // Microsecond precision timestamp (8 bytes)
        uint32_t sequence;         // Monotonic packet index for drop detection (4 bytes)
        uint16_t buttons;          // Digital button state mask (2 bytes)
        int16_t accel[3];          // Accelerometer x, y, z raw data (6 bytes)
        int16_t gyro[3];           // MotionPlus Gyroscope pitch, roll, yaw (6 bytes)
        uint16_t ir_pointer[2];    // IR Camera tracking coordinates (4 bytes)
    };
    #pragma pack(pop)
    ```

### 2.2 Custom Dolphin Emulator Fork
Located in [`dolphin/`](file:///Volumes/Clay%20X10/02%20-%20Coding%20Projects/supersluggers-online/dolphin), the custom C++ emulator fork incorporates input injection, adaptive buffer math, and memory state synchronizations.

#### 1. Input Injection Hook
Intercepts Dolphin's hardware emulation polling thread inside `WiimoteEmu.cpp` and injects raw proxy structures, completely bypassing standard operating system device mapping lags.
```cpp
void Wiimote::UpdateInput() {
    if (NetplayInputReceiver::GetInstance().IsActive()) {
        EmulatedWiimoteState state = NetplayInputReceiver::GetInstance().GetEmulatedState();
        m_buttons = state.buttons;
        m_accel_x = state.accel[0]; m_accel_y = state.accel[1]; m_accel_z = state.accel[2];
        m_gyro_pitch = state.gyro[0]; m_gyro_roll = state.gyro[1]; m_gyro_yaw = state.gyro[2];
        m_ir_x = state.ir_pointer[0]; m_ir_y = state.ir_pointer[1];
        return;
    }
    // ... Original Dolphin mapping fallback ...
}
```

#### 2. Adaptive Jitter Buffer Math
Plays back inputs from a time-warped queue. When packet arrivals are delayed:
*   **Accelerometer & Gyroscope LERP:** Smooths standard motion vectors:
    $$\vec{A}_{\text{interpolated}} = \vec{A}_1 + t \cdot (\vec{A}_2 - \vec{A}_1) \quad \text{where} \quad t = \frac{T_{\text{target}} - T_1}{T_2 - T_1}$$
*   **IR Pointer Cubic Hermite Spline:** Prevents jagged cursor movements by interpolating a curved path over 4 surrounding frames ($P_0, P_1, P_2, P_3$):
    $$P(t) = (2t^3 - 3t^2 + 1)P_1 + (t^3 - 2t^2 + t)\vec{m}_1 + (-2t^3 + 3t^2)P_2 + (t^3 - t^2)\vec{m}_2$$
    $$\text{where} \quad \vec{m}_1 = \frac{P_2 - P_0}{2}, \quad \vec{m}_2 = \frac{P_3 - P_1}{2}$$

#### 3. Dynamic Host Authority Protocol
Maintains **0ms local input latency** for critical timing events by dynamically shifting input authority.
```
Pitching Phase:
  Pitcher Client ───> Instant Local Input (0ms) ───> Sends Inputs ───> Batter Client (renders pitch)

Hit Detected (Gecko hook writes 0x02 to 0x80002F00):
  Batter Client <─── Instant Local Input (0ms) <─── Holds Authority <─── Pitcher Client (renders fielding)
```
1.  **Pitching Phase (`0x01`):** The pitching team executes inputs instantly (0ms delay). The batting team buffers and renders the pitch.
2.  **Contact/Fielding Phase (`0x02`):** Instantly triggered when the bat strikes the ball. Authority shifts to the offensive team, allowing the batter to run and field with 0ms response time while the pitcher buffers.

### 2.3 Tauri Desktop Launcher
Located in [`launcher/`](file:///Volumes/Clay%20X10/02%20-%20Coding%20Projects/supersluggers-online/launcher), the desktop shell provides user configurations and manages low-level subprocesses.

*   **Settings Isolation:** Automatically places an empty `portable.txt` in the custom Dolphin directory to isolate netplay configurations from the host's global setups. Writes a customized low-latency `Dolphin.ini` forcing Vulkan rendering, connective netplay ports (`5556`), and chunked codes.
*   **Lifecycle Daemon:** When you click **Inject & Launch Playball**, the Rust backend spawns `SluggersProxy` in the background and runs Dolphin directly into the ISO. A background thread polls Dolphin's status every 500ms; the moment you exit the game window, Rust kills the background C++ proxy automatically to leave the host system completely clean.

### 2.4 Gecko Memory Hook Subsystem
Located in [`gecko/`](file:///Volumes/Clay%20X10/02%20-%20Coding%20Projects/supersluggers-online/gecko), this defines the assembly hook locations in Wii `MEM1` RAM (`0x80000000` to `0x817FFFFF`) that detect gameplay phase transitions.

*   **Hook A: Pitcher Wind-Up Hook:** Captures the starting frame of the pitcher's wind-up animation to trigger Defensive Authority.
*   **Hook B: Batter Swing Hook:** Bypasses standard Bluetooth latency thresholds by scanning analog swing acceleration floats directly in memory.
*   **Hook C: Ball-in-Play Hook:** Instantly detects the millisecond the ball contacts the bat and changes the Netplay Register at `0x80002F00` to `0x02` to swap authority to the Batter/Fielder client.

---

## 3. Developer Quick Start

### 3.1 Prerequisites
Ensure the following tools are set up on your development workstation:
*   `Clang++` (with C++17 support)
*   `Node.js` (v18 or higher)
*   `Rust` compiler (`rustup`)
*   `CMake` (v3.13 or higher)
*   `devkitPPC` (for compilation of game asset extractions)

### 3.2 Compiling & Verifying the Proxy (`SluggersProxy`)
You can compile and run automated diagnostics on the C++ Bluetooth Proxy locally.

1.  **Compile the Proxy:**
    ```bash
    cd proxy
    clang++ -std=c++17 -O3 -Wall -pthread src/main.cpp -o SluggersProxy
    ```
2.  **Run Automated Timing and Packet Diagnostics:**
    The root directory contains an automated python suite [`verify_proxy.py`](file:///Volumes/Clay%20X10/02%20-%20Coding%20Projects/supersluggers-online/verify_proxy.py) that spins up the proxy daemon, binds local UDP sockets, records 3,000 frames, conducts timing and jitter analyses, and generates a formal report:
    ```bash
    python verify_proxy.py
    ```

> [!TIP]
> View [`proxy_validation_report.md`](file:///Volumes/Clay%20X10/02%20-%20Coding%20Projects/supersluggers-online/proxy_validation_report.md) in the workspace root to check compliance bounds. The high-precision thread spinlock routinely locks loop intervals to **1000 Hz with less than 15 us of standard deviation jitter**.

### 3.3 Running the Tauri Launcher
To spin up the native dashboard in developer mode:
```bash
cd launcher
npm install
npm run tauri dev
```
To compile the launcher for distribution (outputs `.exe` on Windows, `.deb`/`.AppImage` on Linux):
```bash
npm run tauri build
```

### 3.4 Compiling the Custom Dolphin Fork
#### Linux Mint / Debian:
```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```
#### Windows (Visual Studio 2022):
1.  Open the Dolphin root directory in Visual Studio 2022.
2.  Generate CMake solutions:
    ```cmd
    cmake -B build -G "Visual Studio 17 2022" -A x64
    ```
3.  Open `build/Dolphin.sln`, set configuration to **Release / x64**, and compile.

---

## 4. Progress Tracker & Roadmap

*   [x] **Setup Workspace Environment:** Acquisition and verification of `RMBE01` (NTSC-USA) WBFS image.
*   [x] **Deploy Extract Tools:** Apple Silicon code signing bypassed for `wit` compiler (v3.05a).
*   [x] **Partition Asset Extraction:** Completed. Game partitions fully decompiled, [main.dol](file:///Volumes/Clay%20X10/02%20-%20Coding%20Projects/supersluggers-online/extracted_game/sys/main.dol) extracted.
*   [x] **Phase 2 (Proxy Development):** C++ high-frequency proxy completed, mock simulators integrated, and HIDAPI compilation models supported.
*   [x] **Phase 3 (Dolphin Customizations):** Input injection socket listeners and adaptive Hermite/LERP jitter buffers verified.
*   [x] **Phase 4 (Automated Launcher):** Tauri-based isolated settings and multi-process lifecycle monitors completed.
*   [x] **Timing & Diagnostic Certification:** Automated verification completes successfully with strict standard deviation thresholds.
*   [ ] **Phase 5 (Engine Reverse Engineering):** Scan `MEM1` dynamic values in live games to identify address targets and finalize custom Gecko hooks.

---

## 5. System Specifications & Hardware Setup

*   **Netplay Host/Server:** 2016 MacBook Pro running Linux Mint (Gigabit Ethernet connection for WAN sync).
*   **Play clients:** Standard x86/x64 Windows PCs and modern Linux play stations.
*   **Controllers:** Original Nintendo Wii Remotes (RVL-001) or MotionPlus-integrated Remotes connected to host PCs using standard USB Bluetooth adapters or CSR 4.0 adapters.

---

## 6. License
This architecture is licensed under the [MIT License](LICENSE) - see the project details for terms.
