#define _USE_MATH_DEFINES
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cmath>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <intrin.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    typedef int SOCKET;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
#endif

// Dual-Mode Compilation: Enable HIDAPI if compiling with hardware support
#if defined(HAS_HIDAPI) && HAS_HIDAPI
    #include <hidapi.h>
    #define HARDWARE_SUPPORT_ENABLED 1
#else
    #define HARDWARE_SUPPORT_ENABLED 0
#endif

// Standard Wii Remote HID Report Payload structure (UDP package)
#pragma pack(push, 1)
struct WiiRemoteReport {
    uint64_t timestamp_us;     // High-resolution timestamp in microseconds
    uint32_t sequence;         // Monotonic packet sequence number for drop detection
    uint16_t buttons;          // Digital button states mask
    int16_t accel[3];          // Accelerometer x, y, z raw data (10-bit resolution)
    int16_t gyro[3];           // MotionPlus Gyroscope pitch, roll, yaw raw data (14-bit)
    uint16_t ir_pointer[2];    // IR Camera tracking coordinates (X, Y)
};
#pragma pack(pop)

std::atomic<bool> g_running(true);

// High-precision sleep function using spinlocks + standard chrono sleep
void HighPrecisionSleep(std::chrono::microseconds duration) {
    auto start = std::chrono::high_resolution_clock::now();
    // Sleep for the majority of the time to yield core to OS
    if (duration > std::chrono::milliseconds(1)) {
        std::this_thread::sleep_for(duration - std::chrono::microseconds(500));
    }
    // Spin lock for ultra-precise sub-millisecond resolution
    while (std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::high_resolution_clock::now() - start) < duration) {
        #if defined(_MSC_VER)
        _mm_pause();
        #elif defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
        #elif defined(__arm__) || defined(__aarch64__)
        asm volatile("yield");
        #endif
    }
}

/**
 * Parses raw HID report packets received from the Nintendo Wii Remote.
 * Compatible with standard reporting modes: 0x30, 0x31, 0x33, 0x35.
 */
