#include "input_injection.h"
#include <iostream>
#include <algorithm>
#include <cmath>

#if !defined(_WIN32)
    #include <unistd.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #define closesocket close
#endif

// ============================================================================
// AdaptiveJitterBuffer Implementation
// ============================================================================

AdaptiveJitterBuffer::AdaptiveJitterBuffer() {
    m_buffer_delay_us = 30000; // Default 30ms buffering delay (approx. 2 frames)
    std::memset(&m_last_valid_report, 0, sizeof(ProxyInputReport));
    std::memset(&m_second_last_report, 0, sizeof(ProxyInputReport));
}

AdaptiveJitterBuffer::~AdaptiveJitterBuffer() {}

void AdaptiveJitterBuffer::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
    std::memset(&m_last_valid_report, 0, sizeof(ProxyInputReport));
    std::memset(&m_second_last_report, 0, sizeof(ProxyInputReport));
}

void AdaptiveJitterBuffer::PushReport(const ProxyInputReport& report) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back(report);

    // Keep queue trimmed to the last 500ms of reports to prevent memory leakage
    if (m_queue.size() > 500) {
        m_queue.erase(m_queue.begin(), m_queue.begin() + 100);
    }
}

ProxyInputReport AdaptiveJitterBuffer::InterpolateLinear(const ProxyInputReport& p1, const ProxyInputReport& p2, double t) {
    ProxyInputReport result;
    result.timestamp_us = p1.timestamp_us + static_cast<uint64_t>(t * (p2.timestamp_us - p1.timestamp_us));
    result.buttons = (t < 0.5) ? p1.buttons : p2.buttons; // Latched digital buttons

    // LERP raw accelerometers
    for (int i = 0; i < 3; ++i) {
        result.accel[i] = p1.accel[i] + static_cast<int16_t>(t * (p2.accel[i] - p1.accel[i]));
        result.gyro[i]  = p1.gyro[i]  + static_cast<int16_t>(t * (p2.gyro[i]  - p1.gyro[i]));
    }
    
    // LERP pointer coordinate fallbacks (Hermite used for high-fidelity)
    result.ir_pointer[0] = p1.ir_pointer[0] + static_cast<uint16_t>(t * (p2.ir_pointer[0] - p1.ir_pointer[0]));
    result.ir_pointer[1] = p1.ir_pointer[1] + static_cast<uint16_t>(t * (p2.ir_pointer[1] - p1.ir_pointer[1]));
    
    return result;
}

/**
 * Calculates cubic Hermite spline interpolation for smooth IR pointer curves.
 * Formula: h00(t)*P1 + h10(t)*m1 + h01(t)*P2 + h11(t)*m2
 * Where m1, m2 are tangents derived from surrounding frames.
 */
void AdaptiveJitterBuffer::InterpolateHermiteIR(const ProxyInputReport& p0, const ProxyInputReport& p1, 
                                                const ProxyInputReport& p2, const ProxyInputReport& p3, 
                                                double t, float out_ir[2]) {
    double t2 = t * t;
    double t3 = t2 * t;

    // Hermite basis functions
    double h00 = 2*t3 - 3*t2 + 1;
    double h10 = t3 - 2*t2 + t;
    double h01 = -2*t3 + 3*t2;
    double h11 = t3 - t2;

    for (int i = 0; i < 2; ++i) {
        double val0 = p0.ir_pointer[i];
        double val1 = p1.ir_pointer[i];
        double val2 = p2.ir_pointer[i];
        double val3 = p3.ir_pointer[i];

        // Finite difference tangents
        double tangent1 = 0.5 * (val2 - val0);
        double tangent2 = 0.5 * (val3 - val1);

        // Spline result
        double result = h00 * val1 + h10 * tangent1 + h01 * val2 + h11 * tangent2;
        
        // Normalize coordinates to emulated screen space (0.0 to 1.0)
        out_ir[i] = static_cast<float>(std::clamp(result / 1023.0, 0.0, 1.0));
    }
}

