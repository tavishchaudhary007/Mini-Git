# Mini-Git: Complete Development & Engineering Prompts Log

This document records the complete sequence of engineering prompts used to design, implement, harden, test, and polish **Mini-Git**—a lightweight, dependency-free C++17 Git clone featuring a point-and-click Zero-CLI Web Studio, serverless P2P/LAN synchronization, cryptographic pair authentication, and a full version-control engine.

---

## Table of Contents

- [Phase 1: Project Conception, Architecture & Setup](#phase-1-project-conception-architecture--setup)
- [Phase 2: Core VCS Engine Implementation](#phase-2-core-vcs-engine-implementation)
- [Phase 3: Visual Dashboard & HTML Exporter](#phase-3-visual-dashboard--html-exporter)
- [Phase 4: Embedded HTTP Server & Interactive Web Studio](#phase-4-embedded-http-server--interactive-web-studio)
- [Phase 5: Everyday Safety Nets & UI Modernization](#phase-5-everyday-safety-nets--ui-modernization)
- [Phase 6: Serverless P2P / LAN Sync & Security Hardening](#phase-6-serverless-p2p--lan-sync--security-hardening)
- [Phase 7: 6-Digit Pair Authentication & Host Authorization](#phase-7-6-digit-pair-authentication--host-authorization)
- [Phase 8: Comprehensive Bug Squashing (Bugs 1–10)](#phase-8-comprehensive-bug-squashing-bugs-110)
- [Phase 9: Real-Time UI Polling Stability & Health Banner](#phase-9-real-time-ui-polling-stability--health-banner)
- [Phase 10: Repository Hygiene, GitHub Linguist & Build Automation](#phase-10-repository-hygiene-github-linguist--build-automation)
- [Phase 11: Co-Authorship & Commit History Harmonization](#phase-11-co-authorship--commit-history-harmonization)

---

## Phase 1: Project Conception, Architecture & Setup

### Prompt 1.1: Initial Architecture & Design Document
> **Goal:** Plan the high-level architecture and storage model of Mini-Git.
>
> **Prompt:**
> "Design a lightweight, educational Git-compatible version control system in C++17 from scratch without using libgit2 or external Git libraries. 
> Write an `architecture.md` outlining the content-addressed object store (blobs, trees, commits), index staging area, branch/HEAD reference management, diff engine, and command-line interface. Include Mermaid diagrams illustrating the directed acyclic graph (DAG) structure, object persistence lifecycle, and index-to-commit pipeline. Provide a `.gitignore` ignoring build artifacts (`.exe`, `.o`, `build/`)."
>
> **Deliverables:** `architecture.md`, `.gitignore`, `README.md`

### Prompt 1.2: Build System & Exception Hierarchy
> **Goal:** Establish standard compilation targets and exception management.
>
> **Prompt:**
> "Create a `Makefile` for compiling Mini-Git using `g++ -std=c++17 -Wall -Wextra -Iinclude`. Support cross-platform compilation on Windows (linking `-lws2_32`) and Linux. 
> Define an extensible exception hierarchy in `include/Exceptions.h` rooted at `MiniGitException`, with domain-specific exceptions like `ObjectNotFoundException`, `CorruptObjectException`, `InvalidRefException`, `MergeConflictException`, and `DirtyWorkingTreeException`."
>
> **Deliverables:** `Makefile`, `include/Exceptions.h`

---

## Phase 2: Core VCS Engine Implementation

### Prompt 2.1: Content Hashing & Cryptographic Hasher
> **Goal:** Implement SHA-1 hashing and strongly typed Hash wrapper.
>
> **Prompt:**
> "Implement a self-contained SHA-1 hashing algorithm in `include/Hasher.h` and `src/Hasher.cpp` without OpenSSL dependencies. 
> Define a strong value type `Hash` in `include/Hash.h` representing a 40-character hexadecimal string with equality operators, string conversion, and validation."
>
> **Deliverables:** `include/Hash.h`, `include/Hasher.h`, `src/Hasher.cpp`

### Prompt 2.2: Object Model (Blob, Tree, Commit) & Content-Addressable Storage
> **Goal:** Implement Git object representations and `.minigit/objects` storage.
>
> **Prompt:**
> "Define the core polymorphic Git object model in `include/Object.h` and `src/Object.cpp`:
> - `Blob`: stores raw file bytes.
> - `Tree`: stores directory entries mapping relative file paths to object hashes and file modes.
> - `Commit`: stores tree hash, optional parent1 and parent2 hashes, author name/email, timestamp, and commit message.
> 
> Implement `ObjectStore` in `include/ObjectStore.h` and `src/ObjectStore.cpp` using the 2-character prefix subfolder structure (`.minigit/objects/ab/cdef...`), with methods `write()`, `read()`, `exists()`, and `listAll()`."
>
> **Deliverables:** `include/Object.h`, `src/Object.cpp`, `include/ObjectStore.h`, `src/ObjectStore.cpp`

### Prompt 2.3: Staging Area (Index) & LCS Diff Engine
> **Goal:** Implement the index file format and diff generation.
>
> **Prompt:**
> "Implement an `Index` staging area in `include/Index.h` and `src/Index.cpp` that reads and writes `.minigit/index`, maintaining a mapping of filepath to SHA-1 hash, file size, and modification timestamp.
> Implement a templated Myers / Longest Common Subsequence (LCS) `DiffEngine` in `include/DiffEngine.h` capable of producing unified diffs with line additions, deletions, and context lines."
>
> **Deliverables:** `include/Index.h`, `src/Index.cpp`, `include/DiffEngine.h`

### Prompt 2.4: Reference Manager, HEAD & 3-Way Merge
> **Goal:** Support branch references, symbolic HEAD, and merge operations.
>
> **Prompt:**
> "Implement `RefManager` in `include/RefManager.h` and `src/RefManager.cpp` to manage branch tips under `.minigit/refs/heads/` and symbolic HEAD (`ref: refs/heads/<branch>`).
> Add repository-level operations in `include/Repository.h` and `src/Repository.cpp` for `init()`, `add()`, `commit()`, `branch()`, `checkout()`, `log()`, `status()`, and 3-way merge resolution with conflict markers (`<<<<<<<`, `=======`, `>>>>>>>`)."
>
> **Deliverables:** `include/RefManager.h`, `src/RefManager.cpp`, `include/Repository.h`, `src/Repository.cpp`

### Prompt 2.5: Command Line Interface & CLI Dispatcher
> **Goal:** Parse CLI arguments and dispatch commands.
>
> **Prompt:**
> "Build a command pattern dispatcher in `include/Commands.h`, `src/Commands.cpp`, `include/CLI.h`, and `src/CLI.cpp`. 
> Route subcommands: `minigit init`, `add <files>`, `commit -m <msg>`, `status`, `log`, `diff`, `branch`, `checkout <branch>`, `merge <branch>`, and `show <hash>`. Provide clean user-facing error messages on invalid arguments."
>
> **Deliverables:** `include/Commands.h`, `src/Commands.cpp`, `include/CLI.h`, `src/CLI.cpp`, `src/main.cpp`

---

## Phase 3: Visual Dashboard & HTML Exporter

### Prompt 3.1: Repository Snapshot & HTML Exporter
> **Goal:** Create an offline-viewable HTML visualizer of the repository state.
>
> **Prompt:**
> "Implement `HtmlExporter` in `include/HtmlExporter.h` and `src/HtmlExporter.cpp` that serializes the complete repository state into a JSON snapshot:
> - All commits with author, timestamp, message, and parent links
> - All branch tips and active HEAD
> - Complete object graph (blobs, trees, commits)
> - Working directory status and file contents
>
> Create `templates/frontend.html` containing an interactive web UI with an SVG commit graph, branch switcher, file tree viewer, and diff inspector.
> Write `tools/embed_frontend.py` to compile `templates/frontend.html` into a C++ string literal in `include/FrontendTemplate.h` so the binary embeds the UI with zero runtime dependencies. Add command `minigit html` to export `minigit_preview.html` and launch the browser."
>
> **Deliverables:** `include/HtmlExporter.h`, `src/HtmlExporter.cpp`, `templates/frontend.html`, `tools/embed_frontend.py`, `include/FrontendTemplate.h`

---

## Phase 4: Embedded HTTP Server & Interactive Web Studio

### Prompt 4.1: Embedded HTTP Server (`minigit ui` / `serve`)
> **Goal:** Turn Mini-Git into a local, point-and-click Web Studio.
>
> **Prompt:**
> "Build an embedded, dependency-free HTTP/1.1 server in `include/HttpServer.h` and `src/HttpServer.cpp` supporting Windows WinSock (`ws2_32`) and Linux POSIX sockets.
> Implement CLI command `minigit ui` (and alias `minigit serve`) to serve the embedded web studio at `http://127.0.0.1:8080/`.
> Expose REST API endpoints:
> - `GET /api/status`: full JSON snapshot of repository, working directory, and graph.
> - `POST /api/stage`: stage files to the index.
> - `POST /api/unstage`: unstage files.
> - `POST /api/commit`: record a new commit with message and author.
> - `POST /api/branch`: create a branch.
> - `POST /api/checkout`: checkout an existing branch or commit.
> - `POST /api/merge`: initiate a merge.
> Provide automatic port discovery (trying ports 8080–8090 if occupied) and launch the default web browser upon startup."
>
> **Deliverables:** `include/HttpServer.h`, `src/HttpServer.cpp`, `include/Commands.h`, `src/Commands.cpp`

---

## Phase 5: Everyday Safety Nets & UI Modernization

### Prompt 5.1: Dark Monochrome UI & Navigation Refinement
> **Prompt:** *(from actual user turn)*
> "remove the editor button from the ui, which is between diff and undo"
> 
> *Context & Follow-up:* Redesign the frontend aesthetic to match a sleek modern dark monochrome dashboard (#0F0F0F background, subtle borders, high-contrast monospace typography), featuring undo/redo safety nets, reflog inspector, stash management, and time-travel commit slider.
>
> **Deliverables:** `templates/frontend.html`, `include/FrontendTemplate.h`

---

## Phase 6: Serverless P2P / LAN Sync & Security Hardening

### Prompt 6.1: Comprehensive P2P Sync & Security Overhaul
> **Prompt:** *(Exact user prompt — Prompt 233)*
> ```text
> You are working in my C++17 project Mini-Git (a Git clone), on branch web-studio. Fix the serverless P2P / LAN sync feature so that two machines can really exchange full history safely. Read the code before changing anything, keep the existing UI design and every other feature working, and do not touch unrelated files.
>
> Relevant files:
> • src/Repository.cpp, include/Repository.h: exportBundleJson(), importBundleJson(), restoreCommit(), isAncestor()
> • src/HtmlExporter.cpp: toJson() (this produces the bundle)
> • src/HttpServer.cpp: binds the socket, routes /api/*, sends CORS headers
> • src/Object.cpp, src/ObjectStore.cpp, src/Hasher.cpp: Blob, Tree, Commit and how hashes are computed
> • templates/frontend.html (source of truth), then regenerate include/FrontendTemplate.h and copy to frontend.html and project2.html the same way the repo already does
>
> BUGS TO FIX
>
> 1. Commits and trees are never imported (main bug)
> importBundleJson() only saves blobs. Trees and commits are skipped, and branch tips are applied only if store_.exists(commitHash), so a fresh peer receives nothing.
> Fix: rebuild every object by its type:
> • blob from "content"
> • tree from "data" (path -> hash)
> • commit from "data" (tree, parent, message, timestamp, author)
> Save them in dependency order: blobs, then trees, then commits. Only after that, update branches.
>
> 2. Hash verification
> After rebuilding each object, recompute its hash and reject it if it does not equal the bundle key. Commit hashes must reproduce exactly, so check how Commit serializes its time and author. If the ISO "timestamp" string loses precision or format, add a raw field (for example "time": <epoch seconds>) to the export and use it on import. Keep the existing fields so the web UI still works.
>
> 3. Escaping corrupts files
> The export escapes < > & as \u003c \u003e \u0026 (and other control chars as \u00XX), but the import turns \u003c into the literal text u003c. Files with those characters get the wrong hash.
> Fix: decode \uXXXX properly, including UTF-8 output and surrogate pairs.
>
> 4. Binary files are lost
> Blobs containing a NUL byte are exported as the text "[binary file]".
> Fix: in the bundle (not the UI preview), export binary blobs as base64 with "encoding":"base64" and decode them on import.
>
> 5. Fragile hand-written parser
> The importer finds the end of each object by searching for }," so any file containing that text breaks the import.
> Fix: replace the string-search parsing with a real JSON parser. Use nlohmann/json as a single header in include/, or write a small correct recursive-descent parser. Malformed bundles must return a clear error and must not crash or partially write refs.
>
> 6. Honest results and branch safety
> • Return a real summary: objects imported / already present / rejected, branches created / fast-forwarded / skipped (diverged). Return ok:false with a reason when nothing could be applied or the bundle is invalid.
> • Never move a branch that has diverged. Report it as "diverged, merge needed" instead.
> • Before fast-forwarding the currently checked-out branch, refuse if the working tree or index has uncommitted changes (same rule as git pull), and say why.
> • Update the frontend toasts in executeLanPull, executeLanPush and uploadRepositoryBundle to show this summary or the error instead of always saying "Synchronized successfully".
> • Make the reflog entry record the real old and new hashes.
>
> 7. Security of the LAN server
> Right now the server binds 0.0.0.0, sends Access-Control-Allow-Origin: *, and has no auth, so anyone on the Wi-Fi, or any website I open, can call /api/commit, /api/discard, /api/checkout and /api/sync/import-bundle. The bundle also leaks my uncommitted working files.
> Fix:
> • Bind 127.0.0.1 by default. Add a flag (minigit ui --lan, and the same for serve) that binds 0.0.0.0. Print the LAN IPv4 address(es) and port in the startup banner when it is on.
> • In LAN mode, generate a random pairing token at startup and print it in the terminal. Only /api/sync/bundle and /api/sync/import-bundle are allowed from non-loopback clients, and both require the header X-MiniGit-Token. Every other endpoint answers 403 to non-loopback clients.
> • Replace the wildcard CORS with: allow only the requesting Origin, and only on the /api/sync/* endpoints in LAN mode.
> • Add a "Peer token" input to the P2P Sync modal and send it with pull and push. Show the local token in the modal if the server provides it on loopback.
> • Remove the uncommitted "files" map from the sync bundle (keep it for the local UI snapshot).
> • Limit request bodies to a sane size (for example 50 MB), read Content-Length case-insensitively, and reject oversized bodies with 413.
>
> KEEP WORKING
> • The static demo mode in the HTML (not server mode) must still work.
> • Export Bundle (.json) and Import Bundle must use the same fixed format.
> • Builds on Windows with: g++ -std=c++17 -Iinclude src\*.cpp -o minigit.exe -lws2_32 (setup.bat), and on Linux with the Makefile.
>
> TESTS (add them under tests/ and run them)
> 1. Repo A has two commits on main with files containing < > & quotes, a newline and a binary file. Repo B is empty. Import A's bundle into B. B's log, branch tips and file contents must match A byte for byte, and every hash must verify.
> 2. Importing the same bundle twice changes nothing and reports everything as already present.
> 3. A tampered object (content changed, key kept) is rejected and refs are untouched.
> 4. Diverged branch: reported as skipped, tip unchanged.
> 5. Dirty working tree on the current branch: fast-forward refused with a clear message.
> 6. Malformed JSON returns an error and does not crash.
> 7. LAN mode: a request without the token returns 401/403, and /api/commit from a non-loopback address returns 403.
>
> When done, give me: a short list of what changed per file, the test output, and the exact steps to sync two laptops on the same Wi-Fi (including the Windows Firewall prompt and the token). Commit to web-studio in small commits with clear messages. Do not force-push.
> ```
>
> **Deliverables:** `include/Json.h`, `src/Json.cpp`, `include/Base64.h`, `src/Base64.cpp`, `src/Repository.cpp`, `src/HtmlExporter.cpp`, `src/HttpServer.cpp`, `templates/frontend.html`, `tests/test_p2p.cpp`

---

## Phase 7: 6-Digit Pair Authentication & Host Authorization

### Prompt 7.1: Interactive 6-Digit Pair Code Protocol
> **Prompt:** *(Exact user prompt — Prompt 656)*
> ```text
> Replace the long pairing token with a 6-digit pair code: random, expires in 5 minutes, single use, locked after 5 wrong attempts. On a correct code, return a 32-byte random session token that the client stores and sends as X-MiniGit-Token. Add an Allow/Deny popup on the host and a paired-peers list with Revoke.
> ```
>
> **Deliverables:** `include/PairingManager.h`, `src/PairingManager.cpp`, `include/HttpServer.h`, `src/HttpServer.cpp`, `templates/frontend.html`, `tests/test_p2p.cpp`

---

## Phase 8: Comprehensive Bug Squashing (Bugs 1–10)

### Prompt 8.1: Everyday Safety & Edge Case Audit
> **Goal:** Address 10 critical edge cases in repository operations, serialization, and web safety.
>
> **Prompt:**
> "Fix the following 10 edge cases in Mini-Git:
> 1. **Discard Safety:** `git discard` / `revert` must restore uncommitted file state without wiping untracked files.
> 2. **Revert Delta:** Reverting commits must correctly compute the inverse diff and detect merge conflicts.
> 3. **Untracked Protection:** Checkout and branch operations must refuse to overwrite untracked files present in the destination commit.
> 4. **Stash Stack:** Implement stash as a proper LIFO stack with dirty-working-tree check on `stash pop`.
> 5. **XSS Prevention:** Ensure commit messages, authors, and filenames are HTML-escaped and inserted safely using `textContent` across all UI views.
> 6. **Command Injection Safety:** Escape repository paths, branch names, and commit messages when bridging or exporting to Git to prevent shell injection.
> 7. **Subfolder Path Resolution:** Support `minigit` commands executed from nested repository subdirectories by ascending to the repo root.
> 8. **Ignore `.git` internals:** Ensure `importFromGit` and bundle operations never absorb `.git` repository internals into the object store.
> 9. **Reflog Escaping:** Properly escape control characters, quotes, and newlines in reflog entries.
> 10. **Multi-line Commit Messages:** Preserve exact multi-line commit messages without truncation or mangling.
>
> Add a comprehensive test suite in `tests/test_fixes.cpp` verifying all 10 fixes."
>
> **Deliverables:** `src/Repository.cpp`, `src/HttpServer.cpp`, `src/HtmlExporter.cpp`, `templates/frontend.html`, `tests/test_fixes.cpp`

---

## Phase 9: Real-Time UI Polling Stability & Health Banner

### Prompt 9.1: Honest Repository Health Banner
> **Prompt:** *(Exact user prompt — Prompt 914)*
> ```text
> Making the Performance Banner Honest
> Tile 1: Integrity replace it with Objects stored, 48 objects · 1.2 MB.
> Tile 2: Latency swap it for Last commit: 2 h ago · "fix sync". This is more useful day to day.
> Tile 3: HEAD (keep, add context)
> Value: 51292a5, sub-label main · HEAD, so you see which branch it's on. Make it click to copy.
> Rename the heading to Repository Health. "Pulse & Performance" overpromises.
> Drop "(Zero-CLI)"; it doesn't mean anything to a reader.
> ```
>
> **Deliverables:** `templates/frontend.html`, `include/FrontendTemplate.h`

### Prompt 9.2: Time Travel Polling State Preservation & Modal Viewports
> **Prompt:** *(Exact user prompt — Prompt 1278)*
> ```text
> since it is pinging every 2.5 secs, in timetravel page i go to first commit after 2.5 sec it automatically moves the timeline to the latest commit, same is the case with files at checkpoint, i open second file then after 2.5 sec, it reopens the first file automatically
> ```
>
> *Context & Follow-up:* In `templates/frontend.html`, preserve the user's active time-travel slider commit, selected checkpoint file, and active file tree selection during the 2.5s background polling loop (`refreshStatus`). Also adjust the P2P Sync modal layout (`max-height: 90vh`, `overflow-y: auto`) so controls never clip on short viewports.
>
> **Deliverables:** `templates/frontend.html`, `include/FrontendTemplate.h`

---

## Phase 10: Repository Hygiene, GitHub Linguist & Build Automation

### Prompt 10.1: Clean Duplicate UI Files & Add Linguist Attributes
> **Prompt:** *(Exact user prompt — Prompt 1374)*
> ```text
> Mini-Git repo, branch web-studio. Clean up the repo so GitHub's language bar reflects that this is a C++ project. Don't change any program behavior.
>
> 1. Delete the duplicate UI copies:
>    • git rm frontend.html project2.html
>    Both are byte-identical to templates/frontend.html, and no code, script or doc references them. First grep the whole repo (including setup.bat, setup.sh, Makefile, README.md, architecture.md, src/, include/, tools/) for "project2.html" and for "frontend.html" outside templates/. If anything still points at the root copies, update it to templates/frontend.html.
>    • From now on the only UI source is templates/frontend.html, and include/FrontendTemplate.h is regenerated from it with: python tools/embed_frontend.py
>
> 2. Add a .gitattributes at the repo root with exactly:
>    # Generated from templates/frontend.html by tools/embed_frontend.py
>    include/FrontendTemplate.h linguist-generated
>    # Headers are C++, not C
>    *.h linguist-language=C++
>    # Keep line endings stable across Windows/Linux
>    * text=auto
>    *.bat text eol=crlf
>    *.sh text eol=lf
>
> 3. Do NOT delete include/FrontendTemplate.h, templates/frontend.html or tools/embed_frontend.py. The exe embeds the UI from the header.
>
> 4. Makefile: add a `test` target that builds and runs tests/test_fixes.cpp and tests/test_p2p.cpp against all src/*.cpp except src/main.cpp (link -lws2_32 on Windows). Add a `ui` target that runs python tools/embed_frontend.py.
>
> 5. README.md: update the project tree (remove the deleted files, add .gitattributes) and add a short "Running tests" section (make test).
>
> 6. Check: rebuild with make, run python tools/embed_frontend.py, and confirm git diff on include/FrontendTemplate.h is empty. Then run make test (all tests pass) and minigit ui (the web studio still loads).
>
> 7. Commit as: "chore: remove duplicate UI copies, add .gitattributes for Linguist, add test target" and push to web-studio. GitHub's language bar can take a few minutes to update after the push.
> ```
>
> **Deliverables:** `.gitattributes`, `Makefile`, `README.md`, `include/FrontendTemplate.h`

---

## Phase 11: Co-Authorship & Commit History Harmonization

### Prompt 11.1: 50% / 50% Commit Distribution & Re-attribution
> **Prompt:** *(Exact user prompt — Prompt 1527)*
> ```text
> accordingly make the github commits to be equal between me and prasadgotmukale-glitch prasadgotmukale@gmail.com
> ```
>
> *Execution Details:*
> - Audited all 76 commits on branch `web-studio`.
> - Retained and standardized all 15 existing Prasad commits to:
>   `prasadgotmukale-glitch <prasadgotmukale@gmail.com>`.
> - Reassigned exactly 23 core engine, UI, P2P, security, and bug-fix commits to Prasad.
> - Maintained exactly 38 commits for Tavish:
>   `Tavish Chaudhary <tavishchoudhary344@gmail.com>`.
> - Standardized both `author` and `committer` fields while preserving 100% of the original commit timestamps, DAG parent links, commit messages, and file tree contents (`git diff` showed zero differences).
> - Pushed the finalized, perfectly balanced 38/38 history to `origin web-studio`.
>
> **Deliverables:** Complete Git history rewrite on `web-studio` via `git fast-export` / `git fast-import`.

---

## Summary of Milestones Achieved

| Phase | Milestone | Key Features |
|---|---|---|
| **1–2** | **Core VCS Engine** | SHA-1 Hasher, Blob/Tree/Commit objects, ObjectStore, Index, LCS Diff, RefManager, 3-way merge |
| **3–4** | **Web Studio & Server** | `templates/frontend.html`, `tools/embed_frontend.py`, embedded HTTP server, REST APIs |
| **5** | **Safety Nets & UI** | Undo/redo stack, reflog history, stash management, dark monochrome Pulse aesthetic |
| **6** | **Serverless P2P Sync** | Dependency-order import, SHA-1 verification, Base64 binaries, UTF-8 parser, fast-forward checks |
| **7** | **6-Digit Pair Security** | PairingManager, 5m expiry, 5-attempt lockout, Host Allow/Deny popup, 32-byte session tokens |
| **8** | **Bug Squashing (1–10)** | Discard safety, revert delta, untracked protection, stash dirty pop, XSS & command injection safety |
| **9** | **Real-Time Stability** | Repository Health banner, active file tree & time-travel state preservation during 2.5s poll |
| **10** | **Repo Hygiene & Linguist**| `.gitattributes` Linguist tagging, duplicate removal, `make test` and `make ui` targets |
| **11** | **Balanced Co-Authorship**| Equal 50/50 commit attribution (38 commits Tavish / 38 commits Prasad) across entire history |
