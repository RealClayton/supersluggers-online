#!/usr/bin/env python3
import socket
import struct
import hashlib
import time
import os
import sys
import argparse
import subprocess
import platform

# MEM1 constants for Wii
MEM1_START = 0x80000000
MEM1_SIZE = 24 * 1024 * 1024  # 24 MB

class DolphinMemoryReader:
    """Reads MEM1 from the local Dolphin process using native shared memory."""
    def __init__(self, use_mock=False):
        self.use_mock = use_mock
        self.shm_file = None
        self.is_windows = platform.system() == "Windows"
        
        if not self.use_mock:
            try:
                self.initialize_memory_mapping()
            except Exception as e:
                print(f"[Warning] Failed to bind to live Dolphin memory: {e}. Falling back to MOCK mode.")
                self.use_mock = True

    def initialize_memory_mapping(self):
        if self.is_windows:
            # On Windows, Dolphin uses a named file mapping object
            import ctypes
            from ctypes import wintypes
            
            # Try to open the PhysicalMemory mapping
            FILE_MAP_READ = 0x0004
            INVALID_HANDLE_VALUE = -1
            
            # Try opening Dolphin memory mapping
            # "PhysicalMemory" or "dolphin-emu" depending on fork/version
            self.h_map = ctypes.windll.kernel32.OpenFileMappingW(FILE_MAP_READ, False, "PhysicalMemory")
            if not self.h_map:
                self.h_map = ctypes.windll.kernel32.OpenFileMappingW(FILE_MAP_READ, False, "dolphin-emu")
                
            if not self.h_map:
                raise OSError("Could not locate Windows Dolphin file mapping.")
                
            self.p_mem = ctypes.windll.kernel32.MapViewOfFile(self.h_map, FILE_MAP_READ, 0, 0, MEM1_SIZE)
            if not self.p_mem:
                ctypes.windll.kernel32.CloseHandle(self.h_map)
                raise OSError("Could not map view of Dolphin memory.")
        else:
            # On macOS/Linux, Dolphin exposes a POSIX shared memory file under /dev/shm or /tmp
            # Typically mapped as '/dolphin-emu' which translates to /dev/shm/dolphin-emu
            shm_paths = [
                "/dev/shm/dolphin-emu",
                "/tmp/dolphin-emu",
                "/dev/shm/dolphin-emu." + str(os.getuid()) if hasattr(os, 'getuid') else ""
            ]
            
            found = False
            for path in shm_paths:
                if path and os.path.exists(path):
                    self.shm_file = open(path, "rb")
                    found = True
                    break
            
            if not found:
                # On macOS POSIX shared memory might be accessed via shm_open, check standard fallback
                try:
                    import mmap
                    # Attempt open /dev/shm/dolphin-emu directly if mmap-able
                    fd = os.open("/dev/shm/dolphin-emu", os.O_RDONLY)
                    self.shm_mmap = mmap.mmap(fd, MEM1_SIZE, mmap.MAP_SHARED, mmap.PROT_READ)
                    found = True
                except Exception:
                    pass
                    
            if not found:
                raise OSError("Could not find Dolphin POSIX shared memory file.")

    def read_mem1(self):
        """Reads the entire 24MB MEM1 array or returns a deterministic mock."""
        if self.use_mock:
            # Return a mock representation of memory
            # Incorporates a frame counter so hash evolves over time deterministically
            frame = int(time.time() * 60) % 10000
            mock_data = bytearray(1024 * 1024)  # 1MB is enough for mock testing speed
            struct.pack_into("<I", mock_data, 0, frame) # Store frame at start
            # Write some dummy scoreboard values
            struct.pack_into("<HH", mock_data, 100, 3, 2) # Runs/Outs
            return bytes(mock_data)

        if self.is_windows:
            import ctypes
            buffer = ctypes.create_string_buffer(MEM1_SIZE)
            ctypes.memmove(buffer, self.p_mem, MEM1_SIZE)
            return buffer.raw
        else:
            if hasattr(self, 'shm_mmap'):
                self.shm_mmap.seek(0)
                return self.shm_mmap.read(MEM1_SIZE)
            elif self.shm_file:
                self.shm_file.seek(0)
                return self.shm_file.read(MEM1_SIZE)
            else:
                return b""

    def get_frame_number(self):
        """Reads the frame number from the target game offset (e.g. 0x80002F10) or mocks it."""
        data = self.read_mem1()
        if not data:
            return 0
        # If in Mock, it is at index 0
        if self.use_mock:
            return struct.unpack_from("<I", data, 0)[0]
        # In a real game, frame counter address must be determined. We read from start for now.
        return struct.unpack_from("<I", data, 0)[0]

    def close(self):
        if self.use_mock:
            return
        if self.is_windows:
            import ctypes
            ctypes.windll.kernel32.UnmapViewOfFile(self.p_mem)
            ctypes.windll.kernel32.CloseHandle(self.h_map)
        else:
            if self.shm_file:
                self.shm_file.close()

