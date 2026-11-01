#!/usr/bin/env python3
"""Automated verification checks for Mini-Git safeguards (C1).

Covers:
- Syntax check: node --check on every <script> block pulled out of templates/frontend.html
- Frontend tests: execution of all tests/frontend/*.test.js with node
- Header check: verifying include/FrontendTemplate.h is byte-identical to a freshly generated one
- Forbidden pattern check: scanning src/ for system(, popen(, argv[0] used to detect tests, and getenv("TEST
"""

import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

def find_node():
    nb = shutil.which("node") or shutil.which("node.exe")
    if nb:
        return nb
    for cand in glob.glob(r"C:\hostedtoolcache\windows\node\*\x64\node.exe") + [
        r"C:\Program Files\nodejs\node.exe",
        r"C:\Program Files (x86)\nodejs\node.exe",
    ]:
        if os.path.exists(cand):
            return cand
    return "node"

def check_frontend_tests():
    pattern = os.path.join(ROOT_DIR, "tests", "frontend", "*.test.js")
    test_files = sorted(glob.glob(pattern))
    if not test_files:
        print("[FAIL] No frontend test files found matching tests/frontend/*.test.js")
        return False

    node_bin = find_node()
    all_passed = True
    for tf in test_files:
        rel = os.path.relpath(tf, ROOT_DIR)
        print(f"  Running {rel}...")
        res = subprocess.run([node_bin, tf], cwd=ROOT_DIR)
        if res.returncode != 0:
            print(f"[FAIL] {rel} exited with code {res.returncode}")
            all_passed = False
        else:
            print(f"  [OK] {rel}")
    return all_passed

def check_script_syntax():
    html_file = os.path.join(ROOT_DIR, "templates", "frontend.html")
    if not os.path.exists(html_file):
        print(f"[FAIL] Missing {html_file}")
        return False

    with open(html_file, "r", encoding="utf-8") as f:
        html = f.read()

    # Extract all inline script blocks (ignore scripts with external src=)
    script_pattern = re.compile(r'<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)</script>', re.IGNORECASE)
    matches = script_pattern.findall(html)
    if not matches:
        print("[FAIL] No inline <script> blocks found in frontend.html")
        return False

    print(f"  Found {len(matches)} inline <script> block(s) to validate with node --check...")
    node_bin = find_node()
    all_passed = True
    for idx, code in enumerate(matches):
        with tempfile.NamedTemporaryFile(suffix=".js", delete=False, mode="w", encoding="utf-8") as tmp:
            tmp.write(code)
            tmp_path = tmp.name

        try:
            res = subprocess.run([node_bin, "--check", tmp_path], capture_output=True, text=True)
            if res.returncode != 0:
                print(f"[FAIL] <script> block {idx + 1} syntax error:\n{res.stderr.strip()}")
                all_passed = False
            else:
                print(f"  [OK] <script> block {idx + 1} syntax check passed")
        finally:
            if os.path.exists(tmp_path):
                try:
                    os.remove(tmp_path)
                except OSError:
                    pass

    return all_passed

def check_header_identity():
    header_path = os.path.join(ROOT_DIR, "include", "FrontendTemplate.h")
    if not os.path.exists(header_path):
        print(f"[FAIL] {header_path} does not exist.")
        return False

    with open(header_path, "rb") as f:
        current_bytes = f.read().replace(b"\r\n", b"\n")

    # Run embed_frontend.py to regenerate
    embed_script = os.path.join(ROOT_DIR, "tools", "embed_frontend.py")
    res = subprocess.run([sys.executable, embed_script], cwd=ROOT_DIR, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[FAIL] tools/embed_frontend.py failed:\n{res.stderr}")
        return False

    with open(header_path, "rb") as f:
        fresh_bytes = f.read().replace(b"\r\n", b"\n")

    if current_bytes != fresh_bytes:
        print(f"[FAIL] include/FrontendTemplate.h was NOT byte-identical to freshly generated output!")
        print(f"       Current size: {len(current_bytes)} bytes, Fresh size: {len(fresh_bytes)} bytes.")
        print(f"       Run 'python tools/embed_frontend.py' and commit include/FrontendTemplate.h.")
        return False

    print("  [OK] include/FrontendTemplate.h is byte-identical to freshly generated template.")
    return True

def check_forbidden_patterns():
    # Grep src/ for system(, popen(, argv[0] used to detect tests, and getenv("TEST
    forbidden_tokens = [
        'system(',
        'popen(',
        'getenv("TEST',
        'argv[0]'
    ]

    violations = []
    src_dir = os.path.join(ROOT_DIR, "src")

    for root, _, files in os.walk(src_dir):
        for fname in files:
            if fname.endswith((".cpp", ".h")):
                fpath = os.path.join(root, fname)
                with open(fpath, "r", encoding="utf-8", errors="ignore") as f:
                    content = f.read()
                    for token in forbidden_tokens:
                        if token in content:
                            violations.append((os.path.relpath(fpath, ROOT_DIR), token))

    if violations:
        print("[FAIL] Forbidden patterns found in src/:")
        for path, token in violations:
            print(f"       {path}: forbidden '{token}'")
        return False

    print("  [OK] No forbidden system/popen/test-detection calls found in src/.")
    return True

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "--all"

    success = True
    if mode in ("--frontend-tests", "--all"):
        print("\n--- Running Frontend Node Tests ---")
        if not check_frontend_tests():
            success = False

    if mode in ("--syntax-check", "--all"):
        print("\n--- Running Script Syntax Check (node --check) ---")
        if not check_script_syntax():
            success = False

    if mode in ("--header-check", "--all"):
        print("\n--- Checking FrontendTemplate.h Freshness ---")
        if not check_header_identity():
            success = False

    if mode in ("--grep-check", "--all"):
        print("\n--- Grepping src/ for Forbidden Patterns ---")
        if not check_forbidden_patterns():
            success = False

    sys.exit(0 if success else 1)
