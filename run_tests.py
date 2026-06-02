#!/usr/bin/env python3
import os
import sys
import time
import subprocess
import platform

# ANSI Color Codes
RESET = "\033[0m"
BOLD = "\033[1m"
RED = "\033[31m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
CYAN = "\033[36m"
WHITE = "\033[37m"

def clear_screen():
    os.system('cls' if platform.system() == 'Windows' else 'clear')

def get_source_files(dirs):
    """Recursively collect source files and their modification times."""
    src_files = {}
    for d in dirs:
        if not os.path.exists(d):
            continue
        for root, _, files in os.walk(d):
            for file in files:
                if file.endswith(('.cpp', '.h', '.hpp', '.c', '.cc')):
                    full_path = os.path.join(root, file)
                    try:
                        src_files[full_path] = os.path.getmtime(full_path)
                    except OSError:
                        pass
    return src_files

def compile_target(compiler, source_files, output_bin, flags):
    """Compiles source files into an output binary."""
    cmd = [compiler] + flags + source_files + ["-o", output_bin]
    # Remove empty elements
    cmd = [c for c in cmd if c]
    
    print(f"{CYAN}Compiling:{RESET} {' '.join(cmd)}")
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode != 0:
        print(f"{RED}Compilation FAILED for {output_bin}!{RESET}")
        print(f"{RED}Errors:\n{result.stderr}{RESET}")
        return False, result.stderr
    
    if result.stderr:
        print(f"{YELLOW}Compiled with warnings:\n{result.stderr}{RESET}")
    else:
        print(f"{GREEN}Compiled successfully.{RESET}")
    return True, result.stderr

def run_executable(bin_path, args=None, timeout=10.0):
    """Runs a test binary and returns execution status, output, and duration."""
    if args is None:
        args = []
    cmd = [bin_path] + args
    start_time = time.time()
    try:
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout)
        duration = time.time() - start_time
        if result.returncode == 0:
            return True, result.stdout, duration
        else:
            return False, result.stdout + "\n" + result.stderr, duration
    except subprocess.TimeoutExpired:
        duration = time.time() - start_time
        return False, f"Test execution timed out after {timeout}s", duration
    except Exception as e:
        duration = time.time() - start_time
        return False, f"Execution failed: {str(e)}", duration

