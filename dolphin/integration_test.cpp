#define _USE_MATH_DEFINES
#include "input_injection.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cassert>
#include <cmath>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <intrin.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    typedef int SOCKET;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
#endif

// Shared lifecycle flag
std::atomic<bool> g_sender_running(true);

// Re-use standard Wii Remote packet structure for sender mock
#pragma pack(push, 1)
struct SenderMockReport {
    uint64_t timestamp_us;
    uint32_t sequence;
    uint16_t buttons;
    int16_t accel[3];
    int16_t gyro[3];
    uint16_t ir_pointer[2];
};
#pragma pack(pop)

// Precise Sleep helper
void PrecisionSleep(std::chrono::microseconds duration) {
    auto start = std::chrono::high_resolution_clock::now();
    if (duration > std::chrono::milliseconds(1)) {
        std::this_thread::sleep_for(duration - std::chrono::microseconds(500));
    }
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

// Emulates the Phase 2 Bluetooth Proxy UDP sender running on a 1000Hz thread
void IntegrationSenderThread(SOCKET client_socket, sockaddr_in target_addr) {
    std::cout << "[Integration Test] Launching Mock 1000Hz C++ UDP Sender..." << std::endl;
    
    uint64_t frame = 0;
    while (g_sender_running) {
        auto loop_start = std::chrono::high_resolution_clock::now();

        SenderMockReport report;
        report.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            loop_start.time_since_epoch()).count();
        report.sequence = static_cast<uint32_t>(frame);
        
        // Emulate a smooth circular motion on the IR pointer coordinates
        double t = frame * 0.001; // 1ms intervals
        report.buttons = (frame % 100 < 5) ? 0x0008 : 0x0000; // Hold A button occasionally
        report.accel[0] = static_cast<int16_t>(512 + 100 * std::sin(2 * M_PI * t * 2.0)); // Accelerometer curve
        report.accel[1] = 512;
        report.accel[2] = 512;
        report.gyro[0] = 0; report.gyro[1] = 0; report.gyro[2] = 0;
        report.ir_pointer[0] = static_cast<uint16_t>(512 + 200 * std::sin(2 * M_PI * t * 1.0)); // Circular X
        report.ir_pointer[1] = static_cast<uint16_t>(384 + 150 * std::cos(2 * M_PI * t * 1.0)); // Circular Y

        sendto(client_socket, reinterpret_cast<const char*>(&report), sizeof(report), 0,
               reinterpret_cast<struct sockaddr*>(&target_addr), sizeof(target_addr));

        frame++;

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - loop_start);
        
        if (elapsed < std::chrono::microseconds(1000)) {
            PrecisionSleep(std::chrono::microseconds(1000) - elapsed);
        }
    }
    std::cout << "[Integration Test] Sender Thread shut down." << std::endl;
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "Sluggers Netplay: C++ Socket Integration Diagnostics" << std::endl;
    std::cout << "==================================================" << std::endl;

#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    // 1. Initialize Dolphin Fork UDP Receiver on Port 5555
    std::cout << "[Integration Test] Initializing custom Dolphin Fork receiver..." << std::endl;
    NetplayInputReceiver& receiver = NetplayInputReceiver::GetInstance();
    receiver.Start(5555);

    // 2. Setup C++ Client Socket for Sender
    SOCKET sender_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(sender_socket != INVALID_SOCKET);

    sockaddr_in target_addr;
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(5555);
    inet_pton(AF_INET, "127.0.0.1", &target_addr.sin_addr);

    // 3. Launch 1000Hz Sender Thread
    std::thread sender_thread(IntegrationSenderThread, sender_socket, target_addr);

    // Let the pipeline run for 2.0 seconds to fill buffers and stabilize threads
    std::cout << "[Integration Test] Processing socket data pipeline for 2.0 seconds..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // 4. Perform Emulation Tick Ingestion Test
    std::cout << "[Integration Test] Simulating active emulator frame ticks (GetEmulatedState())..." << std::endl;
    
    int successful_frames = 0;
    for (int frame = 0; frame < 10; ++frame) {
        EmulatedWiimoteState state = receiver.GetEmulatedState();
        
        // Assert that the coordinates and accelerometers contain parsed, interpolated data from our sender
        std::cout << "[Frame Tick " << frame << "] Extracted Accel X: " << state.accel[0] 
                  << " | IR X/Y pointer: (" << state.ir_pointer[0] << ", " << state.ir_pointer[1] << ")" << std::endl;
        
        // Emulated pointer coordinates should be normalized (0.0 to 1.0) and populated
        assert(state.ir_pointer[0] >= 0.0f && state.ir_pointer[0] <= 1.0f);
        assert(state.ir_pointer[1] >= 0.0f && state.ir_pointer[1] <= 1.0f);
        // Ensure we are receiving actual dynamic data, not fallback values (default is 0.5f and accel X is 0)
        assert(!(std::abs(state.ir_pointer[0] - 0.5f) < 0.001f && std::abs(state.ir_pointer[1] - 0.5f) < 0.001f && std::abs(state.accel[0]) < 0.001f));
        successful_frames++;
        
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // Simulate ~60fps frame steps
    }

    assert(successful_frames == 10);
    std::cout << "[Integration Test Success] Emulated frame input ticks verified." << std::endl;

    // 5. Graceful Teardown
    std::cout << "[Integration Test] Initiating teardown sequence..." << std::endl;
    g_sender_running = false;
    if (sender_thread.joinable()) {
        sender_thread.join();
    }

    receiver.Stop();
    
#if defined(_WIN32)
    closesocket(sender_socket);
    WSACleanup();
#else
    close(sender_socket);
#endif

    std::cout << "\n==================================================" << std::endl;
    std::cout << "All Socket Integration Diagnostics: PASSED!" << std::endl;
    std::cout << "==================================================" << std::endl;

    return 0;
}
