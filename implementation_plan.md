# Implementation Plan: Mario Super Sluggers Custom Netplay Architecture

This document establishes the comprehensive strategy, architecture, and step-by-step roadmap for achieving LAN-like Netplay for *Mario Super Sluggers* (Wii, ID: `RMBE01`). By replacing the default Dolphin emulator fixed-delay netplay with a custom hybrid input proxy and dynamic host authority network, this project aims to eliminate frame delay and network jitter desynchronization.

---

## 1. System Architecture Overview

The netplay architecture is divided into four distinct layers cooperating in real-time.

```mermaid
graph TD
    subgraph Client A (Pitching/Fielding Team)
        A_Wiimote["Physical Wii Remote"] -->|Bluetooth 1000Hz| A_Proxy["Bluetooth Proxy (C++)"]
        A_Proxy -->|UDP Low Latency| A_Dolphin["Custom Dolphin Fork"]
        A_Dolphin -->|Input Injection| A_Game["Mario Super Sluggers (main.dol)"]
        A_Game -->|Gecko State Hooks| A_Dolphin
    end
    
    subgraph Network (UDP Netplay)
        A_Dolphin <==>|Dynamic Netplay Protocol| B_Dolphin["Custom Dolphin Fork"]
    end
    
    subgraph Client B (Batting/Running Team)
        B_Wiimote["Physical Wii Remote"] -->|Bluetooth 1000Hz| B_Proxy["Bluetooth Proxy (C++)"]
        B_Proxy -->|UDP Low Latency| B_Dolphin["Custom Dolphin Fork"]
        B_Dolphin -->|Input Injection| B_Game["Mario Super Sluggers (main.dol)"]
        B_Game -->|Gecko State Hooks| B_Dolphin
    end
    
    A_Tauri["Tauri Launcher (Rust)"] -.->|Manages Lifecycle| A_Proxy
    A_Tauri -.->|Launches Isolated| A_Dolphin
    B_Tauri["Tauri Launcher (Rust)"] -.->|Manages Lifecycle| B_Proxy
    B_Tauri -.->|Launches Isolated| B_Dolphin
```

### Key Highlights
*   **1000Hz Bluetooth Polling:** Direct hardware interface polling raw HID reports to eliminate local Bluetooth connection jitter.
*   **UDP Input Injection:** Direct memory injection bypassing standard Bluetooth emulated stacks inside Dolphin, mapping physical remote streams to "Emulated Wii Remote" buffers.
*   **Gecko State Hooks:** Real-time state hooks modifying or monitoring specific RAM values inside `MEM1` to detect phase transitions.
*   **Dynamic Host Authority:** Shifting game state control from one emulator instance to another depending on who is pitching, batting, or fielding, maintaining 0ms input delay for critical physical actions (e.g., batting swing, pitcher release).

---

## 2. Progress Tracker & Completed Setup

### Task Board
- [x] **Setup Workspace Environment**
- [x] **Acquire and Verify Game WBFS Image** (`RMBE01` - NTSC-USA)
- [x] **Deploy & Verify Extract Tools (`wit` compiler)**
    - *Note:* Downloaded WIT v3.05a, handled Apple Silicon execution signing via `codesign` to bypass Gatekeeper SIGKILL (Exit Code 137).
