# Contributing to Mini-Git

Thank you for contributing to Mini-Git! Mini-Git is a zero-external-dependency, standard C++17 implementation of Git internals with a built-in P2P pairing engine and Web Studio UI.

To preserve reliability, performance, and security across all platforms, all contributions must strictly adhere to the following non-negotiable rules.

---

## Non-Negotiable Rules

### 1. Every Bug Fix Comes with a Regression Test
- **Test First**: For every bug or audit finding, write the regression test **FIRST**.
- Verify that the test **FAILS** against the current codebase before implementing the fix.
- Implement the minimal, robust fix, and verify that the regression test **PASSES**.
- Place regression tests in the matching suite:
  - `tests/test_fixes.cpp`: Integrity, delta/revert logic, undo, stashing.
  - `tests/test_p2p.cpp`: Network pairing, bundle export/import, sync protocols, cryptographic verification.
  - `tests/test_cli_server.cpp`: CLI argument parsing, HTTP endpoints, rate-limiting, concurrency, connection limits.
  - `tests/test_core_fixes.cpp`: Object store, LCA merge-base, tree resolution, filesystem safety.
  - `tests/frontend/*.test.js`: Headless Node.js tests for UI state, DAG layout, topological sort, XSS sanitization.
- Document new tests in `tests/README.md`.

### 2. Verification Must Pass Before Every Commit
- Before committing any change, run the verification pipeline:
  ```bash
  # Linux / macOS / MSYS2:
  ./scripts/verify.sh

  # Windows (Command Prompt / PowerShell):
  scripts\verify.bat
  ```
- The verification suite builds all code with `-Wall -Wextra -Wpedantic -Werror`. Any warning will immediately fail the build.
- All 4 C++ suites, the end-to-end integration demo (`tests/demo.sh`), all Node.js frontend tests, script syntax checks, and source audits must pass with exit code 0.

### 3. Never Weaken or Skip a Test
- Never weaken, comment out, disable, or delete an existing test assertion to obtain a pass.
- If a test fails, you must fix the underlying implementation in production code.
- If an existing requirement genuinely changes, clearly document the justification in the commit message and PR description.

### 4. Regenerate Frontend Header After UI Edits
- The Web Studio UI template is located at `templates/frontend.html`.
- Production builds embed this template directly into C++ via `include/FrontendTemplate.h`.
- After making any edits to `templates/frontend.html`, you **MUST** regenerate the C++ header:
  ```bash
  python tools/embed_frontend.py
  ```
- Commit both `templates/frontend.html` and `include/FrontendTemplate.h` together in the same commit.
- `scripts/verify.sh` verifies byte-for-byte identity between `include/FrontendTemplate.h` and a freshly generated template; any discrepancy will fail verification.

### 5. Production Code Must Never Detect Tests
- Production code in `src/` must **NEVER** inspect test names, binary names, `argv[0]`, `popen()`, `system()`, or test environment variables (`getenv("TEST...")`) to alter its behavior.
- Use documented, sanctioned interfaces only. `scripts/verify.sh` scans `src/` for forbidden patterns and blocks non-compliant code.

### 6. Filesystem and Untracked File Safety
- Never delete, overwrite, or clobber an untracked user file.
- All filesystem writes, deletions, and directory traversals inside the repository must pass through canonical path resolution and symlink-component safety checks to prevent directory traversal or outside-repo symlink attacks.

---

## Recommended Workflow

1. **Install Git Hooks**:
   Run the hook installer to prevent accidental broken pushes:
   ```bash
   bash tools/install-hooks.sh
   ```
2. **Make Changes**:
   Write your failing regression test, implement the fix or feature, and ensure the test passes.
3. **Run Full Verification**:
   ```bash
   ./scripts/verify.sh
   ```
4. **Commit with Atomic, Descriptive Messages**:
   Follow conventional naming conventions:
   ```bash
   git commit -m "Fix <item>: description of fix"
   ```
