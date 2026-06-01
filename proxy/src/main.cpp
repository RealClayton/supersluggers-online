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
        #if defined(__x86_64__) || defined(_M_X64)
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

    // 1. Core Buttons: Bytes 1 & 2
    // Mask out the accelerometer LSBs (bits 5 & 6) from button bytes
    uint16_t button1 = buf[1] & 0x1F; // Mask: Left, Right, Down, Up, Plus
    uint16_t button2 = buf[2] & 0x9F; // Mask: Two, One, B, A, Minus, Home (bit 7)
    report.buttons = (button1 << 8) | button2;

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
            report.buttons |= 0x0008; // Simulate holding 'A' button during swing
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
        
        // Configure Wii Remote: Enable continuous reporting and set mode to 0x33 (Buttons, Accel, 12-byte IR)
        // Command: 0x12 -> reporting mode write. 0x04 -> continuous flag. 0x33 -> Report ID target.
        unsigned char config_cmd[3] = { 0x12, 0x04, 0x33 };
        hid_write(wii_remote, config_cmd, sizeof(config_cmd));
    }
#else
    std::cout << "[Proxy] Pure software compilation active (Mock Simulator enabled)." << std::endl;
#endif

    uint64_t frame_count = 0;
    auto last_time = std::chrono::high_resolution_clock::now();

    while (g_running) {
        auto loop_start = std::chrono::high_resolution_clock::now();

        WiiRemoteReport report;
        std::memset(&report, 0, sizeof(WiiRemoteReport));
        
        report.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            loop_start.time_since_epoch()).count();

#if HARDWARE_SUPPORT_ENABLED
        if (wii_remote) {
            unsigned char buf[22];
            std::memset(buf, 0, sizeof(buf));
            
            // Poll standard Bluetooth reports with 1ms timeout
            int bytes_read = hid_read_timeout(wii_remote, buf, sizeof(buf), 1);
            if (bytes_read > 0) {
                ParseHIDReport(buf, bytes_read, report);
            }
        } else {
            GenerateMockWiiRemoteReport(report, frame_count);
        }
#else
        GenerateMockWiiRemoteReport(report, frame_count);
#endif

        // Serialize and send UDP packet to custom Dolphin Fork API
        int bytes_sent = sendto(udp_socket, reinterpret_cast<const char*>(&report), sizeof(report), 0,
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
                      << report.accel[0] << std::endl;
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