- [x] **Extract Game Partition Assets & Executables**
    - Raw executable located at: [main.dol](file:///Volumes/Clay%20X10/02%20-%20Coding%20Projects/supersluggers-online/extracted_game/sys/main.dol) (7.4 MB)
- [ ] **Phase 1: Engine Reverse Engineering** (Current Focus)
- [ ] **Phase 2: High-Frequency Bluetooth Proxy (C++)**
- [ ] **Phase 3: Dolphin Fork Core Integrations**
- [ ] **Phase 4: Tauri Automated Launcher**

---

## 3. Phase 1: Engine Reverse Engineering Strategy

To implement *Dynamic Host Authority* netcode, we must locate the key memory variables in `MEM1` (0x80000000 - 0x817FFFFF) that dictate the game's high-level state:

```
┌────────────────────────────────────────────────────────┐
│                   Game Play Phases                     │
├───────────────┬──────────────────────┬─────────────────┤
│ Pitching Phase│ Batting/Swing Phase  │ Fielding Phase  │
│ (Pitcher runs │ (Batter releases swing│ (Ball in-play,  │
│ wind-up logic)│ thresholds, timing)  │ fields active)  │
└───────────────┴──────────────────────┴─────────────────┘
```

### 3.1 Memory Location Goals
1.  **Pitching Wind-Up Hook:** Address that changes value when a pitcher begins their wind-up.
2.  **Batter Swing Threshold:** Float or integer tracking the rotation angle or swing acceleration triggers of the bat.
3.  **Ball-in-Play Flag:** Address toggling the moment a ball is struck by a bat or hits the field.
4.  **Team Possession / Turn Register:** Register identifying which team is on offense and which is on defense.

### 3.2 Toolchain & Decompilation Setup
Ghidra is our tool of choice for reverse engineering `main.dol`.

> [!NOTE]
> Standard Ghidra does not support Wii `.dol` executables natively. We must use the [Ghidra-GameCube-Loader](https://github.com/Cuyler36/Ghidra-GameCube-Loader) extension.

#### Conversion alternative (dol2elf):
If Ghidra extensions cause loading issues on Apple Silicon, we will convert the DOL binary to a standard ELF binary using `devkitPPC`'s `doltool` or modern GameCube tools:
```bash
doltool -e main.dol
```

### 3.3 Dynamic RAM Inspection via Dolphin Debugger
Static analysis in Ghidra is combined with dynamic RAM scanning:
1.  Launch **Dolphin Emulator** with the `--developer` or debugger CLI options active.
2.  Use the **Cheat Search** or the **Dolphin Memory Engine** tool to freeze values and track changes:
    *   *Search Strategy:* Track swing thresholds by conducting a float/integer search on the bat swing animation state. Track pitching states by searching for changing states during a pitch release.

---

## 4. Phase 2: High-Frequency Bluetooth Proxy (C++)

A dedicated background C++ process will interface with the host's Bluetooth HID adapter.

### Core Architecture
*   **Library:** Native C++ calling `hidapi` (for cross-platform HID polling) or native macOS/Linux Bluetooth sockets.
*   **Polling Loop:** Running on a high-precision multimedia timer thread executing at 1000Hz (1ms intervals).
*   **Packet Format:** Custom UDP packet containing:
    ```cpp
    struct WiiRemoteReport {
        uint64_t timestamp_us;     // Microsecond precision timestamp
        uint16_t buttons;          // Digital button states
        int16_t accel[3];          // Accelerometer x, y, z
        int16_t gyro[3];           // Gyroscope x, y, z (MotionPlus)
        uint16_t ir_pointer[2];    // X, Y IR sensor coordinates
    };
    ```

---

## 5. Phase 3: Custom Dolphin Emulator Fork

A specialized fork of Dolphin written in C++ will act as our input consumer and netplay syncer.

### 5.1 Input Injection API
We will add a UDP listening server thread in Dolphin. Instead of opening a local Bluetooth driver (which standard macOS/Windows Dolphin instances struggle with), this thread ingests raw reports from the Phase 2 C++ Proxy and writes them directly into the emulated controller state buffer.

### 5.2 Adaptive Jitter Buffer
Because networks jitter, packets will occasionally arrive out of order or late.
*   Instead of stalling the game (like standard Dolphin netplay), our custom buffer will use **linear interpolation** to predict motion sensor inputs for late frames.
*   IR pointers will use **Hermite spline interpolation** over the last 3-5 frames to ensure the cursor moves fluidly across the screen even on a 50ms jitter connection.

### 5.3 Dynamic Host Authority (Netcode)
Standard netplay synchronizes all buttons on a single frame boundary, incurring round-trip delay. Our network layers will run a custom netcode:
*   When Client A (Pitcher) releases the ball, Client A's instance has authority. Inputs from Client A are executed locally immediately (0ms delay) and sent to Client B.
*   The moment the ball crosses the plate, authority is dynamically negotiated.
*   When the ball is hit, a custom **Gecko Assembly Hook** instantly detects the "Ball-in-Play" state and shifts authority to Client B (Batter/Fielder) so they can swing and field with 0ms local response.

---

## 6. Next Steps & Actions

1.  **Symbol Mapping:** Cross-reference known Wii SDK symbol tables to map out typical system libraries inside `main.dol` (e.g., standard OS functions, Wii Remote libraries `WPAD`, etc.).
2.  **Dolphin Setup:** Configure a developer version of Dolphin on the workstation to prepare for memory scans.
3.  **Memory Search Plan:** Create a standard guide for scanning pitching, swing, and batting values inside Dolphin's Memory Engine.
