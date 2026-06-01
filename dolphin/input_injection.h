#pragma once

#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

#if defined(_WIN32)
    #include <winsock2.h>
#else
    typedef int SOCKET;
#endif

// Packet matching the C++ Bluetooth Proxy report structure
#pragma pack(push, 1)
struct ProxyInputReport {
    uint64_t timestamp_us;     // High-resolution timestamp
    uint16_t buttons;          // Core buttons bitmask
    int16_t accel[3];          // 10-bit Accelerometer vectors
    int16_t gyro[3];           // 14-bit MotionPlus Gyroscope vectors
    uint16_t ir_pointer[2];    // 10-bit IR cursor coordinates
};
#pragma pack(pop)

// Interpolated Wii Remote state to feed into Dolphin's emulated hardware loop
struct EmulatedWiimoteState {
    uint16_t buttons;
    float accel[3];            // Normalized G-force float values
    float gyro[3];             // Normalized angular velocity float values
    float ir_pointer[2];       // Emulated pointer coordinates normalized (0.0 to 1.0)
};

/**
 * High-performance, low-latency Jitter Buffer Queue utilizing
 * linear and Hermite spline interpolation for late packets.
 */
class AdaptiveJitterBuffer {
public:
    AdaptiveJitterBuffer();
    ~AdaptiveJitterBuffer();

    // Inserts a raw report from the network proxy thread
    void PushReport(const ProxyInputReport& report);

    // Pulls the interpolated input state for the current emulation frame
    EmulatedWiimoteState PullState(uint64_t current_time_us);

    // Clear the buffer queue on game reset/load
    void Reset();

private:
    std::mutex m_mutex;
    std::vector<ProxyInputReport> m_queue;
    
    // History trackers for Hermite spline interpolation
    ProxyInputReport m_last_valid_report;
    ProxyInputReport m_second_last_report;
    
    uint64_t m_buffer_delay_us; // Dynamic target buffering delay based on network jitter
    
    // Interpolation Helpers
    ProxyInputReport InterpolateLinear(const ProxyInputReport& p1, const ProxyInputReport& p2, double t);
    void InterpolateHermiteIR(const ProxyInputReport& p0, const ProxyInputReport& p1, 
                              const ProxyInputReport& p2, const ProxyInputReport& p3, 
                              double t, float out_ir[2]);
};

/**
 * Dynamic Host Authority Manager
 * Monitors game states (from Gecko RAM hooks) and negotiates input ownership.
 */
enum class NetplayAuthority {
    LOCAL_ONLY,       // Single-player / standard local connection
    PITCHER_CONTROL,  // Pitching team holds local 0ms authority
    BATTER_CONTROL    // Batting/Fielding team holds local 0ms authority
};

class HostAuthorityManager {
public:
    HostAuthorityManager();
    
    // Updates local authority state based on RAM state register (Gecko Hooks)
    void UpdateStateFromMemoryRegister(uint8_t memory_reg_val);
    
    // Returns true if this instance should immediately execute local inputs (0ms delay)
    bool HasLocalAuthority() const;
    
    // Returns current authority type
    NetplayAuthority GetAuthorityMode() const { return m_mode; }

private:
    NetplayAuthority m_mode;
    bool m_is_offense_team; // True if this client is batting/running
};

/**
 * Background daemon listening on UDP port 5555 for proxy packets
 * and feeding them into the active emulator input loops.
 */
class NetplayInputReceiver {
public:
    static NetplayInputReceiver& GetInstance();

    void Start(uint16_t port = 5555);
    void Stop();

    // Invoked by Dolphin's emulated Wiimote polling tick (Core/HW/Wiimote.cpp)
    EmulatedWiimoteState GetEmulatedState();

    // Updates memory register states from Gecko core hooks
    void SyncGeckoState(uint8_t state_reg);

private:
    NetplayInputReceiver();
    ~NetplayInputReceiver();

    void ListeningLoop();

    std::thread m_listener_thread;
    std::atomic<bool> m_running;
    uint16_t m_port;
    SOCKET m_socket;

    AdaptiveJitterBuffer m_jitter_buffer;
    HostAuthorityManager m_authority_manager;
};