def execute_test_suite():
    workspace_dir = os.path.dirname(os.path.abspath(__file__))
    is_windows = platform.system() == "Windows"
    exe_suffix = ".exe" if is_windows else ""
    
    # Configure compiler & flags
    compiler = "clang++"
    common_flags = ["-std=c++17", "-O3", "-Wall", "-pthread"]
    
    # Check if clang++ exists
    try:
        subprocess.run([compiler, "--version"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except FileNotFoundError:
        # Fall back to g++
        compiler = "g++"
        try:
            subprocess.run([compiler, "--version"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except FileNotFoundError:
            print(f"{RED}Error: Neither clang++ nor g++ was found in system PATH. Cannot compile tests.{RESET}")
            return False

    # Target configuration
    targets = {
        "SluggersProxy": {
            "sources": [os.path.join(workspace_dir, "proxy", "src", "main.cpp")],
            "output": os.path.join(workspace_dir, "proxy", f"SluggersProxy{exe_suffix}"),
            "flags": common_flags,
            "type": "compile"
        },
        "dolphin_test": {
            "sources": [
                os.path.join(workspace_dir, "dolphin", "dolphin_test.cpp"),
                os.path.join(workspace_dir, "dolphin", "input_injection.cpp")
            ],
            "output": os.path.join(workspace_dir, "dolphin", f"dolphin_test{exe_suffix}"),
            "flags": common_flags,
            "type": "compile"
        },
        "integration_test": {
            "sources": [
                os.path.join(workspace_dir, "dolphin", "integration_test.cpp"),
                os.path.join(workspace_dir, "dolphin", "input_injection.cpp")
            ],
            "output": os.path.join(workspace_dir, "dolphin", f"integration_test{exe_suffix}"),
            "flags": common_flags + (["-lws2_32"] if is_windows else []),
            "type": "compile"
        },
        "stress_test": {
            "sources": [
                os.path.join(workspace_dir, "dolphin", "stress_test.cpp"),
                os.path.join(workspace_dir, "dolphin", "input_injection.cpp")
            ],
            "output": os.path.join(workspace_dir, "dolphin", f"stress_test{exe_suffix}"),
            "flags": common_flags + (["-lws2_32"] if is_windows else []),
            "type": "compile"
        }
    }

    print(f"\n{BOLD}{WHITE}=== 1. COMPILATION PHASE ==={RESET}")
    compilation_results = {}
    any_compile_failed = False
    
    for name, info in targets.items():
        print(f"\n{BOLD}Building {name}...{RESET}")
        success, err = compile_target(compiler, info["sources"], info["output"], info["flags"])
        compilation_results[name] = success
        if not success:
            any_compile_failed = True
            
    if any_compile_failed:
        print(f"\n{RED}{BOLD}Compilation failed for one or more targets. Aborting test execution.{RESET}")
        return False

    print(f"\n{BOLD}{WHITE}=== 2. RUNNING TEST SUITES ==={RESET}")
    test_results = []
    
    # Test 1: Proxy timing and structure diagnostics
    print(f"\n{BOLD}Running: Proxy Verification Diagnostics (verify_proxy.py)...{RESET}")
    pypath = sys.executable or "python3"
    start_time = time.time()
    try:
        res = subprocess.run([pypath, os.path.join(workspace_dir, "verify_proxy.py")], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=15.0)
        duration = time.time() - start_time
        if res.returncode == 0:
            print(f"{GREEN}✓ Proxy Verification Passed!{RESET}")
            # Try to grab Jitter output
            jitter_info = "Success"
            for line in res.stdout.split("\n"):
                if "Jitter (Standard Deviation):" in line or "Average Frequency:" in line:
                    print(f"  {line.strip()}")
                    if "Jitter" in line:
                        jitter_info = line.strip()
            test_results.append(("Proxy Verification", "PASS", duration, jitter_info))
        else:
            print(f"{RED}✗ Proxy Verification FAILED!{RESET}")
            print(res.stdout + "\n" + res.stderr)
            test_results.append(("Proxy Verification", "FAIL", duration, "Daemon or Timing failure"))
    except Exception as e:
        duration = time.time() - start_time
        print(f"{RED}✗ Proxy Verification Exec FAILED: {e}{RESET}")
        test_results.append(("Proxy Verification", "FAIL", duration, str(e)))

    # Test 2: Dolphin Unit Tests
    print(f"\n{BOLD}Running: Dolphin Unit Tests (dolphin_test)...{RESET}")
    ok, out, dur = run_executable(targets["dolphin_test"]["output"])
    if ok:
        print(f"{GREEN}✓ Dolphin Unit Tests Passed!{RESET}")
        test_results.append(("Dolphin Unit Tests", "PASS", dur, "Jitter buffer LERP & Hermite spline verified"))
    else:
        print(f"{RED}✗ Dolphin Unit Tests FAILED!{RESET}")
        print(out)
        test_results.append(("Dolphin Unit Tests", "FAIL", dur, "Unit test assertion failed"))

    # Test 3: Dolphin Integration Tests
    print(f"\n{BOLD}Running: Dolphin Socket Integration (integration_test)...{RESET}")
    ok, out, dur = run_executable(targets["integration_test"]["output"], timeout=10.0)
    if ok:
        print(f"{GREEN}✓ Dolphin Integration Tests Passed!{RESET}")
        test_results.append(("Dolphin Integration Tests", "PASS", dur, "Mock 1000Hz sender & tick ingestion verified"))
    else:
        print(f"{RED}✗ Dolphin Integration Tests FAILED!{RESET}")
        print(out)
        test_results.append(("Dolphin Integration Tests", "FAIL", dur, "Socket communication or parser failure"))

    # Test 4: Dolphin Stress Tests
    print(f"\n{BOLD}Running: Dolphin Stress Tests (stress_test)...{RESET}")
    ok, out, dur = run_executable(targets["stress_test"]["output"])
    if ok:
        print(f"{GREEN}✓ Dolphin Stress Tests Passed!{RESET}")
        test_results.append(("Dolphin Stress Tests", "PASS", dur, "Out-of-order, dropped packets, and struct packing verified"))
    else:
        print(f"{RED}✗ Dolphin Stress Tests FAILED!{RESET}")
        print(out)
        test_results.append(("Dolphin Stress Tests", "FAIL", dur, "Edge case regression or parser mismatch"))

    # Print Summary Table
    print(f"\n{BOLD}{WHITE}=== 3. SUMMARY DIAGNOSTIC REPORT ==={RESET}\n")
    print(f"{BOLD}{'Test Name':<30} | {'Status':<6} | {'Duration (s)':<12} | {'Notes':<50}{RESET}")
    print("-" * 105)
    
    all_passed = True
    for name, status, dur, note in test_results:
        color = GREEN if status == "PASS" else RED
        print(f"{name:<30} | {color}{status:<6}{RESET} | {dur:<12.3f} | {note:<50}")
        if status != "PASS":
            all_passed = False
            
    print("-" * 105)
    if all_passed:
        print(f"\n{GREEN}{BOLD}🎉 ALL VERIFICATION CHECKS PASSED! NO GAME BREAKING ISSUES DETECTED.{RESET}\n")
    else:
        print(f"\n{RED}{BOLD}🚨 REGRESSION WARNING: One or more test suites failed! Please fix issues before deploying.{RESET}\n")
        
    return all_passed

def watch_mode():
    workspace_dir = os.path.dirname(os.path.abspath(__file__))
    watch_dirs = [
        os.path.join(workspace_dir, "proxy"),
        os.path.join(workspace_dir, "dolphin")
    ]
    
    print(f"{CYAN}{BOLD}Initializing continuous testing environment in Watch Mode...{RESET}")
    print(f"{CYAN}Watching for changes in: {', '.join(watch_dirs)}{RESET}")
    print(f"{CYAN}Press Ctrl+C to terminate the watcher.{RESET}\n")
    
    # Initial scan and test execution
    tracked_files = get_source_files(watch_dirs)
    execute_test_suite()
    
    try:
        while True:
            time.sleep(1.0)
            current_scan = get_source_files(watch_dirs)
            
            # Check for changes, additions, or deletions
            changed = False
            
            # Check for modified or deleted files
            for file, mtime in tracked_files.items():
                if file not in current_scan:
                    print(f"\n{YELLOW}[Watcher] File deleted: {os.path.basename(file)}{RESET}")
                    changed = True
                    break
                elif current_scan[file] > mtime:
                    print(f"\n{YELLOW}[Watcher] File modified: {os.path.basename(file)}{RESET}")
                    changed = True
                    break
            
            # Check for added files
            if not changed:
                for file in current_scan:
                    if file not in tracked_files:
                        print(f"\n{YELLOW}[Watcher] File added: {os.path.basename(file)}{RESET}")
                        changed = True
                        break
            
            if changed:
                print(f"{CYAN}Triggering automated re-build and regression validation...{RESET}")
                clear_screen()
                print(f"{CYAN}{BOLD}[Watcher Alert] Code change detected. Running diagnostics...{RESET}")
                execute_test_suite()
                tracked_files = current_scan
                print(f"\n{CYAN}Watching for changes... (Ctrl+C to exit){RESET}")
                
    except KeyboardInterrupt:
        print(f"\n{YELLOW}Watcher terminated by user. Goodbye!{RESET}")

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--watch":
        watch_mode()
    else:
        success = execute_test_suite()
        sys.exit(0 if success else 1)