void ParseHIDReport(const unsigned char* buf, int bytes_read, WiiRemoteReport& report) {
    if (bytes_read < 3) return;

    // Byte 0 is Report ID
    uint8_t report_id = buf[0];

    // 1. Mapped Digital Buttons: Align physical reports with Dolphin's expected bitmasks
    uint16_t mapped_buttons = 0;
    // buf[1] contains: D-pad Left (bit 0), Right (bit 1), Down (bit 2), Up (bit 3), Plus (bit 4)
    if (buf[1] & 0x01) mapped_buttons |= 0x0001; // PAD_LEFT
    if (buf[1] & 0x02) mapped_buttons |= 0x0002; // PAD_RIGHT
    if (buf[1] & 0x04) mapped_buttons |= 0x0004; // PAD_DOWN
    if (buf[1] & 0x08) mapped_buttons |= 0x0008; // PAD_UP
    if (buf[1] & 0x10) mapped_buttons |= 0x0010; // BUTTON_PLUS

    // buf[2] contains: Two (bit 0), One (bit 1), B (bit 2), A (bit 3), Minus (bit 4), Home (bit 7)
    if (buf[2] & 0x01) mapped_buttons |= 0x0100; // BUTTON_TWO
    if (buf[2] & 0x02) mapped_buttons |= 0x0200; // BUTTON_ONE
    if (buf[2] & 0x04) mapped_buttons |= 0x0400; // BUTTON_B
    if (buf[2] & 0x08) mapped_buttons |= 0x0800; // BUTTON_A
    if (buf[2] & 0x10) mapped_buttons |= 0x1000; // BUTTON_MINUS
    if (buf[2] & 0x80) mapped_buttons |= 0x8000; // BUTTON_HOME

    report.buttons = mapped_buttons;

    // 2. Accelerometer: Bytes 3, 4, 5 (10-bit precision)
    if (report_id == 0x31 || report_id == 0x33 || report_id == 0x35 || report_id == 0x37) {
        if (bytes_read >= 6) {
            uint8_t x_msb = buf[3];
            uint8_t y_msb = buf[4];
            uint8_t z_msb = buf[5];

            // Extract LSBs from the button bytes
            uint8_t x_lsb = (buf[1] >> 5) & 0x03; // Bits 5 and 6
            uint8_t y_lsb = (buf[2] >> 5) & 0x01; // Bit 5
            uint8_t z_lsb = (buf[2] >> 6) & 0x01; // Bit 6

            // Reconstruct 10-bit raw value
            report.accel[0] = (x_msb << 2) | x_lsb;
            report.accel[1] = (y_msb << 2) | y_lsb;
            report.accel[2] = (z_msb << 2) | z_lsb;
        }
    }

    // 3. IR Camera (12-byte Mode 0x33)
    if (report_id == 0x33) {
        if (bytes_read >= 12) {
            // Blob 1: bytes 6, 7, 8
            uint8_t b1_x_msb = buf[6];
            uint8_t b1_y_msb = buf[7];
            uint8_t b1_lsb   = buf[8];
            
            // Blob 2: bytes 9, 10, 11
            uint8_t b2_x_msb = buf[9];
            uint8_t b2_y_msb = buf[10];
            uint8_t b2_lsb   = buf[11];

            bool blob1_active = !(b1_x_msb == 0xFF && b1_y_msb == 0xFF);
            bool blob2_active = !(b2_x_msb == 0xFF && b2_y_msb == 0xFF);

            uint16_t blob1_x = 0, blob1_y = 0;
            uint16_t blob2_x = 0, blob2_y = 0;

            if (blob1_active) {
                blob1_x = (b1_x_msb << 2) | ((b1_lsb >> 4) & 0x03);
                blob1_y = (b1_y_msb << 2) | ((b1_lsb >> 6) & 0x03);
            }
            if (blob2_active) {
                blob2_x = (b2_x_msb << 2) | ((b2_lsb >> 4) & 0x03);
                blob2_y = (b2_y_msb << 2) | ((b2_lsb >> 6) & 0x03);
            }

            // Interpolate coordinates to emulate a pointer centered between sensor bar lights
            if (blob1_active && blob2_active) {
                report.ir_pointer[0] = (blob1_x + blob2_x) / 2;
                report.ir_pointer[1] = (blob1_y + blob2_y) / 2;
            } else if (blob1_active) {
                report.ir_pointer[0] = blob1_x;
                report.ir_pointer[1] = blob1_y;
            } else {
                // Centered default
                report.ir_pointer[0] = 512;
                report.ir_pointer[1] = 384;
            }
        }
    }

    // 4. MotionPlus Gyroscope (14-bit data in extension payload, mode 0x35)
    if (report_id == 0x35) {
        // Extension payload resides at buf[6] through buf[21] (16 bytes)
        if (bytes_read >= 12) {
            uint8_t yaw_lsb   = buf[6];
            uint8_t pitch_lsb = buf[7];
            uint8_t roll_lsb  = buf[8];
            uint8_t yaw_msb   = buf[9];
            uint8_t pitch_msb = buf[10];
            uint8_t roll_msb  = buf[11];

            // Verify that extension reports MotionPlus data (DF / Data Format bit must be 1)
            bool mplus_data = (roll_msb & 0x02) != 0;

            if (mplus_data) {
                // Extrapolate 14-bit gyroscope values
                uint16_t yaw_raw   = ((yaw_msb & 0xFC) << 6)   | yaw_lsb;
                uint16_t pitch_raw = ((pitch_msb & 0xFC) << 6) | pitch_lsb;
                uint16_t roll_raw  = ((roll_msb & 0xFC) << 6)  | roll_lsb;

                // MotionPlus features Fast/Slow velocity reporting (Slow = higher resolution scaling)
                bool yaw_slow   = (yaw_msb & 0x02) != 0;
                bool pitch_slow = (pitch_msb & 0x02) != 0;
                bool roll_slow  = (roll_msb & 0x01) != 0;

                // Calibrate angular rates relative to baseline calibration offset (typically 8192)
                report.gyro[0] = static_cast<int16_t>(pitch_raw) - 8192; // Pitch
                report.gyro[1] = static_cast<int16_t>(roll_raw) - 8192;  // Roll
                report.gyro[2] = static_cast<int16_t>(yaw_raw) - 8192;   // Yaw

                // Normalize velocity based on speed range mode
                if (!pitch_slow) report.gyro[0] *= 5; // Fast mode multiplier
                if (!roll_slow)  report.gyro[1] *= 5;
                if (!yaw_slow)   report.gyro[2] *= 5;
            }
        }
    }
}

