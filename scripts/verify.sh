#!/usr/bin/env bash
set -u

# Ensure running from repository root
cd "$(dirname "$0")/.." || exit 1
REPO_ROOT="$(pwd)"

# Ensure compiler runtime DLLs are found on Windows / MinGW
if command -v g++ >/dev/null 2>&1; then
    COMPILER_DIR="$(dirname "$(which g++ 2>/dev/null)")"
    export PATH="$COMPILER_DIR:$PATH"
fi

# Locate working Python (avoid Windows Store App execution alias)
PYTHON=""
for candidate in python3 python py; do
    if command -v "$candidate" >/dev/null 2>&1; then
        if "$candidate" -c "import sys; sys.exit(0)" >/dev/null 2>&1; then
            PYTHON="$candidate"
            break
        fi
    fi
done

if [ -z "$PYTHON" ]; then
    # Look for installed Python in standard Windows locations
    for p in /c/Users/*/AppData/Local/Programs/Python/Python*/python.exe /c/Python*/python.exe /c/msys64/ucrt64/bin/python3.exe /c/msys64/mingw64/bin/python3.exe; do
        if [ -f "$p" ]; then
            PYTHON="$p"
            export PATH="$(dirname "$p"):$PATH"
            break
        fi
    done
fi

if [ -z "$PYTHON" ]; then
    echo "Error: Working Python 3 executable could not be found." >&2
    exit 1
fi

# Locate working Node.js
NODE="node"
if ! command -v node >/dev/null 2>&1; then
    for cand in /c/hostedtoolcache/windows/node/*/x64/node.exe "/c/Program Files/nodejs/node.exe" "/c/Program Files (x86)/nodejs/node.exe"; do
        if [ -f "$cand" ]; then
            NODE="$cand"
            export PATH="$(dirname "$cand"):$PATH"
            break
        fi
    done
fi

# Pick make executable
if command -v mingw32-make >/dev/null 2>&1; then
    MAKE=mingw32-make
elif command -v make >/dev/null 2>&1; then
    MAKE=make
else
    echo "Error: make/mingw32-make not found in PATH" >&2
    exit 1
fi

echo "============================================================"
echo "          Mini-Git Full Verification Pipeline (CI)          "
echo "============================================================"
echo "Using Compiler: $($MAKE -v 2>&1 | head -n 1)"
echo "Using Python:   $($PYTHON --version 2>&1)"
echo "Using Node:     $($NODE --version 2>&1)"

SUMMARY=()
TOTAL_FAILED=0

run_step() {
    local label="$1"
    shift
    echo ""
    echo ">>> Running step: $label"
    "$@"
    local rc=$?
    if [ $rc -eq 0 ]; then
        SUMMARY+=("$label|PASS")
        echo ">>> [PASS] $label"
    else
        SUMMARY+=("$label|FAIL")
        echo ">>> [FAIL] $label (exit code $rc)"
        TOTAL_FAILED=$((TOTAL_FAILED + 1))
    fi
}

# 1. Clean build with -Wall -Wextra -Wpedantic -Werror
build_step() {
    $MAKE clean
    $MAKE all test-build CXX="${CXX:-g++}" CXXFLAGS="-std=c++17 -Wall -Wextra -Wpedantic -Werror -O2 -Iinclude -pthread"
}
run_step "Clean Build (-Werror)" build_step

# Locate test binaries (.exe on Windows, bare binary on Linux)
EXE=""
if [ -f "build/test_fixes.exe" ]; then
    EXE=".exe"
fi

# 2. Run all 4 C++ suites
run_step "Test Suite: test_fixes" "./build/test_fixes$EXE"
run_step "Test Suite: test_p2p" "./build/test_p2p$EXE"
run_step "Test Suite: test_cli_server" "./build/test_cli_server$EXE"
run_step "Test Suite: test_core_fixes" "./build/test_core_fixes$EXE"

# 3. Run demo script
demo_step() {
    bash tests/demo.sh
}
run_step "End-to-End Demo (tests/demo.sh)" demo_step

# 4. Frontend tests
run_step "Frontend Node Tests" "$PYTHON" tools/verify_checks.py --frontend-tests

# 5. Script block syntax check
run_step "Script Syntax Check (node --check)" "$PYTHON" tools/verify_checks.py --syntax-check

# 6. FrontendTemplate.h byte identity
run_step "FrontendTemplate.h Byte Identity" "$PYTHON" tools/verify_checks.py --header-check

# 7. Grep src/ for forbidden patterns
run_step "Forbidden Pattern Grep (src/)" "$PYTHON" tools/verify_checks.py --grep-check

# Print Summary Table
echo ""
echo "============================================================"
echo "                    VERIFICATION SUMMARY                    "
echo "============================================================"
printf "%-46s | %s\n" "Verification Step" "Result"
echo "-----------------------------------------------+------------"
for item in "${SUMMARY[@]}"; do
    IFS='|' read -r step result <<< "$item"
    printf "%-46s | %s\n" "$step" "$result"
done
echo "============================================================"

if [ $TOTAL_FAILED -eq 0 ]; then
    echo "ALL VERIFICATION CHECKS PASSED!"
    exit 0
else
    echo "VERIFICATION FAILED: $TOTAL_FAILED step(s) failed."
    exit 1
fi