EmulatedWiimoteState AdaptiveJitterBuffer::PullState(uint64_t current_time_us) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    EmulatedWiimoteState state;
    std::memset(&state, 0, sizeof(EmulatedWiimoteState));
    
    if (m_queue.empty()) {
        // Safe defaults if no packets are present
        state.buttons = 0x0000;
        state.accel[1] = 1.0f; // 1G gravity Y
        state.ir_pointer[0] = 0.5f; state.ir_pointer[1] = 0.5f; // Center
        return state;
    }

    // Playback target time with adaptive jitter buffer delay offset
    uint64_t target_time = current_time_us - m_buffer_delay_us;

    // Boundary: Target is older than the oldest packet in queue (extreme latency shift)
    if (target_time <= m_queue.front().timestamp_us) {
        ProxyInputReport oldest = m_queue.front();
        state.buttons = oldest.buttons;
        for (int i = 0; i < 3; ++i) {
            state.accel[i] = (oldest.accel[i] - 512.0f) / 102.0f; // Scale to -1G / +1G bounds
            state.gyro[i]  = oldest.gyro[i] / 8192.0f;
        }
        state.ir_pointer[0] = oldest.ir_pointer[0] / 1023.0f;
        state.ir_pointer[1] = oldest.ir_pointer[1] / 1023.0f;
        return state;
    }

    // Boundary: Target is newer than the newest packet (network drop/late frame)
    if (target_time >= m_queue.back().timestamp_us) {
        // Extrapolate with slight decay to prevent analog drift loops
        ProxyInputReport newest = m_queue.back();
        state.buttons = newest.buttons;
        for (int i = 0; i < 3; ++i) {
            state.accel[i] = ((newest.accel[i] - 512.0f) / 102.0f) * 0.95f; 
            state.gyro[i]  = (newest.gyro[i] / 8192.0f) * 0.90f; // Rapid rotational decay
        }
        state.ir_pointer[0] = newest.ir_pointer[0] / 1023.0f;
        state.ir_pointer[1] = newest.ir_pointer[1] / 1023.0f;
        
        // Dynamically widen buffer size due to late packet detection (adaptation)
        m_buffer_delay_us = std::min(m_buffer_delay_us + 1000, uint64_t(100000)); // Max 100ms
        return state;
    }

    // Find the framing reports surrounding the target playback time
    size_t idx = 0;
    for (size_t i = 0; i < m_queue.size() - 1; ++i) {
        if (target_time >= m_queue[i].timestamp_us && target_time <= m_queue[i+1].timestamp_us) {
            idx = i;
            break;
        }
    }

    ProxyInputReport p1 = m_queue[idx];
    ProxyInputReport p2 = m_queue[idx+1];

    // Compute interpolation fraction (t)
    double t = 0.0;
    if (p2.timestamp_us > p1.timestamp_us) {
        t = static_cast<double>(target_time - p1.timestamp_us) / (p2.timestamp_us - p1.timestamp_us);
    }

    // 1. Digital buttons
    state.buttons = (t < 0.5) ? p1.buttons : p2.buttons;

    // 2. Linear Interpolate accelerometers & gyroscopes
    for (int i = 0; i < 3; ++i) {
        float val1 = (p1.accel[i] - 512.0f) / 102.0f;
        float val2 = (p2.accel[i] - 512.0f) / 102.0f;
        state.accel[i] = val1 + static_cast<float>(t) * (val2 - val1);

        float gyro1 = p1.gyro[i] / 8192.0f;
        float gyro2 = p2.gyro[i] / 8192.0f;
        state.gyro[i] = gyro1 + static_cast<float>(t) * (gyro2 - gyro1);
    }

    // 3. Hermite Spline Interpolate IR pointer coordinate using 4 surrounding points
    size_t p0_idx = (idx > 0) ? idx - 1 : idx;
    size_t p3_idx = (idx < m_queue.size() - 2) ? idx + 2 : idx + 1;

    ProxyInputReport p0 = m_queue[p0_idx];
    ProxyInputReport p3 = m_queue[p3_idx];

    InterpolateHermiteIR(p0, p1, p2, p3, t, state.ir_pointer);

    // Slowly shrink delay if network is stable (jitter decay)
    if (m_buffer_delay_us > 20000) { // Keep floor of 20ms
        m_buffer_delay_us -= 1; // Sub-microsecond gradual recovery
    }

    return state;
}

