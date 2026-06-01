#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <cstdint>
#include <cstring>

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

// Include hidapi placeholder
// In full implementation, download or link hidapi (https://github.com/libusb/hidapi)
// #include <hidapi.h>

// Standard Wii Remote HID Report Payload structure (UDP package)
#pragma pack(push, 1)
struct WiiRemoteReport {
    uint64_t timestamp_us;     // High-resolution timestamp in microseconds
    uint16_t buttons;          // Digital button states mask
    int16_t accel[3];          // Accelerometer x, y, z raw data
    int16_t gyro[3];           // MotionPlus Gyroscope pitch, roll, yaw raw data
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

// Thread responsible for polling the local physical Wii Remote via Bluetooth HID
void BluetoothPollingThread(SOCKET udp_socket, sockaddr_in target_addr) {
    std::cout << "[Proxy] Starting Bluetooth Polling Thread at 1000Hz..." << std::endl;

    // TODO: Initialize HIDAPI and open the Wii Remote device
    // hid_device* wii_remote = hid_open(0x057e, 0x0306, NULL); // Nintendo Wii Remote Vendor/Product IDs
    
    uint64_t frame_count = 0;
    auto last_time = std::chrono::high_resolution_clock::now();

    while (g_running) {
        auto loop_start = std::chrono::high_resolution_clock::now();

        // 1. Poll the Bluetooth HID reports
        WiiRemoteReport report;
        std::memset(&report, 0, sizeof(WiiRemoteReport));
        
        report.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            loop_start.time_since_epoch()).count();
        
        // Mock data for initial structure test
        report.buttons = 0x0000; // No buttons pressed
        report.accel[0] = 0; report.accel[1] = 512; report.accel[2] = 0; // Baseline gravity
        report.gyro[0] = 0; report.gyro[1] = 0; report.gyro[2] = 0;
        report.ir_pointer[0] = 512; report.ir_pointer[1] = 384; // Centered pointer

        // TODO: Read raw inputs from physical device
        // unsigned char buf[22];
        // int bytes_read = hid_read_timeout(wii_remote, buf, sizeof(buf), 1);
        // if (bytes_read > 0) { ParseHIDReport(buf, report); }

        // 2. Serialize and send UDP packet to custom Dolphin Fork API
        int bytes_sent = sendto(udp_socket, reinterpret_cast<const char*>(&report), sizeof(report), 0,
                                reinterpret_cast<struct sockaddr*>(&target_addr), sizeof(target_addr));
        
        if (bytes_sent == SOCKET_ERROR) {
            std::cerr << "[Proxy Error] Failed to transmit UDP input report!" << std::endl;
        }

        frame_count++;
        if (frame_count % 1000 == 0) {
            auto current_time = std::chrono::high_resolution_clock::now();
            double duration_sec = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_time).count() / 1000.0;
            std::cout << "[Proxy Status] Active. Sent 1000 reports in " << duration_sec << "s (Target: 1.0s)" << std::endl;
            last_time = current_time;
        }

        // Maintain strict 1000Hz cycle (1ms / 1000us intervals)
        auto loop_duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - loop_start);
        
        if (loop_duration < std::chrono::microseconds(1000)) {
            HighPrecisionSleep(std::chrono::microseconds(1000) - loop_duration);
        }
    }

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
