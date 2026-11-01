# Mini-Git Test Suite & Audit Verification Matrix

This document provides a comprehensive mapping of every audit finding (Items 1–50) and follow-up remediation items (Part A: A1–A7, Part B: B1–B7) to their dedicated automated regression tests.

## Test Suites Overview

The test harness consists of five test categories:
1. `build/test_fixes`: Core security and integrity regression tests (Items 1–12).
2. `build/test_p2p`: P2P synchronization, pairing security, and bundle import tests (Items 19, 24–30, B3).
3. `build/test_cli_server`: CLI commands, HTTP server hardening, connection limits, and API tests (Items 17, 18, 21–23, 32, 34, 37, 41, 44, 47, A2, A4, A5, A6, B4, B5, B6).
4. `build/test_core_fixes`: Filesystem, LCA merge, atomic commit, and JSON parser tests (Items 13–16, 31, 33, 35, 36, 40, 42, 43, 46, 48–50).
5. `tests/frontend/*.test.js`: Headless Node.js regression tests verifying UI state machines, DAG layout, topological sort, and XSS sanitization (A1, A3, B1, B2, B7).
6. `tests/demo.sh`: End-to-end integration smoke test exercising CLI workflows.

---

## Complete Audit Mapping Matrix

| Item | Description | Test Location | Test Function / Target |
|:-----|:------------|:--------------|:-----------------------|
| **1** | Discard safety: untracked and modified files protected against accidental loss | `tests/test_fixes.cpp` | `test1()` |
| **2** | Revert delta & conflict detection: clean 3-way merge on revert | `tests/test_fixes.cpp` | `test2()` |
| **3** | Untracked overwrite protection: checkout refuses overwrite of untracked files | `tests/test_fixes.cpp` | `test3()` |
| **4** | Stash stack & dirty pop refusal: non-destructive pop refused on dirty tree | `tests/test_fixes.cpp` | `test4()` |
| **5** | XSS prevention & JSON escaping: HTML special characters escaped in API responses | `tests/test_fixes.cpp` | `test5()` |
| **6** | Git export injection safety: argv array execution without shell interpolation | `tests/test_fixes.cpp` | `test6()` |
| **7** | Subfolder path resolution: nested directories correctly resolved and committed | `tests/test_fixes.cpp` | `test7()` |
| **8** | Poisoned bundle safety: `.git` internals ignored, corrupt bundles rejected | `tests/test_fixes.cpp` | `test8()` |
| **9** | Reflog escaping & JSON roundtrip: reflog entries with special chars preserved | `tests/test_fixes.cpp` | `test9()` |
| **10** | Multi-line commit message preservation: multi-line messages preserved byte-for-byte | `tests/test_fixes.cpp` | `test10()` |
| **11** | Merge conflict markers & abort restore: markers inserted, abort restores cleanly | `tests/test_fixes.cpp` | `test11()` |
| **12** | Commit hash without parent2 preservation: single-parent commit hashes intact | `tests/test_fixes.cpp` | `test12()` |
| **13** | `.gitignore` repo root only: ignore patterns anchored to repository root | `tests/test_core_fixes.cpp` | `test_fix13_14()` |
| **14** | `.gitignore` pattern caching: avoid repeated glob compiling during file walk | `tests/test_core_fixes.cpp` | `test_fix13_14()` |
| **15** | `exportToGit` no stray 'nul' file: Windows device check skips invalid null files | `tests/test_core_fixes.cpp` | `test_fix15()` |
| **16** | `importFromGit` returns 'already up to date' when hashes match | `tests/test_core_fixes.cpp` | `test_fix16()` |
| **17** | `ui`/`serve` port validation: enforces 1–65535 port range | `tests/test_cli_server.cpp` | `test_item17()` |
| **18** | Empty repo branch creation & checkout main: clean error handling on empty repo | `tests/test_cli_server.cpp` | `test_item18()` |
| **19** | P2P bundle import byte-for-byte fidelity & hash verification | `tests/test_p2p.cpp` | `test1()` |
| **20** | DAG graph lane assignment: non-overlapping lanes for concurrent branches | `tests/frontend/b2_graph_lanes.test.js` | `b2_graph_lanes.test.js` |
| **21** | Snapshot JSON contract & real branches: `/api/snapshot` returns complete state | `tests/test_cli_server.cpp` | `test_item21()` |
| **22** | New CLI commands (`branch -d`, `undo`, `revert`, `stash`, `reflog`) | `tests/test_cli_server.cpp` | `test_item22()` |
| **23** | Path validation rejects control chars and `\:*?"<>\|` | `tests/test_cli_server.cpp` | `test_backend2()` |
| **24** | P2P idempotent bundle re-import: reports already present, modifies nothing | `tests/test_p2p.cpp` | `test2()` |
| **25** | P2P tampered object rejection: refs and object store remain untouched | `tests/test_p2p.cpp` | `test3()` |
| **26** | P2P diverged branch handling: peer tip saved to remote ref, active unchanged | `tests/test_p2p.cpp` | `test4()` |
| **27** | P2P dirty working tree fast-forward refusal | `tests/test_p2p.cpp` | `test5()` |
| **28** | Malformed JSON request body handling: returns 400 Bad Request with zero crashes | `tests/test_p2p.cpp` | `test6()` |
| **29** | 6-digit pair code, lockout, host Allow/Deny, 32-byte session token, Revoke | `tests/test_p2p.cpp` | `test7()` |
| **30** | HTTP pairing over LAN interface: handshake and token validation | `tests/test_p2p.cpp` | `test_http_pairing()` |
| **31** | Branch creation validates target revision `at` | `tests/test_core_fixes.cpp` | `test_fix31()` |
| **32** | Server-side reflog endpoint (`/api/reflog` pagination and formatting) | `tests/test_cli_server.cpp` | `test_server_reflog()` |
| **33** | Bundle import path traversal rejection: directory traversal blocked | `tests/test_core_fixes.cpp` | `test_h1()` |
| **34** | HTTP parsing limits, bare-LF handling, early Content-Length 413 check | `tests/test_cli_server.cpp` | `test_a4_http_parsing()` |
| **35** | Web file API symlink refusal: never write to or read from symlinks | `tests/test_core_fixes.cpp` | `test_h2()` |
| **36** | Replacing a file with a folder of the same name | `tests/test_core_fixes.cpp` | `test_m1()` |
| **37** | CLI help flags, checkout empty repo, commit argument validation | `tests/test_cli_server.cpp` | `test_a2_cli_fixes()` |
| **38** | Frontend single toast, pairing `DECISION_IN_FLIGHT`, safe discard | `tests/frontend/a3_frontend_fixes.test.js` | `a3_frontend_fixes.test.js` |
| **39** | Async frontend rendering race condition prevention | `tests/frontend/a3_frontend_fixes.test.js` | `a3_frontend_fixes.test.js` |
| **40** | Bundle import non-commit tip verification (blobs/trees skipped with warnings) | `tests/test_core_fixes.cpp` | `test_l1()` |
| **41** | Branch name validation: control characters and invalid flags rejected | `tests/test_cli_server.cpp` | `test_l2()` |
| **42** | RefManager branch deletion path traversal prevention (`../../index`) | `tests/test_core_fixes.cpp` | `test_g1_refmgr()` |
| **43** | Staged file status markers (A/M/D) and `escapePath` formatting | `tests/test_core_fixes.cpp` | `test_l3()` |
| **44** | Pairing status polling: 404 unknown vs 200 real rejection | `tests/test_cli_server.cpp` | `test_l4()` |
| **45** | Hierarchical file tree navigation supporting nested directories | `tests/frontend/b7_script_injection.test.js` | `b7_script_injection.test.js` |
| **46** | JSON nesting depth limit & 2MB bracket nesting overflow safety | `tests/test_core_fixes.cpp` | `test_g1_json()` |
| **47** | 4KB unauthorized body cap & Slowloris header timeout protection | `tests/test_cli_server.cpp` | `test_group1_body_cap()` |
| **48** | Git export process execution without `system()` shell interpolation | `tests/test_core_fixes.cpp` | `test_g1_export()` |
| **49** | Untracked parent prefix refusal on restore/checkout | `tests/test_core_fixes.cpp` | `test_g2_prefix()` |
| **50** | Non-empty directory at tracked path refusal (prevent recursive deletion) | `tests/test_core_fixes.cpp` | `test_g2_nonempty()` |