// ============================================================================
// HostAuthorityManager Implementation
// ============================================================================

HostAuthorityManager::HostAuthorityManager() {
    m_mode = NetplayAuthority::LOCAL_ONLY;
    m_is_offense_team = false;
}

void HostAuthorityManager::UpdateStateFromMemoryRegister(uint8_t memory_reg_val) {
    // 0x01 -> Pitcher holds authority. Offense client yields control.
    // 0x02 -> Batter/Fielder holds authority. Defense client yields control.
    if (memory_reg_val == 0x01) {
        m_mode = NetplayAuthority::PITCHER_CONTROL;
    } else if (memory_reg_val == 0x02) {
        m_mode = NetplayAuthority::BATTER_CONTROL;
    } else {
        m_mode = NetplayAuthority::LOCAL_ONLY;
    }
}

bool HostAuthorityManager::HasLocalAuthority() const {
    if (m_mode == NetplayAuthority::LOCAL_ONLY) {
        return true;
    }
    if (m_mode == NetplayAuthority::PITCHER_CONTROL) {
        // If we are defending (pitching), we have 0ms instant authority
        return !m_is_offense_team;
    }
    if (m_mode == NetplayAuthority::BATTER_CONTROL) {
        // If offense (batting/fielding), we have 0ms instant authority
        return m_is_offense_team;
    }
    return true;
}

// ============================================================================
// NetplayInputReceiver Implementation
// ============================================================================

NetplayInputReceiver& NetplayInputReceiver::GetInstance() {
    static NetplayInputReceiver instance;
    return instance;
}

NetplayInputReceiver::NetplayInputReceiver() : m_running(false), m_port(5555), m_socket(INVALID_SOCKET) {}

NetplayInputReceiver::~NetplayInputReceiver() {
    Stop();
}

void NetplayInputReceiver::Start(uint16_t port) {
    if (m_running) return;

    m_port = port;
    m_running = true;

#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    
    sockaddr_in local_addr;
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(m_port);
    local_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_socket, reinterpret_cast<sockaddr*>(&local_addr), sizeof(local_addr)) == SOCKET_ERROR) {
        std::cerr << "[Dolphin Fork] Failed to bind Netplay Input Socket on port " << m_port << std::endl;
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        m_running = false;
        return;
    }

    std::cout << "[Dolphin Fork] Netplay input listener bound successfully on port " << m_port << std::endl;
    m_listener_thread = std::thread(&NetplayInputReceiver::ListeningLoop, this);
}

void NetplayInputReceiver::Stop() {
    if (!m_running) return;

    m_running = false;
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

    if (m_listener_thread.joinable()) {
        m_listener_thread.join();
    }

#if defined(_WIN32)
    WSACleanup();
#endif
}

void NetplayInputReceiver::ListeningLoop() {
    ProxyInputReport report;
    sockaddr_in sender_addr;
    int sender_len = sizeof(sender_addr);

    while (m_running) {
        int bytes_rec = recvfrom(m_socket, reinterpret_cast<char*>(&report), sizeof(report), 0,
                                 reinterpret_cast<sockaddr*>(&sender_addr), &sender_len);
        
        if (bytes_rec == SOCKET_ERROR) {
            // Socket was closed or read error
            continue;
        }

        if (bytes_rec == sizeof(ProxyInputReport)) {
            m_jitter_buffer.PushReport(report);
        }
    }
}

EmulatedWiimoteState NetplayInputReceiver::GetEmulatedState() {
    uint64_t current_time = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        
    // Retrieve the jitter-buffered and interpolated controller input
    return m_jitter_buffer.PullState(current_time);
}

void NetplayInputReceiver::SyncGeckoState(uint8_t state_reg) {
    m_authority_manager.UpdateStateFromMemoryRegister(state_reg);
}
