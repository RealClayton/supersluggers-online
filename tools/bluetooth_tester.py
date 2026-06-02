#!/usr/bin/env python3
import socket
import struct
import time
import sys
import argparse
import statistics

# WiiRemoteReport UDP format (30 bytes):
# <Q I H 3h 3h 2H (timestamp, seq, buttons, accel[3], gyro[3], ir[2])
STRUCT_FORMAT = "<Q I H 3h 3h 2H"
EXPECTED_SIZE = struct.calcsize(STRUCT_FORMAT)

def run_diagnostics(port, packets_to_collect):
    print(f"[*] Binding to local input socket on 127.0.0.1:{port}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    try:
        sock.bind(("127.0.0.1", port))
    except Exception as e:
        print(f"[Error] Failed to bind local socket on port {port}: {e}")
        print("  Ensure that Dolphin is not currently running and locking this port.")
        sys.exit(1)

    print(f"[Active] Awaiting SluggersProxy stream connection...")
    print(f"[*] Collecting {packets_to_collect} samples for Bluetooth performance grading...")
    
    timestamps = []
    sequences = []
    
    # Wait for the first packet to establish active stream
    sock.settimeout(10.0) # Wait up to 10 seconds for initial connection
    try:
        data, addr = sock.recvfrom(512)
        if len(data) == EXPECTED_SIZE:
            unpacked = struct.unpack(STRUCT_FORMAT, data)
            timestamps.append(unpacked[0])
            sequences.append(unpacked[1])
            print(f"[Connected] Receiving input stream from {addr[0]}:{addr[1]}")
    except socket.timeout:
        print("\n🔴 TIMEOUT: No input reports received on port " + str(port))
        print("  - Make sure SluggersProxy is active and running in your terminal.")
        print("  - Verify that the Wii Remote is paired and successfully emitting inputs.")
        sock.close()
        sys.exit(1)

    # Collect remaining reports
    sock.settimeout(2.0)
    try:
        for i in range(1, packets_to_collect):
            data, addr = sock.recvfrom(512)
            if len(data) == EXPECTED_SIZE:
                unpacked = struct.unpack(STRUCT_FORMAT, data)
                timestamps.append(unpacked[0])
                sequences.append(unpacked[1])
    except socket.timeout:
        print(f"\n[Warning] Input stream stopped prematurely after {len(timestamps)} packets.")
    finally:
        sock.close()

    if len(timestamps) < 100:
        print("🔴 ERROR: Insufficient samples collected. Stream is unstable or disconnected.")
        sys.exit(1)

    # Performance calculations
    intervals_us = []
    for i in range(1, len(timestamps)):
        delta = timestamps[i] - timestamps[i-1]
        intervals_us.append(delta)

    # Sort sequence verification
    dropped_packets = 0
    out_of_order = 0
    for i in range(1, len(sequences)):
        expected = sequences[i-1] + 1
        if sequences[i] > expected:
            dropped_packets += (sequences[i] - expected)
        elif sequences[i] < expected:
            out_of_order += 1

    # Convert intervals to milliseconds
    intervals_ms = [x / 1000.0 for x in intervals_us]
    avg_interval = statistics.mean(intervals_ms)
    jitter_std = statistics.stdev(intervals_ms) if len(intervals_ms) > 1 else 0.0
    max_interval_spike = max(intervals_ms)

    print("\n==================================================")
    print("      BLUETOOTH ADAPTER PERFORMANCE REPORT        ")
    print("==================================================")
    print(f"Total Samples Collected: {len(timestamps)}")
    print(f"Average Report Interval: {avg_interval:.3f} ms (Target: 1.000 ms)")
    print(f"Jitter Standard Dev:     {jitter_std:.3f} ms (15us std-dev = Mayflash Bar)")
    print(f"Max Spike Gap:           {max_interval_spike:.2f} ms")
    print(f"Dropped Packets Count:   {dropped_packets}")
    print(f"Out-of-order Packets:    {out_of_order}")
    print("--------------------------------------------------")

    # 1. Adapter Grading
    # Grade criteria matches typical stack latencies
    if jitter_std < 0.150: # < 150 microseconds
        grade = "A+ (Mayflash Mode 4 / Pro Grade)"
        color = "GREEN"
        desc = "Perfect sub-millisecond precision. Completely desync-proof."
    elif jitter_std < 0.350: # < 350 microseconds
        grade = "A (High Quality USB Bluetooth Dongle)"
        color = "GREEN"
        desc = "Excellent tracking. Easily smoothed by Hermite splines."
    elif jitter_std < 0.800:
        grade = "B (Standard Bluetooth Stack / Built-in OS)"
        color = "YELLOW"
        desc = "Playable. Expect minor cursor micro-stutter without buffers."
    elif jitter_std < 1.500:
        grade = "C (High Latency OS Stack / Power Saving Mode)"
        color = "YELLOW"
        desc = "Sub-optimal. Disable Windows Bluetooth power-saving features."
    else:
        grade = "F (Failed - Severe Packet Staggering)"
        color = "RED"
        desc = "Do not play. High jitter will trigger jitter-buffer starvation desyncs."

    print(f"Hardware Rating:  {grade}")
    print(f"Status Details:   {desc}")
    print("==================================================")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="SluggersProxy Local Bluetooth Adapter Calibration Tool")
    parser.add_argument("--port", type=int, default=5555, help="UDP listening port (default 5555)")
    parser.add_argument("--samples", type=int, default=1000, help="Number of packets to analyze (default 1000)")
    args = parser.parse_args()

    print("==================================================")
    print("    Local Bluetooth Interface Diagnostic Utility  ")
    print("==================================================")
    run_diagnostics(args.port, args.samples)
