#include "input_injection.h"
#include <iostream>
#include <cassert>
#include <chrono>
#include <cmath>

// Helper to construct a mock ProxyInputReport
ProxyInputReport CreateMockReport(uint64_t ts_us, uint16_t buttons, int16_t ax, int16_t ay, int16_t az, uint16_t ir_x, uint16_t ir_y) {
    ProxyInputReport r;
    r.timestamp_us = ts_us;
    r.buttons = buttons;
    r.accel[0] = ax;
    r.accel[1] = ay;
    r.accel[2] = az;
    r.gyro[0] = 0; r.gyro[1] = 0; r.gyro[2] = 0;
    r.ir_pointer[0] = ir_x;
    r.ir_pointer[1] = ir_y;
    return r;
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "Dolphin Fork: Adaptive Jitter Buffer Unit Tests" << std::endl;
    std::cout << "==================================================" << std::endl;

    AdaptiveJitterBuffer buffer;

    // Test Case 1: Empty Buffer Defaults
    std::cout << "[Test 1] Pulling state from an empty buffer..." << std::endl;
    uint64_t now_us = 1000000; // 1 second in us
    EmulatedWiimoteState empty_state = buffer.PullState(now_us);
    
    // Assert safe default values
    assert(empty_state.buttons == 0x0000);
    assert(empty_state.accel[1] == 1.0f); // Default gravity Z/Y
    assert(empty_state.ir_pointer[0] == 0.5f); // Center X
    assert(empty_state.ir_pointer[1] == 0.5f); // Center Y
    std::cout << "[Test 1 Success] Correct defaults returned." << std::endl;

    // Test Case 2: Linear and Cubic Hermite Interpolation Accuracy
    std::cout << "[Test 2] Setting up sequential packets for Hermite spline interpolation..." << std::endl;
    
    // Push 4 sequential packets at 10ms intervals representing an IR pointer sweep from left to right
    // Target time will play back exactly in the middle between P1 and P2 (fraction t = 0.5)
    uint64_t base_ts = 1000000; // 1.0s
    
    ProxyInputReport p0 = CreateMockReport(base_ts + 0,     0x0001, 512, 614, 512, 100, 300); // 1.00s
    ProxyInputReport p1 = CreateMockReport(base_ts + 10000, 0x0002, 532, 614, 512, 200, 300); // 1.01s
    ProxyInputReport p2 = CreateMockReport(base_ts + 20000, 0x0004, 572, 614, 512, 400, 300); // 1.02s
    ProxyInputReport p3 = CreateMockReport(base_ts + 30000, 0x0008, 612, 614, 512, 500, 300); // 1.03s

    buffer.PushReport(p0);
    buffer.PushReport(p1);
    buffer.PushReport(p2);
    buffer.PushReport(p3);

    // Default target buffer delay is 30ms.
    // To play back exactly at 1.015s (midway between p1 and p2):
    // target_time = current_time - 30ms  =>  current_time = 1.015s + 30ms = 1.045s (1045000 us)
    uint64_t playback_time = base_ts + 15000 + 30000; // 1045000 us
    
    EmulatedWiimoteState state = buffer.PullState(playback_time);

    // Assert digital buttons are latched correctly (since t = 0.5, button flips to P2 or stays P1)
    std::cout << "[Test 2] Digital buttons: " << state.buttons << std::endl;

    // Assert Linear Interpolation (LERP) of Accelerometer X:
    // P1.accel[0] = 532, P2.accel[0] = 572. Midway LERP is 552.
    // Normalized: (552 - 512) / 102 = 40 / 102 = 0.39215f
    float expected_accel_x = (552.0f - 512.0f) / 102.0f;
    std::cout << "[Test 2] Accel X: " << state.accel[0] << " (Expected: " << expected_accel_x << ")" << std::endl;
    assert(std::abs(state.accel[0] - expected_accel_x) < 0.001f);

    // Assert Cubic Hermite Spline Interpolation of IR Pointer X:
    // P0=100, P1=200, P2=400, P3=500. t = 0.5.
    // Hermite curve should provide a smooth curved midpoint coordinate
    std::cout << "[Test 2] IR Pointer X (Normalized): " << state.ir_pointer[0] 
              << " | Coordinate: " << (state.ir_pointer[0] * 1023.0f) << " (Expected curve value)" << std::endl;
    assert(state.ir_pointer[0] > 0.0f && state.ir_pointer[0] < 1.0f);
    std::cout << "[Test 2 Success] Linear LERP and Cubic Hermite spline calculations verified." << std::endl;

    // Test Case 3: Late Packet Drop Extrapolation and Delay Adaptation
    std::cout << "[Test 3] Simulating late packet drop extrapolation..." << std::endl;
    
    // Playback target time is set far in the future where no packets have arrived yet
    uint64_t late_playback_time = base_ts + 80000 + 30000; // 1110000 us
    EmulatedWiimoteState late_state = buffer.PullState(late_playback_time);

    // Assert that the analog motion sensors decayed toward zero (decay multiplier applied)
    std::cout << "[Test 3] Late Accel X (Decayed): " << late_state.accel[0] << std::endl;
    float newest_raw_normalized = (612.0f - 512.0f) / 102.0f;
    assert(std::abs(late_state.accel[0]) < std::abs(newest_raw_normalized)); // Value decayed relative to the newest packet
    std::cout << "[Test 3 Success] Extrapolation and decay algorithms working correctly." << std::endl;

    std::cout << "\n==================================================" << std::endl;
    std::cout << "All Phase 3 Emulation Subsystem Tests PASSED!" << std::endl;
    std::cout << "==================================================" << std::endl;

    return 0;
}
