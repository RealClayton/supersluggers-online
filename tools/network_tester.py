#!/usr/bin/env python3
import socket
import time
import sys
import argparse
import statistics

# Default port for P2P connection diagnostics
DEFAULT_PORT = 5558

def run_server(port):
    print(f"[Network Daemon] Launching ping-pong diagnostic server on UDP port {port}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(("0.0.0.0", port))
    except Exception as e:
        print(f"[Error] Failed to bind server socket: {e}")
        sys.exit(1)

    print("[Network Daemon] Awaiting connection from client...")
    try:
        while True:
            # Echo back any incoming UDP packet instantly
            data, addr = sock.recvfrom(1024)
            sock.sendto(data, addr)
    except KeyboardInterrupt:
        print("\n[Network Daemon] Shutting down.")
    finally:
        sock.close()

def run_client(host, port):
    print(f"[Network Diagnostics] Connecting to remote host {host}:{port}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0) # 1 second timeout

    pings = []
    dropped = 0
    count = 500 # Send 500 packets to get a stable sample size
    
    print(f"[*] Blasting {count} UDP pings to measure latency & jitter...")
    
    for seq in range(count):
        start = time.perf_counter()
        payload = f"PING:{seq}".encode('utf-8')
        try:
            sock.sendto(payload, (host, port))
            data, addr = sock.recvfrom(1024)
            end = time.perf_counter()
            
            # Record Round Trip Time in milliseconds
            rtt_ms = (end - start) * 1000.0
            pings.append(rtt_ms)
        except socket.timeout:
            dropped += 1
        
        # Sleep briefly (10ms) between checks to avoid overloading buffers
        time.sleep(0.01)

    sock.close()

    # Calculations
    total_sent = count
    total_received = len(pings)
    loss_rate = (dropped / total_sent) * 100.0

    print("\n==================================================")
    print("        NETPLAY P2P DIAGNOSTIC REPORT             ")
    print("==================================================")
    print(f"Packets Sent:     {total_sent}")
    print(f"Packets Received: {total_received}")
    print(f"Packet Loss Rate: {loss_rate:.2f}%")

    if total_received == 0:
        print("\n🔴 CONNECTION FAILED: Remote server is unreachable or timed out.")
        print("  - Verify your peer has port forwarding configured on UDP " + str(port))
        print("  - Check firewall settings to ensure incoming UDP traffic is allowed.")
        print("==================================================")
        return

    avg_latency = statistics.mean(pings)
    min_latency = min(pings)
    max_latency = max(pings)
    jitter = statistics.stdev(pings) if len(pings) > 1 else 0.0

    print(f"Minimum RTT:      {min_latency:.2f} ms")
    print(f"Maximum RTT:      {max_latency:.2f} ms")
    print(f"Average Latency:  {avg_latency:.2f} ms")
    print(f"Jitter (StdDev):  {jitter:.2f} ms")
    print("--------------------------------------------------")

    # 1. Connection Grading
    if loss_rate > 3.0:
        grade = "D (Unstable - Expect Desyncs)"
    elif loss_rate > 1.0 or jitter > 15.0:
        grade = "C (Variable - High Jitter)"
    elif avg_latency > 80.0:
        grade = "B (High Latency - Asymmetric Swaps Required)"
    else:
        grade = "A (Excellent - Optimal Netplay Link)"
    
    # 2. Frame Buffer Recommendations
    # Rule of thumb: 1 frame buffer = 16.6ms of latency coverage (round trip)
    # We add 1.5 frames of margin over the average latency to absorb jitter spikes safely
    recommended_buffer = int(((avg_latency + (jitter * 2.0)) / 16.67) + 1)
    # Floor of 2 frames to absorb emulator processing buffers
    recommended_buffer = max(2, recommended_buffer)

    print(f"Connection Quality Grade: {grade}")
    print(f"Recommended Dolphin Frame Buffer: {recommended_buffer} frames")
    print("==================================================")
    print("Copy this recommended frame buffer directly to the Netplay settings window.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="P2P Connection Diagnostics for Mario Super Sluggers Netplay")
    parser.add_argument("--server", action="store_true", help="Run diagnostic host daemon")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"UDP port (default {DEFAULT_PORT})")
    parser.add_argument("--connect", type=str, help="Peer external IP to test connection link")
    args = parser.parse_args()

    if not args.server and not args.connect:
        print("Please specify either --server or --connect <peer_ip>.")
        sys.exit(1)

    if args.server:
        run_server(args.port)
    elif args.connect:
        run_client(args.connect, args.port)