def kill_dolphin():
    """Kills local Dolphin instances on desync detection."""
    print("[Desync Catcher] Kiling local Dolphin emulator instances...")
    system = platform.system()
    try:
        if system == "Windows":
            subprocess.run(["taskkill", "/F", "/IM", "Dolphin.exe"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            subprocess.run(["killall", "-9", "Dolphin"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(["killall", "-9", "dolphin-emu"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print("[Desync Catcher] Emulator processes terminated successfully.")
    except Exception as e:
        print(f"[Warning] Failed to terminate Dolphin processes: {e}")

def run_server(port, mock=False):
    """Runs a daemon on the host that serves frame number and MEM1 hash."""
    reader = DolphinMemoryReader(use_mock=mock)
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        server_sock.bind(("0.0.0.0", port))
        server_sock.listen(1)
        print(f"[Desync Daemon] Server listening on port {port} (Mock: {mock})...")
        
        while True:
            conn, addr = server_sock.accept()
            print(f"[Desync Daemon] Connected to client from {addr}")
            try:
                while True:
                    # Expect a query byte
                    req = conn.recv(1)
                    if not req:
                        break
                    
                    mem = reader.read_mem1()
                    frame = reader.get_frame_number()
                    h = hashlib.sha256(mem).hexdigest()
                    
                    # Format: frame (4 bytes) + hash string (64 bytes)
                    payload = struct.pack("<I 64s", frame, h.encode('ascii'))
                    conn.sendall(payload)
                    time.sleep(0.5) # Sample at 2Hz rate
            except ConnectionError:
                print(f"[Desync Daemon] Client disconnected.")
            finally:
                conn.close()
    except KeyboardInterrupt:
        print("\n[Desync Daemon] Shutting down.")
    finally:
        server_sock.close()
        reader.close()

def run_client(remote_host, remote_port, mock=False):
    """Connects to the remote daemon and compares hashes against the local instance."""
    print(f"[Desync Client] Connecting to remote host at {remote_host}:{remote_port}...")
    
    local_reader = DolphinMemoryReader(use_mock=mock)
    client_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    
    try:
        client_sock.connect((remote_host, remote_port))
        print("[Desync Client] Connected. Starting real-time memory state synchronization...")
        
        while True:
            # Query remote state
            client_sock.sendall(b"\x01")
            data = client_sock.recv(68)
            if not data or len(data) < 68:
                print("[Error] Received invalid telemetry payload from remote host.")
                break
                
            remote_frame, remote_hash_bytes = struct.unpack("<I 64s", data)
            remote_hash = remote_hash_bytes.decode('ascii')
            
            # Read local state
            local_mem = local_reader.read_mem1()
            local_frame = local_reader.get_frame_number()
            local_hash = hashlib.sha256(local_mem).hexdigest()
            
            print(f"[Frame Match] Remote: {remote_frame} (Hash: {remote_hash[:8]}) | Local: {local_frame} (Hash: {local_hash[:8]})")
            
            if remote_hash != local_hash:
                print(f"\n==================================================")
                print(f"🔴 DESYNCHRONIZATION DETECTED AT FRAME {local_frame}!")
                print(f"  Remote Hash: {remote_hash}")
                print(f"  Local Hash:  {local_hash}")
                print(f"==================================================")
                
                # Instantly terminate local emulators
                kill_dolphin()
                break
                
            time.sleep(1.0) # Check every second
            
    except KeyboardInterrupt:
        print("\n[Desync Client] Terminated by user.")
    except Exception as e:
        print(f"[Desync Client Error] Connection failed: {e}")
    finally:
        client_sock.close()
        local_reader.close()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Automated Desync Catcher for Dolphin Emulators")
    parser.add_argument("--server", action="store_true", help="Run in Daemon server mode")
    parser.add_argument("--port", type=int, default=6000, help="Daemon port (default 6000)")
    parser.add_argument("--connect", type=str, help="Connect to target remote server IP (e.g. 192.168.1.50)")
    parser.add_argument("--mock", action="store_true", help="Use mock memory structures for unit/CI validation")
    args = parser.parse_args()

    # Default fallback to mock if no flags provided
    if not args.server and not args.connect:
        print("Please specify either --server or --connect <host_ip>.")
        sys.exit(1)
        
    if args.server:
        run_server(args.port, args.mock)
    elif args.connect:
        run_client(args.connect, args.port, args.mock)
