#!/usr/bin/env python3
import os
import sys
import shutil
import subprocess
import time
import argparse

# Config content generators for Dolphin.ini
def get_dolphin_ini(is_host, port):
    role = "1" if is_host else "2"
    return f"""[Interface]
ConfirmStop = False
UsePanicHandlers = False
[NetPlay]
Port = {port}
Nickname = {"HostPlayer" if is_host else "ClientPlayer"}
Address = 127.0.0.1
ConnectPort = 5555
[Core]
DoubleCore = False
DSPHLE = True
[Analytics]
Enabled = False
"""

def get_wiimote_ini(local_slot):
    # Configure Wiimote to listen on local socket injected ports
    return f"""[Wiimote1]
Source = 2
[Wiimote2]
Source = 2
[Wiimote3]
Source = 0
[Wiimote4]
Source = 0
"""

def setup_instance(base_path, name, is_host, port, local_slot):
    instance_dir = os.path.join(base_path, name)
    print(f"[*] Setting up isolated instance directory: {instance_dir}")
    os.makedirs(instance_dir, exist_ok=True)
    
    # 1. Create portable.txt to force isolation
    with open(os.path.join(instance_dir, "portable.txt"), "w") as f:
        f.write("")
        
    # 2. Setup Config folders
    config_dir = os.path.join(instance_dir, "User", "Config")
    os.makedirs(config_dir, exist_ok=True)
    
    # 3. Write isolated INI files
    with open(os.path.join(config_dir, "Dolphin.ini"), "w") as f:
        f.write(get_dolphin_ini(is_host, port))
        
    with open(os.path.join(config_dir, "WiimoteNew.ini"), "w") as f:
        f.write(get_wiimote_ini(local_slot))
        
    return instance_dir

def main():
    parser = argparse.ArgumentParser(description="Mario Super Sluggers Online: Local Desk Setup Test Suite")
    parser.add_argument("--dolphin-bin", type=str, help="Path to custom Dolphin executable")
    parser.add_argument("--rom", type=str, help="Path to Mario Super Sluggers WBFS/ISO ROM")
    parser.add_argument("--run", action="store_true", help="Launch the configured instances and tests")
    args = parser.parse_args()

    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    test_suite_dir = os.path.join(base_dir, "local_test_suite")
    
    print("==================================================")
    # 1. Configure Host Instance (P1)
    host_dir = setup_instance(test_suite_dir, "dolphin-host", is_host=True, port=5555, local_slot=0)
    
    # 2. Configure Client Instance (P2)
    client_dir = setup_instance(test_suite_dir, "dolphin-client", is_host=False, port=5556, local_slot=1)
    print("==================================================")
    print("[Success] Isolated directories and configurations generated.")
    
    if args.run:
        if not args.dolphin_bin or not os.path.exists(args.dolphin_bin):
            print("[Error] Must specify a valid path to compiled custom --dolphin-bin to run.")
            sys.exit(1)
        if not args.rom or not os.path.exists(args.rom):
            print("[Error] Must specify a valid path to game --rom to run.")
            sys.exit(1)

        print("[*] Spawning Host (P1) instance...")
        # Point to portable folder User dir
        host_proc = subprocess.Popen([args.dolphin_bin, "-u", os.path.join(host_dir, "User"), "-e", args.rom])
        time.sleep(1.0)
        
        print("[*] Spawning Client (P2) instance...")
        client_proc = subprocess.Popen([args.dolphin_bin, "-u", os.path.join(client_dir, "User"), "-e", args.rom])
        
        print("\n[*] Isolated Emulators running side-by-side.")
        print("    Now perform these steps:")
        print("    1. In Host Dolphin, open Netplay window, select host ROM.")
        print("    2. In Client Dolphin, join 127.0.0.1:5555.")
        print("    3. Degrade loopback network (e.g. using Network Link Conditioner / Clumsy).")
        print("    4. Run 'python3 tools/desync_catcher.py --mock' (or live socket mode).")
        print("    5. Run 'python3 tools/udp_spoofer.py --pattern swing-bat'.")
        
        try:
            while host_proc.poll() is None or client_proc.poll() is None:
                time.sleep(1.0)
        except KeyboardInterrupt:
            print("\n[*] Terminating emulator processes...")
            host_proc.terminate()
            client_proc.terminate()
            print("[*] Done.")
    else:
        print("\nTo run the local test suite automation, pass the --run option along with paths:")
        print(f"  python3 tools/desk_setup_test.py --run --dolphin-bin /path/to/Dolphin.app/Contents/MacOS/Dolphin --rom /path/to/RMBE01.wbfs")

if __name__ == "__main__":
    main()
