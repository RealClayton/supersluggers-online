#pragma once

#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
#else
    typedef int SOCKET;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
#endif

// Packet matching the C++ Bluetooth Proxy report structure
#pragma pack(push, 1)
struct ProxyInputReport {
    uint64_t timestamp_us;     // High-resolution timestamp
    uint32_t sequence;         // Monotonic packet sequence number for drop detection
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

    // Pulls the latest unbuffered raw report for 0ms visual prediction rendering
    ProxyInputReport GetLatestRawReport() const;

    // Clear the buffer queue on game reset/load
    void Reset();

    // Diagnostic accessors
    uint32_t GetDroppedPacketCount() const { return m_dropped_packets; }
    uint32_t GetOutOfOrderCount() const { return m_out_of_order_packets; }
    uint64_t GetBufferDelayUs() const { return m_buffer_delay_us; }

    // Controls whether to bypass jitter delay for 0ms local input (Offense/Batter mode)
    void SetAuthorityBypass(bool bypass) { m_authority_bypass = bypass; }
    bool GetAuthorityBypass() const { return m_authority_bypass; }

private:
    mutable std::mutex m_mutex;
    std::vector<ProxyInputReport> m_queue;
    bool m_authority_bypass; // In asymmetric mode, true = offense (0ms delay), false = defense (buffered RTT delay)
    
    // History trackers for Hermite spline interpolation
    ProxyInputReport m_last_valid_report;
    ProxyInputReport m_second_last_report;
    
    uint64_t m_buffer_delay_us; // Dynamic target buffering delay based on network jitter
    uint32_t m_expected_sequence;     // Next expected sequence number
    uint32_t m_dropped_packets;      // Count of detected dropped packets
    uint32_t m_out_of_order_packets; // Count of out-of-order arrivals
    
    // Interpolation Helpers
    ProxyInputReport InterpolateLinear(const ProxyInputReport& p1, const ProxyInputReport& p2, double t);
    void InterpolateHermiteIR(const ProxyInputReport& p0, const ProxyInputReport& p1, 
                              const ProxyInputReport& p2, const ProxyInputReport& p3, 
                              double t, float out_ir[2]);
};

/**
 * Dynamic Host Authority Manager
 * Monitors game states (from Gecko RAM hooks) as read-only telemetry and manages player roles.
 */
enum class NetplayAuthority {
    LOCAL_ONLY,       // Single-player / standard local connection
    PITCHER_CONTROL,  // Pitching team (defense, buffered latency input)
    BATTER_CONTROL    // Batting/Fielding team (offense, 0ms latency input)
};

class HostAuthorityManager {
public:
    HostAuthorityManager();
    
    // Updates local authority state based on RAM state register (Gecko Hooks)
    // 0x01 = Pitcher/Defense, 0x02 = Batter/Offense
    void UpdateStateFromMemoryRegister(uint8_t memory_reg_val);
    
    // Returns true if this client represents the offense (0ms local delay)
    bool HasLocalAuthority() const { return m_is_offense_team; }
    
    // Returns current authority type
    NetplayAuthority GetAuthorityMode() const { 
        return m_is_offense_team ? NetplayAuthority::BATTER_CONTROL : NetplayAuthority::PITCHER_CONTROL; 
    }
    
    // Set whether this client represents the offensive team (batting/running)
    void SetTeamRole(bool is_offense) { m_is_offense_team = is_offense; }
    bool IsOffenseTeam() const { return m_is_offense_team; }

private:
    NetplayAuthority m_mode;
    bool m_is_offense_team; // True if this client is batting/running (Offense)
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

    // Pulls the 0ms Visual Prediction Cursor State with Ghost Click protection
    EmulatedWiimoteState Get0msLocalCursorState();

    // Updates memory register states from Gecko core hooks
    void SyncGeckoState(uint8_t state_reg);

    // Update the client's team role
    void SetClientRole(bool is_offense);

    // Returns true if the receiver listener is active
    bool IsActive() const { return m_running; }

    // Slot configuration for Netplay to prevent controller hijacking
    void SetLocalPlayerSlot(int slot) { m_local_player_slot = slot; }
    int GetLocalPlayerSlot() const { return m_local_player_slot; }

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

    // Ghost Click Protection Parameters
    bool m_ghost_click_active;
    uint64_t m_ghost_click_start_us;
    float m_frozen_ir_pointer[2];
    uint16_t m_last_buttons_state;
    int m_local_player_slot; // 0 = Player 1, 1 = Player 2, etc.

    // Sender validation to prevent remote spoofing/hijacking
    bool m_sender_locked;
    uint32_t m_locked_ip;
    uint16_t m_locked_port;
};