/**
 * Mock Data Generator: Mimics physical Wii Remote motions.
 * Simulates pitching and swinging states for development without local hardware.
 */
void GenerateMockWiiRemoteReport(WiiRemoteReport& report, uint64_t frame) {
    report.buttons = 0x0000;
    report.sequence = static_cast<uint32_t>(frame + 1); // 1-indexed sequence
    
    // Simulate swing / pitch motion values using sinusoidal time vectors
    double t = frame * 0.001; // 1ms polling steps
    
    // Core gravity baseline (accel Z = 512 corresponds to 1G)
    report.accel[0] = static_cast<int16_t>(512.0 + 100.0 * std::sin(2 * M_PI * t * 0.5));
    report.accel[1] = static_cast<int16_t>(512.0 + 50.0 * std::cos(2 * M_PI * t * 0.2));
    report.accel[2] = static_cast<int16_t>(512.0 + 20.0 * std::sin(2 * M_PI * t * 1.0));

    // Emulate occasional swings
    if ((frame / 2000) % 2 == 0) { // Every 2 seconds
        // Active swing phase
        double swing_t = (frame % 2000) * 0.001;
        if (swing_t < 0.3) { // 300ms swing duration
            report.gyro[0] = static_cast<int16_t>(2000.0 * std::sin(M_PI * swing_t / 0.3)); // Sudden rotational spikes
            report.accel[0] = static_cast<int16_t>(512 + 800 * std::sin(M_PI * swing_t / 0.3));
            report.buttons |= 0x0800; // Simulate holding 'A' button during swing
        } else {
            report.gyro[0] = 0;
        }
    } else {
        report.gyro[0] = 0;
    }

    report.gyro[1] = 0;
    report.gyro[2] = 0;

    // Simulate standard hand shake/jitter on the IR pointer
    report.ir_pointer[0] = static_cast<uint16_t>(512 + 15 * std::sin(2 * M_PI * t * 3.0));
    report.ir_pointer[1] = static_cast<uint16_t>(384 + 10 * std::cos(2 * M_PI * t * 2.5));
}

