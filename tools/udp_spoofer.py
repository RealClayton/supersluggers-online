#!/usr/bin/env python3
import socket
import struct
import time
import math
import sys
import argparse

# ProxyInputReport binary layout matching input_injection.h:
# struct ProxyInputReport {
#     uint64_t timestamp_us;
#     uint32_t sequence;
#     uint16_t buttons;
#     int16_t accel[3];
#     int16_t gyro[3];
#     uint16_t ir_pointer[2];
# };
# Pack format: <Q I H 3h 3h 2H (little endian, 1-byte alignment, 30 bytes total)
STRUCT_FORMAT = "<Q I H 3h 3h 2H"

def generate_report(timestamp_us, seq, buttons=0, accel=(512, 512, 512), gyro=(0, 0, 0), ir=(512, 512)):
    return struct.pack(
        STRUCT_FORMAT,
        timestamp_us,
        seq,
        buttons,
        accel[0], accel[1], accel[2],
        gyro[0], gyro[1], gyro[2],
        ir[0], ir[1]
    )

def main():
    parser = argparse.ArgumentParser(description="SluggersProxy UDP Payload Spoofer for input injection socket testing")
    parser.add_argument("--port", type=int, default=5555, help="Target UDP port (default 5555)")
    parser.add_argument("--host", type=str, default="127.0.0.1", help="Target host IP (default 127.0.0.1)")
    parser.add_argument("--rate", type=int, default=1000, help="Packet rate in Hz (default 1000)")
    parser.add_argument("--count", type=int, default=0, help="Total packets to send (0 = infinite, default 0)")
    parser.add_argument("--pattern", type=str, choices=["menu-navigate", "swing-bat", "sine-jitter", "chaos-drop"], 
                        default="menu-navigate", help="Input playback pattern")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target = (args.host, args.port)
    interval = 1.0 / args.rate
    seq = 0
    start_time = time.time()
    
    print(f"UDP Spoofer blasting '{args.pattern}' to {args.host}:{args.port} at {args.rate}Hz...")
    
    try:
        while True:
            if args.count > 0 and seq >= args.count:
                break
                
            now = time.time()
            ts_us = int((now - start_time) * 1_000_000)
            t = now - start_time
            
            # Default values
            buttons = 0x0000
            accel = (512, 512, 512)
            gyro = (0, 0, 0)
            ir = (512, 512)
            
            # Select pattern
            if args.pattern == "menu-navigate":
                # Circle movement for pointer
                cx = 512 + int(200 * math.sin(2 * math.pi * t * 0.5))
                cy = 384 + int(150 * math.cos(2 * math.pi * t * 0.5))
                ir = (cx, cy)
                # Press A button (0x0008) every 2 seconds for 100ms
                if int(t) % 2 == 0 and (t % 1.0) < 0.1:
                    buttons = 0x0008
                    
            elif args.pattern == "swing-bat":
                # Static pointer
                ir = (512, 384)
                # Simulate swing motion every 1.5 seconds: high acceleration spike on X/Y axis
                phase = (t % 1.5)
                if phase < 0.15:
                    # Swing phase: acceleration ramps up
                    acc_val = int(512 + 400 * math.sin(math.pi * (phase / 0.15)))
                    gyro_val = int(2000 * math.sin(math.pi * (phase / 0.15)))
                    accel = (acc_val, 512, 512)
                    gyro = (gyro_val, 0, 0)
                    buttons = 0x0008 # Hold A button during swing
                else:
                    # Rest phase
                    accel = (512, 512, 512)
                    gyro = (0, 0, 0)
                    
            elif args.pattern == "sine-jitter":
                # Quick cursor jitter
                cx = 512 + int(10 * math.sin(2 * math.pi * t * 20))
                cy = 384 + int(10 * math.cos(2 * math.pi * t * 20))
                ir = (cx, cy)
                
            elif args.pattern == "chaos-drop":
                # Simulates drop out: do not send package with 10% probability
                if (int(t * 1000) % 10) == 0:
                    seq += 1
                    time.sleep(interval)
                    continue
                cx = 512 + int(150 * math.sin(2 * math.pi * t * 0.2))
                cy = 384 + int(100 * math.cos(2 * math.pi * t * 0.2))
                ir = (cx, cy)

            packet = generate_report(ts_us, seq, buttons, accel, gyro, ir)
            sock.sendto(packet, target)
            seq += 1
            
            # Precise sleep mapping to Hz rate
            elapsed = time.time() - now
            if elapsed < interval:
                time.sleep(interval - elapsed)
                
    except KeyboardInterrupt:
        print("\nSpoofer interrupted by user.")
    finally:
        sock.close()
        print(f"Sent {seq} packets to {args.host}:{args.port}.")

if __name__ == "__main__":
    main()
