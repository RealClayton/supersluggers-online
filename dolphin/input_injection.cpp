#include "input_injection.h"
#include <iostream>
#include <algorithm>
#include <iterator>
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
    m_expected_sequence = 0;
    m_dropped_packets = 0;
    m_out_of_order_packets = 0;
    m_authority_bypass = false;
    std::memset(&m_last_valid_report, 0, sizeof(ProxyInputReport));
    std::memset(&m_second_last_report, 0, sizeof(ProxyInputReport));
}

AdaptiveJitterBuffer::~AdaptiveJitterBuffer() {}

void AdaptiveJitterBuffer::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
    std::memset(&m_last_valid_report, 0, sizeof(ProxyInputReport));
    std::memset(&m_second_last_report, 0, sizeof(ProxyInputReport));
    m_expected_sequence = 0;
    m_dropped_packets = 0;
    m_out_of_order_packets = 0;
    m_buffer_delay_us = 30000;
}

ProxyInputReport AdaptiveJitterBuffer::GetLatestRawReport() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty()) {
        ProxyInputReport empty_report;
        std::memset(&empty_report, 0, sizeof(ProxyInputReport));
        empty_report.ir_pointer[0] = 512;
        empty_report.ir_pointer[1] = 512;
        return empty_report;
    }
    return m_queue.back();
}

void AdaptiveJitterBuffer::PushReport(const ProxyInputReport& report) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Track packet ordering and detect drops via sequence numbers
    if (report.sequence > 0) { // Ignore uninitialized sequence fields
        if (m_expected_sequence == 0) {
            // First packet: calibrate the tracker
            m_expected_sequence = report.sequence + 1;
        } else if (report.sequence < m_expected_sequence) {
            m_out_of_order_packets++;
        } else if (report.sequence > m_expected_sequence) {
            m_dropped_packets += (report.sequence - m_expected_sequence);
            m_expected_sequence = report.sequence + 1;
        } else {
            // Exact match: report.sequence == m_expected_sequence
            m_expected_sequence = report.sequence + 1;
        }
    }

    // Sorted insertion: UDP does not guarantee ordering.
    // Use binary search to find the correct chronological position.
    auto it = std::lower_bound(m_queue.begin(), m_queue.end(), report,
        [](const ProxyInputReport& a, const ProxyInputReport& b) {
            return a.timestamp_us < b.timestamp_us;
        });
    m_queue.insert(it, report);

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
    result.ir_pointer[0] = static_cast<uint16_t>(
        static_cast<double>(p1.ir_pointer[0]) + t * (static_cast<double>(p2.ir_pointer[0]) - static_cast<double>(p1.ir_pointer[0]))
    );
    result.ir_pointer[1] = static_cast<uint16_t>(
        static_cast<double>(p1.ir_pointer[1]) + t * (static_cast<double>(p2.ir_pointer[1]) - static_cast<double>(p1.ir_pointer[1]))
    );
    
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

    // Prevent unsigned underflow / binary search crash if size <= 1
    if (m_queue.size() <= 1) {
        ProxyInputReport oldest = m_queue.front();
        state.buttons = oldest.buttons;
        for (int i = 0; i < 3; ++i) {
            state.accel[i] = (oldest.accel[i] - 512.0f) / 102.0f;
            state.gyro[i]  = oldest.gyro[i] / 8192.0f;
        }
        state.ir_pointer[0] = oldest.ir_pointer[0] / 1023.0f;
        state.ir_pointer[1] = oldest.ir_pointer[1] / 1023.0f;
        return state;
    }

    // Playback target time with adaptive jitter buffer delay offset
    uint64_t target_time = current_time_us;
    if (!m_authority_bypass) {
        target_time -= m_buffer_delay_us;
    }

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
        ProxyInputReport newest = m_queue.back();
        state.buttons = newest.buttons;
        
        uint64_t age_us = 0;
        if (target_time > newest.timestamp_us) {
            age_us = target_time - newest.timestamp_us;
        }
        
        // Only apply decay if the packet is actually stale (older than 15ms)
        float decay_accel = 1.0f;
        float decay_gyro = 1.0f;
        if (age_us > 15000) {
            decay_accel = 0.95f;
            decay_gyro = 0.90f;
            
            // Dynamically widen buffer size due to late packet detection (adaptation)
            if (!m_authority_bypass) {
                m_buffer_delay_us = std::min(m_buffer_delay_us + 1000, uint64_t(100000)); // Max 100ms
            }
        }
        
        for (int i = 0; i < 3; ++i) {
            state.accel[i] = ((newest.accel[i] - 512.0f) / 102.0f) * decay_accel; 
            state.gyro[i]  = (newest.gyro[i] / 8192.0f) * decay_gyro;
        }
        state.ir_pointer[0] = newest.ir_pointer[0] / 1023.0f;
        state.ir_pointer[1] = newest.ir_pointer[1] / 1023.0f;
        return state;
    }

    // Binary search for the framing reports surrounding the target playback time (O(log n))
    ProxyInputReport search_key;
    search_key.timestamp_us = target_time;
    auto upper = std::upper_bound(m_queue.begin(), m_queue.end(), search_key,
        [](const ProxyInputReport& a, const ProxyInputReport& b) {
            return a.timestamp_us < b.timestamp_us;
        });
    size_t idx = 0;
    if (upper != m_queue.begin()) {
        idx = std::distance(m_queue.begin(), upper) - 1;
    }
    // Clamp to prevent out-of-bounds on idx+1 access
    if (idx >= m_queue.size() - 1) {
        idx = m_queue.size() - 2;
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
    if (!m_authority_bypass && m_buffer_delay_us > 20000) { // Keep floor of 20ms
        m_buffer_delay_us = std::max(uint64_t(20000), m_buffer_delay_us - 10); // 10us recovery per frame
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
    // 0x01 = Pitcher/Defense, 0x02 = Batter/Offense
    if (memory_reg_val == 0x02) {
        m_is_offense_team = true;
        m_mode = NetplayAuthority::BATTER_CONTROL;
    } else if (memory_reg_val == 0x01) {
        m_is_offense_team = false;
        m_mode = NetplayAuthority::PITCHER_CONTROL;
    }
}

// ============================================================================
// NetplayInputReceiver Implementation
// ============================================================================

NetplayInputReceiver& NetplayInputReceiver::GetInstance() {
    static NetplayInputReceiver instance;
    return instance;
}

NetplayInputReceiver::NetplayInputReceiver()
    : m_running(false),
      m_port(5555),
      m_socket(INVALID_SOCKET),
      m_ghost_click_active(false),
      m_ghost_click_start_us(0),
      m_last_buttons_state(0),
      m_local_player_slot(0),
      m_sender_locked(false),
      m_locked_ip(0),
      m_locked_port(0) { // Default to Player 1 (slot 0)
    m_frozen_ir_pointer[0] = 0.5f;
    m_frozen_ir_pointer[1] = 0.5f;
}

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

    m_sender_locked = false;
    m_locked_ip = 0;
    m_locked_port = 0;

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    // Apply socket receive timeout (100ms) to prevent thread deadlocks on stop/exit
#if defined(_WIN32)
    DWORD timeout = 100;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    
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
#if defined(_WIN32)
    int sender_len = sizeof(sender_addr);
#else
    socklen_t sender_len = sizeof(sender_addr);
#endif

    while (m_running) {
        int bytes_rec = recvfrom(m_socket, reinterpret_cast<char*>(&report), sizeof(report), 0,
                                 reinterpret_cast<sockaddr*>(&sender_addr), &sender_len);
        
        if (bytes_rec == SOCKET_ERROR) {
            // Socket was closed, read timeout (100ms SO_RCVTIMEO), or read error
            continue;
        }

        if (bytes_rec == sizeof(ProxyInputReport)) {
            // Sender lock validation to prevent arbitrary network input injection
            uint32_t sender_ip = sender_addr.sin_addr.s_addr;
            uint16_t sender_port = ntohs(sender_addr.sin_port);
            
            if (!m_sender_locked) {
                m_locked_ip = sender_ip;
                m_locked_port = sender_port;
                m_sender_locked = true;
                std::cout << "[Dolphin Fork] Locked input receiver to sender " 
                          << inet_ntoa(sender_addr.sin_addr) << ":" << sender_port << std::endl;
            }
            
            if (sender_ip == m_locked_ip && sender_port == m_locked_port) {
                m_jitter_buffer.PushReport(report);
            }
        }
    }
}