// Thread responsible for polling the physical or virtual Wii Remote via Bluetooth
void BluetoothPollingThread(SOCKET udp_socket, sockaddr_in target_addr) {
    std::cout << "[Proxy] Starting Bluetooth Polling Thread at 1000Hz..." << std::endl;

    WiiRemoteReport persistent_report;
    std::memset(&persistent_report, 0, sizeof(WiiRemoteReport));
    persistent_report.ir_pointer[0] = 512;
    persistent_report.ir_pointer[1] = 384;

#if HARDWARE_SUPPORT_ENABLED
    std::cout << "[Proxy] HIDAPI compilation detected. Scanning for hardware..." << std::endl;
    if (hid_init() != 0) {
        std::cerr << "[Proxy Error] Failed to initialize hidapi!" << std::endl;
        return;
    }

    // Vendor ID: 0x057e (Nintendo), Product ID: 0x0306 (Wii Remote)
    hid_device* wii_remote = hid_open(0x057e, 0x0306, NULL);
    if (!wii_remote) {
        std::cerr << "[Proxy Warning] Physical Wii Remote not detected. Falling back to Mock Engine." << std::endl;
    } else {
        std::cout << "[Proxy Success] Wii Remote hardware connection established!" << std::endl;
        
        // Set non-blocking mode to keep loop spinning at exactly 1000Hz
        hid_set_nonblocking(wii_remote, 1);
        
        // -----------------------------------------------------------------
        //               WII REMOTE IR CAMERA INITIALIZATION SEQUENCE
        // -----------------------------------------------------------------
        // Step 1: Enable IR Pixel Clock (Report 0x13 -> exact size 2 bytes)
        unsigned char ir_cmd1[2] = { 0x13, 0x04 };
        if (hid_write(wii_remote, ir_cmd1, 2) < 0) {
            std::cerr << "[Proxy Warning] Step 1 (IR Pixel Clock Enable 0x13) write failed!" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Delay for hardware clock stability
        
        // Step 2: Enable IR Logic (Report 0x1a -> exact size 2 bytes)
        unsigned char ir_cmd2[2] = { 0x1a, 0x04 };
        if (hid_write(wii_remote, ir_cmd2, 2) < 0) {
            std::cerr << "[Proxy Warning] Step 2 (IR Logic Enable 0x1a) write failed!" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        // Step 3: Write 0x01 to Register 0x04b00030 (Power on Camera, Report 0x16 -> size 22)
        unsigned char ir_cmd3[22] = { 0 };
        ir_cmd3[0] = 0x16; // Memory/Register write
        ir_cmd3[1] = 0x04; // Register write flag
        ir_cmd3[2] = 0x04; // Offset 24-16
        ir_cmd3[3] = 0xb0; // Offset 15-8
        ir_cmd3[4] = 0x30; // Offset 7-0 (register 0x30)
        ir_cmd3[5] = 0x01; // Write size = 1
        ir_cmd3[6] = 0x01; // Value = 0x01 (Power on camera)
        if (hid_write(wii_remote, ir_cmd3, sizeof(ir_cmd3)) < 0) {
            std::cerr << "[Proxy Warning] Step 3 (Camera Register Write 0x30) write failed!" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Step 4: Write Sensitivity Block 1 to Register 0x04b00000 (Level 3 sensitivity)
        unsigned char ir_cmd4[22] = { 0 };
        ir_cmd4[0] = 0x16;
        ir_cmd4[1] = 0x04;
        ir_cmd4[2] = 0x04;
        ir_cmd4[3] = 0xb0;
        ir_cmd4[4] = 0x00; // register 0x00
        ir_cmd4[5] = 0x09; // Write size = 9
        ir_cmd4[6] = 0x02;
        ir_cmd4[7] = 0x00;
        ir_cmd4[8] = 0x00;
        ir_cmd4[9] = 0x71;
        ir_cmd4[10] = 0x01;
        ir_cmd4[11] = 0x00;
        ir_cmd4[12] = 0xaa;
        ir_cmd4[13] = 0x00;
        ir_cmd4[14] = 0x64;
        if (hid_write(wii_remote, ir_cmd4, sizeof(ir_cmd4)) < 0) {
            std::cerr << "[Proxy Warning] Step 4 (Sensitivity Block 1 Write 0x00) write failed!" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Step 5: Write Sensitivity Block 2 to Register 0x04b0001a
        unsigned char ir_cmd5[22] = { 0 };
        ir_cmd5[0] = 0x16;
        ir_cmd5[1] = 0x04;
        ir_cmd5[2] = 0x04;
        ir_cmd5[3] = 0xb0;
        ir_cmd5[4] = 0x1a; // register 0x1a
        ir_cmd5[5] = 0x02; // Write size = 2
        ir_cmd5[6] = 0x63;
        ir_cmd5[7] = 0x03;
        if (hid_write(wii_remote, ir_cmd5, sizeof(ir_cmd5)) < 0) {
            std::cerr << "[Proxy Warning] Step 5 (Sensitivity Block 2 Write 0x1a) write failed!" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Step 6: Write Mode 0x03 (Extended Mode) to Register 0x04b00033
        unsigned char ir_cmd6[22] = { 0 };
        ir_cmd6[0] = 0x16;
        ir_cmd6[1] = 0x04;
        ir_cmd6[2] = 0x04;
        ir_cmd6[3] = 0xb0;
        ir_cmd6[4] = 0x33; // register 0x33
        ir_cmd6[5] = 0x01; // Write size = 1
        ir_cmd6[6] = 0x03; // Value = 0x03 (Extended)
        if (hid_write(wii_remote, ir_cmd6, sizeof(ir_cmd6)) < 0) {
            std::cerr << "[Proxy Warning] Step 6 (Extended Mode Write 0x33) write failed!" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Step 7: Write 0x08 to Register 0x04b00030 again (Finalize Power On)
        unsigned char ir_cmd7[22] = { 0 };
        ir_cmd7[0] = 0x16;
        ir_cmd7[1] = 0x04;
        ir_cmd7[2] = 0x04;
        ir_cmd7[3] = 0xb0;
        ir_cmd7[4] = 0x30; // register 0x30
        ir_cmd7[5] = 0x01; // Write size = 1
        ir_cmd7[6] = 0x08;
        if (hid_write(wii_remote, ir_cmd7, sizeof(ir_cmd7)) < 0) {
            std::cerr << "[Proxy Warning] Step 7 (Final Power Write 0x30) write failed!" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        // Step 8: Configure Wii Remote: Enable continuous reporting and set mode to 0x33 (exact size 3 bytes)
        unsigned char config_cmd[3] = { 0x12, 0x04, 0x33 };
        if (hid_write(wii_remote, config_cmd, 3) < 0) {
            std::cerr << "[Proxy Warning] Step 8 (Reporting Mode Write 0x12) write failed!" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Step 9: Request Status Update (Report 0x15 -> exact size 2 bytes) to force state synchronization
        unsigned char status_cmd[2] = { 0x15, 0x00 };
        if (hid_write(wii_remote, status_cmd, 2) < 0) {
            std::cerr << "[Proxy Warning] Step 9 (Status Request 0x15) write failed!" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        std::cout << "[Proxy Success] Wii Remote IR Camera initialized successfully!" << std::endl;
    }
#else
    std::cout << "[Proxy] Pure software compilation active (Mock Simulator enabled)." << std::endl;
#endif

    bool use_mouse_passthrough = false;
#if HARDWARE_SUPPORT_ENABLED
    if (!wii_remote) {
        std::cout << "[Proxy Success] Automatically switched to High-Precision Mouse & Keyboard Passthrough!" << std::endl;
        std::cout << "[Proxy Success] Set your Mayflash DolphinBar to Mode 2 (Mouse Mode) and point remote at screen." << std::endl;
        use_mouse_passthrough = true;
    }
#else
    use_mouse_passthrough = false;
#endif

    uint64_t frame_count = 0;
    auto last_time = std::chrono::high_resolution_clock::now();
    int last_bytes_read = 0;
    unsigned char last_buf[22] = { 0 };

    while (g_running) {
        auto loop_start = std::chrono::high_resolution_clock::now();

        // Update timestamps and sequences in persistent report
        persistent_report.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            loop_start.time_since_epoch()).count();
        persistent_report.sequence = static_cast<uint32_t>(frame_count + 1);

#if HARDWARE_SUPPORT_ENABLED
        if (wii_remote) {
            unsigned char buf[22];
            std::memset(buf, 0, sizeof(buf));
            
            // Poll standard Bluetooth reports non-blocking to prevent Windows scheduler latency
            int bytes_read = hid_read(wii_remote, buf, sizeof(buf));
            if (bytes_read > 0) {
                last_bytes_read = bytes_read;
                std::memcpy(last_buf, buf, bytes_read);
                ParseHIDReport(buf, bytes_read, persistent_report);
            } else if (bytes_read < 0) {
                if (frame_count % 1000 == 0) {
                    std::cerr << "[Proxy Error] Wii Remote read failure or disconnected! (hid_read returned: " << bytes_read << ")" << std::endl;
                }
            }
        } else if (use_mouse_passthrough) {
            // 1. Read Windows Desktop Mouse Cursor (which is moved by the physical remote in Mode 2)
            POINT p;
            if (GetCursorPos(&p)) {
                int screen_width = GetSystemMetrics(SM_CXSCREEN);
                int screen_height = GetSystemMetrics(SM_CYSCREEN);
                if (screen_width > 0 && screen_height > 0) {
                    persistent_report.ir_pointer[0] = static_cast<uint16_t>((p.x * 1023) / screen_width);
                    persistent_report.ir_pointer[1] = static_cast<uint16_t>((p.y * 767) / screen_height);
                }
            }

            // 2. Read Windows Keyboard / Mouse Button states mapped to Wiimote
            uint16_t mapped_buttons = 0;
            if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) mapped_buttons |= 0x0800; // Left Click (Wii A) -> BUTTON_A
            if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) mapped_buttons |= 0x0400; // Right Click (Wii B) -> BUTTON_B
            if (GetAsyncKeyState(VK_LEFT) & 0x8000)    mapped_buttons |= 0x0001; // Left Arrow -> PAD_LEFT
            if (GetAsyncKeyState(VK_RIGHT) & 0x8000)   mapped_buttons |= 0x0002; // Right Arrow -> PAD_RIGHT
            if (GetAsyncKeyState(VK_DOWN) & 0x8000)    mapped_buttons |= 0x0004; // Down Arrow -> PAD_DOWN
            if (GetAsyncKeyState(VK_UP) & 0x8000)      mapped_buttons |= 0x0008; // Up Arrow -> PAD_UP
            if (GetAsyncKeyState(VK_RETURN) & 0x8000)  mapped_buttons |= 0x0010; // Enter -> Plus
            if (GetAsyncKeyState(VK_BACK) & 0x8000)    mapped_buttons |= 0x1000; // Backspace -> Minus
            if (GetAsyncKeyState(0x31) & 0x8000)       mapped_buttons |= 0x0200; // Key 1 -> 1
            if (GetAsyncKeyState(0x32) & 0x8000)       mapped_buttons |= 0x0100; // Key 2 -> 2
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)   mapped_buttons |= 0x8000; // Escape -> Home

            persistent_report.buttons = mapped_buttons;

            // 3. Motion/Swing Detection: Calculate mouse velocity and trigger swing on fast flicks!
            static int last_x = 0;
            static int last_y = 0;
            int dx = p.x - last_x;
            int dy = p.y - last_y;
            last_x = p.x;
            last_y = p.y;

            double velocity = std::sqrt(dx*dx + dy*dy);
            static int swing_frames_left = 0;

            // Trigger physical swing/pitch report on raw remote wrist flicks
            if (velocity > 60.0 && swing_frames_left == 0) { // Flick threshold
                swing_frames_left = 300; // 300ms swing duration
            }

            if (swing_frames_left > 0) {
                persistent_report.gyro[0] = 2000; // Spiking angular velocity
                persistent_report.accel[0] = 512 + 800; // Sudden G-Force spike
                persistent_report.buttons |= 0x0800; // Auto-hold A button during swing
                swing_frames_left--;
            } else {
                persistent_report.gyro[0] = 0;
                persistent_report.accel[0] = 512; // Base gravity (1G)
            }
            persistent_report.gyro[1] = 0;
            persistent_report.gyro[2] = 0;
            persistent_report.accel[1] = 512;
            persistent_report.accel[2] = 512;
        } else {
            GenerateMockWiiRemoteReport(persistent_report, frame_count);
        }
#else
        GenerateMockWiiRemoteReport(persistent_report, frame_count);
#endif

        // Serialize and send UDP packet to custom Dolphin Fork API
        int bytes_sent = sendto(udp_socket, reinterpret_cast<const char*>(&persistent_report), sizeof(persistent_report), 0,
                                reinterpret_cast<struct sockaddr*>(&target_addr), sizeof(target_addr));
        
        if (bytes_sent == SOCKET_ERROR) {
            std::cerr << "[Proxy Error] Failed to transmit UDP input report!" << std::endl;
        }

        frame_count++;
        if (frame_count % 1000 == 0) {
            auto current_time = std::chrono::high_resolution_clock::now();
            double duration_sec = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_time).count() / 1000.0;
            std::cout << "[Proxy Status] Active. Frame: " << frame_count 
                      << " | Polled 1000 packets in " << duration_sec << "s (Target: 1.0s) | Sample Accel X: " 
                      << persistent_report.accel[0] << std::endl;
            
#if HARDWARE_SUPPORT_ENABLED
            if (use_mouse_passthrough) {
                std::cout << "               [Passthrough Log] Active | Mapped X/Y: (" 
                          << persistent_report.ir_pointer[0] << ", " 
                          << persistent_report.ir_pointer[1] << ") | Accel X: "
                          << persistent_report.accel[0] << std::endl;
            } else if (wii_remote && last_bytes_read > 0) {
                std::cout << "               [Telemetry Log] Last bytes read: " << last_bytes_read 
                          << " | Report ID: 0x" << std::hex << (int)last_buf[0] << std::dec << std::endl;
                std::cout << "               [Raw Bytes] ";
                for (int i = 0; i < last_bytes_read; ++i) {
                    std::cout << std::hex << (int)last_buf[i] << " " << std::dec;
                }
                std::cout << std::endl;
                
                // Parse blob statuses for terminal feedback
                bool b1_active = (last_bytes_read >= 9) && !(last_buf[6] == 0xFF && last_buf[7] == 0xFF);
                bool b2_active = (last_bytes_read >= 12) && !(last_buf[9] == 0xFF && last_buf[10] == 0xFF);
                std::cout << "               [IR Diagnostics] Blob1: " << (b1_active ? "ACTIVE" : "INACTIVE")
                          << " | Blob2: " << (b2_active ? "ACTIVE" : "INACTIVE") 
                          << " | Mapped X/Y: (" << persistent_report.ir_pointer[0] << ", " 
                          << persistent_report.ir_pointer[1] << ")" << std::endl;
            }
#endif
            last_time = current_time;
        }

        // Maintain strict 1000Hz cycle (1ms / 1000us intervals)
        auto loop_duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - loop_start);
        
        if (loop_duration < std::chrono::microseconds(1000)) {
            HighPrecisionSleep(std::chrono::microseconds(1000) - loop_duration);
        }
    }

#if HARDWARE_SUPPORT_ENABLED
    if (wii_remote) {
        hid_close(wii_remote);
    }
    hid_exit();
#endif

    std::cout << "[Proxy] Bluetooth Polling Thread terminated." << std::endl;
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "Mario Super Sluggers Netplay: C++ Bluetooth Proxy" << std::endl;
    std::cout << "==================================================" << std::endl;

    // Platform-specific socket startup
#if defined(_WIN32)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "Winsock initialization failed!" << std::endl;
        return 1;
    }
#endif

    // Setup client UDP socket to talk to Dolphin
    SOCKET udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_socket == INVALID_SOCKET) {
        std::cerr << "Failed to create UDP socket!" << std::endl;
        return 1;
    }

    sockaddr_in dolphin_addr;
    dolphin_addr.sin_family = AF_INET;
    dolphin_addr.sin_port = htons(5555); // Custom Netplay Input Injection Port
    inet_pton(AF_INET, "127.0.0.1", &dolphin_addr.sin_addr);

    // Launch High-Frequency loop
    std::thread poll_thread(BluetoothPollingThread, udp_socket, dolphin_addr);

    std::cout << "Press ENTER to stop the proxy service..." << std::endl;
    std::cin.get();

    g_running = false;
    if (poll_thread.joinable()) {
        poll_thread.join();
    }

    // Clean up
#if defined(_WIN32)
    closesocket(udp_socket);
    WSACleanup();
#else
    close(udp_socket);
#endif

    std::cout << "[Proxy] Service successfully closed." << std::endl;
    return 0;
}
