#!/usr/bin/env python3
# ============================================================================
#         MARIO SUPER SLUGGERS NETPLAY: UDP RELAY SERVER
# ============================================================================
# Runs on your Linux Server (MacBook Pro / Linux Mint) to completely bypass
# home router port forwarding, firewalls, and NAT issues for both players.
#
# Usage:
#   python3 relay_server.py
#
# Setup:
#   Both players enter this Linux Server's IP address as the "friend's IP"
#   in the launcher or launch scripts!

import socket
import threading
import sys

def relay_port(port):
    print(f"[Relay] Starting UDP relay on port {port}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(("0.0.0.0", port))
    except Exception as e:
        print(f"[Port {port}] Failed to bind socket: {e}")
        return
        
    player1 = None
    player2 = None
    
    while True:
        try:
            data, addr = sock.recvfrom(4096)
            
            # 1. Dynamically register players as they connect
            if player1 is None:
                player1 = addr
                print(f"[Port {port}] Registered Player 1: {addr}")
            elif addr == player1:
                pass
            elif player2 is None:
                player2 = addr
                print(f"[Port {port}] Registered Player 2: {addr}")
            elif addr == player2:
                pass
            else:
                # If a third player pings, update Player 2 connection mapping
                print(f"[Port {port}] Re-mapping Player 2 connection to new client: {addr}")
                player2 = addr
                
            # 2. Relay packets back and forth
            if addr == player1 and player2 is not None:
                sock.sendto(data, player2)
            elif addr == player2 and player1 is not None:
                sock.sendto(data, player1)
                
        except Exception as e:
            print(f"[Port {port}] Server error encountered: {e}")
            break

def main():
    print("===================================================================")
    print("         MARIO SUPER SLUGGERS: ULTRA-LOW LATENCY UDP RELAY         ")
    print("===================================================================")
    print(" [Architecture]: Relays UDP synchronization frames over a high-band")
    print("                 Linux server to bypass residential NAT routers.")
    print("===================================================================")
    print("")

    # Start relay threads for Handshake (5558) and Input Sync (5556)
    t1 = threading.Thread(target=relay_port, args=(5558,), daemon=True)
    t2 = threading.Thread(target=relay_port, args=(5556,), daemon=True)
    
    t1.start()
    t2.start()
    
    print("[Server Status] Online and listening on:")
    print("   -> UDP Port 5558 (Sync-Launch Handshake)")
    print("   -> UDP Port 5556 (Netplay Input Sync)")
    print("")
    print(" [Instructions]: Both Host and Client must configure their")
    print("                 remote IP to be this Linux Server's IP address.")
    print("===================================================================")
    print("Press Ctrl+C to safely terminate the relay...")
    
    try:
        # Keep main thread alive
        while True:
            t1.join(timeout=1.0)
            t2.join(timeout=1.0)
            if not t1.is_alive() or not t2.is_alive():
                print("[Server Error] One of the port listeners died. Exiting.")
                break
    except KeyboardInterrupt:
        print("\n[Server Shutdown] Safely closing all relay threads. Have a good game!")
        sys.exit(0)

if __name__ == "__main__":
    main()