---

## PART A: Follow-up Fixes

| Item | Description | Test Location | Test Function / Target |
|:-----|:------------|:--------------|:-----------------------|
| **A1** | Save & commit / Save & stash in unsaved-changes dialog: dirty buffers saved and staged before commit/stash | `tests/frontend/a1_unsaved_dialog.test.js` | `a1_unsaved_dialog.test.js` |
| **A2** | CLI polish: `minigit help`/`-h`/`--help` exit 0, empty checkout exits 1, commit rejects unknown args, multiple `-m` joined with blank line, `log` indents multi-line messages, `log --stat` scales only >20 lines, `serve`/`ui` bad flag prints usage and exits 1 | `tests/test_cli_server.cpp` | `test_a2_cli_fixes()` |
| **A3** | Frontend fixes: single toast for LAN pull/bundle upload, `DECISION_IN_FLIGHT` flag during pairing decide, `apiPost` non-JSON handling, discard restores buffer on failure | `tests/frontend/a3_frontend_fixes.test.js` | `a3_frontend_fixes.test.js` |
| **A4** | HTTP parsing: bare `\n\n` headers parsed without timeout, Content-Length checked before body read (413), headers >8KB return 431 without connection reset | `tests/test_cli_server.cpp` | `test_a4_http_parsing()` |
| **A5** | Connection flooding: per-IP limit of 4 connections (503 for that IP), 5s header deadline, loopback worker slots reserved so host UI is never locked out | `tests/test_cli_server.cpp` | `test_a5_connection_flooding()` |
| **A6** | Pairing security: host-side alert for pairing failures, slower global brute-force ceiling (<0.1% chance over 5 min), per-IP exponential backoff with "Try again in N seconds" message | `tests/test_cli_server.cpp` | `test_a6_pairing_backoff()` |
| **A7** | Build portability: `-pthread` in CXXFLAGS/LDFLAGS on MinGW/Linux, frontend chunking under 60KB in `FrontendTemplate.h` to eliminate `-Woverlength-strings` warnings | `tools/embed_frontend.py` | `embed_frontend.py` & `scripts/verify.sh` |

