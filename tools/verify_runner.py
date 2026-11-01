#!/usr/bin/env python3
"""Cross-platform Verification Pipeline Runner for Mini-Git Safeguards (C1)."""

import glob
import os
import shutil
import subprocess
import sys
import tempfile

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

def run_command(cmd, cwd=ROOT_DIR):
    res = subprocess.run(cmd, cwd=cwd, shell=isinstance(cmd, str))
    return res.returncode == 0

def find_make():
    for m in ["mingw32-make", "make"]:
        if shutil.which(m):
            return m
    return "make"

def find_bash():
    b = shutil.which("bash")
    if b:
        return b
    git_bash = r"C:\Program Files\Git\bin\bash.exe"
    if os.path.exists(git_bash):
        return git_bash
    return None

def main():
    print("============================================================")
    print("          Mini-Git Full Verification Pipeline (CI)          ")
    print("============================================================")

    # Add compiler directory to PATH on Windows if found
    gpp = shutil.which("g++")
    if gpp:
        gpp_dir = os.path.dirname(gpp)
        os.environ["PATH"] = gpp_dir + os.pathsep + os.environ.get("PATH", "")

    make_bin = find_make()
    summary = []
    total_failed = 0

    def step(name, func):
        nonlocal total_failed
        print(f"\n>>> Running step: {name}")
        try:
            ok = func()
        except Exception as e:
            print(f"[ERROR] Exception during {name}: {e}")
            ok = False

        if ok:
            summary.append((name, "PASS"))
            print(f">>> [PASS] {name}")
        else:
            summary.append((name, "FAIL"))
            print(f">>> [FAIL] {name}")
            total_failed += 1

    # 1. Clean build with -Werror
    def do_build():
        subprocess.run([make_bin, "clean"], cwd=ROOT_DIR, check=True)
        cxx = os.environ.get("CXX", "")
        cxxflags = "-std=c++17 -Wall -Wextra -Wpedantic -Werror -O2 -Iinclude -pthread"
        cmd = [make_bin, "-j4", "all", "test-build", f"CXXFLAGS={cxxflags}"]
        if cxx:
            cmd.append(f"CXX={cxx}")
        res = subprocess.run(cmd, cwd=ROOT_DIR)
        return res.returncode == 0

    step("Clean Build (-Werror)", do_build)

    # Detect test executable extension (.exe on Windows)
    exe = ".exe" if os.path.exists(os.path.join(ROOT_DIR, "build", "test_fixes.exe")) or sys.platform == "win32" else ""

    # 2. Four C++ test suites
    def run_suite(target):
        bin_path = os.path.join(ROOT_DIR, "build", f"{target}{exe}")
        if not os.path.exists(bin_path):
            bin_path = os.path.join(ROOT_DIR, "build", target)
        res = subprocess.run([bin_path], cwd=ROOT_DIR)
        return res.returncode == 0

    step("Test Suite: test_fixes", lambda: run_suite("test_fixes"))
    step("Test Suite: test_p2p", lambda: run_suite("test_p2p"))
    step("Test Suite: test_cli_server", lambda: run_suite("test_cli_server"))
    step("Test Suite: test_core_fixes", lambda: run_suite("test_core_fixes"))

    # 3. End-to-end demo
    def do_demo():
        bash_bin = find_bash()
        if not bash_bin:
            print("[WARN] bash not found; skipping bash demo.sh")
            return True
        demo_script = os.path.join(ROOT_DIR, "tests", "demo.sh")
        res = subprocess.run([bash_bin, demo_script], cwd=ROOT_DIR)
        return res.returncode == 0

    step("End-to-End Demo (tests/demo.sh)", do_demo)

    # 4. Frontend tests
    import verify_checks
    step("Frontend Node Tests", verify_checks.check_frontend_tests)

    # 5. Script block syntax check
    step("Script Syntax Check (node --check)", verify_checks.check_script_syntax)

    # 6. FrontendTemplate.h byte identity
    step("FrontendTemplate.h Byte Identity", verify_checks.check_header_identity)

    # 7. Grep src/ for forbidden patterns
    step("Forbidden Pattern Grep (src/)", verify_checks.check_forbidden_patterns)

    # Print Summary Table
    print("\n============================================================")
    print("                    VERIFICATION SUMMARY                    ")
    print("============================================================")
    print(f"{'Verification Step':<46} | {'Result'}")
    print("-----------------------------------------------+------------")
    for s_name, s_res in summary:
        print(f"{s_name:<46} | {s_res}")
    print("============================================================")

    if total_failed == 0:
        print("ALL VERIFICATION CHECKS PASSED!")
        sys.exit(0)
    else:
        print(f"VERIFICATION FAILED: {total_failed} step(s) failed.")
        sys.exit(1)

if __name__ == "__main__":
    main()