EmulatedWiimoteState NetplayInputReceiver::GetEmulatedState() {
    uint64_t current_time = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        
    // Dynamically bypass jitter buffering delay if client represents the offense (Batter)
    m_jitter_buffer.SetAuthorityBypass(m_authority_manager.HasLocalAuthority());
        
    // Retrieve the jitter-buffered and interpolated controller input
    return m_jitter_buffer.PullState(current_time);
}

EmulatedWiimoteState NetplayInputReceiver::Get0msLocalCursorState() {
    uint64_t current_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        
    ProxyInputReport latest = m_jitter_buffer.GetLatestRawReport();
    EmulatedWiimoteState state;
    std::memset(&state, 0, sizeof(EmulatedWiimoteState));
    
    state.buttons = latest.buttons;
    for (int i = 0; i < 3; ++i) {
        state.accel[i] = (latest.accel[i] - 512.0f) / 102.0f;
        state.gyro[i] = latest.gyro[i] / 8192.0f;
    }

    float raw_ir_x = latest.ir_pointer[0] / 1023.0f;
    float raw_ir_y = latest.ir_pointer[1] / 1023.0f;

    // Detect Click Transition (Button A: 0x0008, Button B: 0x0004, or byte-swapped representations)
    uint16_t click_mask = 0x000C | 0x0C00; 
    uint16_t current_clicks = latest.buttons & click_mask;
    uint16_t last_clicks = m_last_buttons_state & click_mask;
    
    // If a click has occurred (released -> pressed transition)
    if (current_clicks && (current_clicks != last_clicks)) {
        if (!m_ghost_click_active) {
            m_ghost_click_active = true;
            m_ghost_click_start_us = current_time_us;
            m_frozen_ir_pointer[0] = raw_ir_x;
            m_frozen_ir_pointer[1] = raw_ir_y;
        }
    }
    
    m_last_buttons_state = latest.buttons;

    // Verify if the ghost click freeze duration (matching dynamic buffer delay) has elapsed
    if (m_ghost_click_active) {
        uint64_t freeze_duration = m_jitter_buffer.GetBufferDelayUs();
        if (current_time_us - m_ghost_click_start_us >= freeze_duration) {
            m_ghost_click_active = false;
        }
    }

    if (m_ghost_click_active) {
        state.ir_pointer[0] = m_frozen_ir_pointer[0];
        state.ir_pointer[1] = m_frozen_ir_pointer[1];
    } else {
        state.ir_pointer[0] = raw_ir_x;
        state.ir_pointer[1] = raw_ir_y;
    }

    return state;
}

void NetplayInputReceiver::SyncGeckoState(uint8_t state_reg) {
    m_authority_manager.UpdateStateFromMemoryRegister(state_reg);
}

void NetplayInputReceiver::SetClientRole(bool is_offense) {
    m_authority_manager.SetTeamRole(is_offense);
}