---

## PART B: Verification & Hardening

| Item | Description | Test Location | Test Function / Target |
|:-----|:------------|:--------------|:-----------------------|
| **B1** | Time Travel and dashboard order: topological and chronological commit ordering reachable from current branch with parent-order tie-breaking | `tests/frontend/b1_topological_sort.test.js` | `b1_topological_sort.test.js` |
| **B2** | Graph lanes: DAG lane assignment with 8+ concurrent branches with forks and merges, zero lane collisions over overlapping rows | `tests/frontend/b2_graph_lanes.test.js` | `b2_graph_lanes.test.js` |
| **B3** | Poisoned bundle: bundles with bad header, invalid size, missing objects, or corrupt object rejected with MiniGitException; object store and refs intact | `tests/test_p2p.cpp` | `test_b3_poisoned_bundle()` |
| **B4** | Snapshot & ETag: `lanUrl` present in LAN mode, comma-separated `If-None-Match` tags return 304, CORS headers present on 304 | `tests/test_cli_server.cpp` | `test_b4_snapshot_lan_etag()` |
| **B5** | Port search boundary: starting at 65530 with ports occupied never wraps past 65535, raises formatted error | `tests/test_cli_server.cpp` | `test_b5_port_search_boundary()` |
| **B6** | Concurrency: 20 parallel API calls modifying repository simultaneously without crashes; repository passes full log walk and object store readback | `tests/test_cli_server.cpp` | `test_b6_parallel_concurrency()` |
| **B7** | Script injection prevention: file names, branch names, and commit messages containing `<img src=x onerror=alert(1)>` rendered strictly as plain text | `tests/frontend/b7_script_injection.test.js` | `b7_script_injection.test.js` |

---

## Running the Verification Suite

To run all tests and verify full repository integrity from a clean state:

```bash
# On Linux / macOS / MSYS2:
./scripts/verify.sh

# On Windows Command Prompt / PowerShell:
scripts\verify.bat
```
