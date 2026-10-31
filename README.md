# Mini-Git

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A small version control system written in C++17 for CS3104 (OOP Mini-Project #21). It hashes content with a hand-written SHA-1, stores blobs, trees and commits in a `.minigit/` folder like real Git, and supports staging, commits, history, diff, branches, checkout and merging (fast-forward and three-way with conflict markers). A built-in local web server, the **Web Studio**, adds a point-and-click interface with a file editor, time travel, stash, undo, reflog, LAN peer-to-peer sync and a Git import/export bridge.

Authors: Tavish Chaudhary (251159), Prasad Gotmukale (251124)

---

## Table of Contents
1. [Features](#features)
2. [Requirements](#requirements)
3. [Quick start (one command)](#quick-start-one-command)
4. [Build](#build)
5. [Running tests](#running-tests)
6. [Tutorial: your first repository](#tutorial-your-first-repository)
7. [Tutorial: branches and merging](#tutorial-branches-and-merging)
8. [Ignoring files: `.minigitignore`](#ignoring-files-minigitignore)
9. [Visual report: `minigit html`](#visual-report-minigit-html)
10. [Web Studio: `minigit ui` / `minigit serve`](#web-studio-minigit-ui--minigit-serve)
11. [LAN pairing and P2P sync](#lan-pairing-and-p2p-sync)
12. [Web security model](#web-security-model)
13. [Git Bridge](#git-bridge)
14. [Command reference](#command-reference)
15. [Project structure](#project-structure)
16. [Design and diagrams](#design-and-diagrams)
17. [How it works](#how-it-works)
18. [Error messages](#error-messages)
19. [Troubleshooting](#troubleshooting)
20. [Limitations](#limitations)
21. [License](#license)

---

## Features

**Command line (16 commands)**

| Command | Purpose |
|---|---|
| `init` | Create a repository |
| `add` | Stage files, folders, or deletions |
| `commit` | Save a snapshot with a message |
| `log` | Show history (`--oneline`, `--stat`) |
| `status` | Staged, modified, deleted and untracked files |
| `diff` | Line-by-line changes (LCS) since the last `add` |
| `branch` | List, create, or delete branches (`-d`) |
| `checkout` | Switch branch, or go to a commit (detached HEAD) |
| `merge` | Fast-forward or three-way merge; `--abort` cancels a conflicted merge |
| `undo` | Undo the last commit, preserving working directory changes |
| `revert` | Create a new commit that reverts a previous commit |
| `stash` | Save, pop, or list uncommitted changes |
| `reflog` | Show chronological history of HEAD updates and actions |
| `html` | Write a read-only visual report and open it |
| `ui` | Start the interactive Web Studio |
| `serve` | Same as `ui` |

**Web Studio** (through the local REST API and UI): Fetch API (native async `fetch()`, zero external JS dependencies like jQuery), edit and save files in the built-in editor, stage/unstage, discard changes, delete files from tree and editor, create/delete branches or branch at any commit, revert a commit, undo the last commit, merge with interactive conflict resolution banner (Abort merge / Mark resolved), unsaved-changes protection, caret preservation on auto-refresh, read-only binary file protection, stash save/pop/list, reflog view and restore, time-travel through commits, repository health banner, lane-based commit DAG and dashboard mini graph, detached-HEAD badge, ETag/304 conditional polling, LAN pairing with CSPRNG codes and security hardening, P2P pull/push with button loading states, and Git export/import bridge.

## Requirements

- A C++17 compiler: **g++ 9 or newer** (or recent clang / MSVC)
- `make` (optional; you can compile with one g++ command)
- **Python 3**, only for the `make` targets that use it (`build`, `ui`, `clean`) and `tools/embed_frontend.py`
- **Git**, only for the Git Bridge feature
- **Windows:** the Winsock library (`-lws2_32`), added automatically by the Makefile

Check your compiler:
```
g++ --version
```

**Windows:** install [MSYS2](https://www.msys2.org/), then in the MSYS2 terminal run:
```
pacman -S mingw-w64-ucrt-x86_64-gcc make python
```

## Quick start (one command)

From the project root (next to `src/` and `include/`):

| OS | Command |
|---|---|
| Windows | `setup.bat` (or double-click it) |
| Linux / macOS / Git Bash | `bash setup.sh` |

It builds the executable and, on Windows, adds Mini-Git to your User PATH so `minigit` works in any folder. Manual steps are below.

## Build

**With make (any OS)**
```bash
make
```

**Linux / macOS, no make**
```bash
g++ -std=c++17 -Iinclude src/*.cpp -o minigit
```

**Windows, no make** (the socket library is required for the Web Studio)
```
g++ -std=c++17 -Iinclude src/*.cpp -o minigit.exe -lws2_32
```

You now have `minigit` (`minigit.exe` on Windows).

## Running tests

```bash
make test
```
Builds and runs four automated test suites (`TEST_TARGETS` in `Makefile`):
- `tests/test_fixes.cpp` (12 tests): Regression, path safety, and edge-case tests (discard safety, revert conflict detection, untracked overwrite protection, stash dirty pop refusal, XSS prevention & JSON escaping, Git export injection safety, subfolder resolution, `.git` internals ignore, reflog escaping, multi-line commit preservation, merge conflict markers and abort restore, commit hash preservation without parent2).
- `tests/test_p2p.cpp` (8 tests): P2P sync and network security tests (full bundle import with Base64 binaries, idempotent re-import, tampered object rejection with untouched refs, diverged branch preservation as `peer-<branch>`, dirty working tree fast-forward refusal, malformed JSON resilience, 6-digit pair code lifecycle and token authentication, and end-to-end 3-way merge with path boundary checks).
- `tests/test_cli_server.cpp`: CLI interface and Web Studio HTTP server tests (readable local date in `log`, `ui`/`serve` `--port` 1–65535 validation, empty repo branch/checkout handling, `status` merge state reporting with unmerged paths, Snapshot JSON contract, CLI commands (`branch -d`, `undo`, `revert`, `stash`, `reflog`), path validation rejecting illegal characters, CLI polish, Git export identity checks, ETag/304 conditional polling, branch creation at commit, and brute-force pairing security enforcement with 429 and 403 checks).
- `tests/test_core_fixes.cpp` (10 tests): Core repository logic and safety fixes (merge conflict tracking and commit refusal, operation locking during merge, Snapshot struct fields, non-destructive `stashPop()`, atomic `revert()`, empty-tree commit on file deletion, `.minigitignore` root scoping and caching, Git export clean teardown, Git import idempotency, and `createBranch` revision validation).

Scripted end-to-end demo (init, add, commit, branch, checkout, merge, error cases) in a temporary folder:
```bash
bash tests/demo.sh
```
On Windows use Git Bash or the MSYS2 terminal.

---

## Tutorial: your first repository

> Work in a **separate test folder**, not inside the source folder, so your source files are not staged.

### Step 0: set up a test folder

**Windows (PowerShell or cmd)**
```powershell
mkdir test
cd test
```
If you built manually without `setup.bat`, copy `minigit.exe` into the folder or add the build folder to your PATH.

**Linux / macOS**
```bash
mkdir ~/test && cd ~/test
export PATH=$PATH:/path/to/minigit/folder
```

### Step 1: initialize
```
minigit init
```
Creates `.minigit/` with `objects/`, `refs/heads/` and `HEAD`.

### Step 2: create a file and check status
```
echo hello > a.txt
minigit status
```
```
On branch main

Untracked files:
  a.txt
```

### Step 3: stage and commit
```
minigit add a.txt
minigit commit -m "first commit"
```
```
[main 3fa21c9] first commit
```
(Your hash will differ.)

### Step 4: change the file and see the diff
Edit `a.txt` (add a second line), then:
```
minigit status
minigit diff a.txt
```
`status` lists `a.txt` under **Changes not staged for commit**. `diff` shows removed lines as `-N:` and added lines as `+N:`.

### Step 5: stage and commit again
```
minigit add a.txt
minigit commit -m "second commit"
```

### Step 6: view history
```
minigit log
minigit log --oneline
minigit log --stat
```
`--stat` adds a per-file summary of insertions and deletions. In the full `log`, `Date:` is printed as a formatted local date and time (`%a %b %d %H:%M:%S %Y %z`).

### Adding whole folders
```
minigit add .          # everything except .minigit and ignored files
minigit add src        # one folder
```
If you delete a tracked file from disk, `minigit add <thatfile>` (or `add .`) stages the deletion.

---

## Tutorial: branches and merging

```
minigit branch feature        # create a branch at the current commit
minigit branch                # list branches (* marks the current one)
minigit checkout feature      # switch to it
```
Make a change on `feature`:
```
echo new > f.txt
minigit add f.txt
minigit commit -m "add f.txt"
```
Go back to `main`; `f.txt` disappears from the folder:
```
minigit checkout main
```

### Fast-forward merge
If `main` has no new commits of its own:
```
minigit merge feature
```
```
Fast-forward to 8c1d4e2
```
`main` now points at the same commit as `feature`.

### Three-way merge
If both branches have new commits, Mini-Git finds their common ancestor (merge base) and decides each file using base, ours and theirs:

- changed on one side only: that side's version is taken
- same change on both sides: kept
- different changes to the same file on both sides: **conflict**

Without conflicts a merge commit with two parents is created:
```
Merged feature (3-way)
```

### Resolving conflicts
```
error [merge]: Automatic merge failed; fix conflicts and then commit the result. Conflicted files: a.txt
```
Conflicted text files contain markers:
```
<<<<<<< ours
our line
=======
their line
>>>>>>> feature
```
Binary files keep our version. `.minigit/MERGE_HEAD` records the other branch while the merge is unfinished. Then either:
```
# fix the files, then
minigit add a.txt
minigit commit -m "merge feature"     # becomes a merge commit with two parents
```
or cancel and restore the last commit:
```
minigit merge --abort
```

### Visiting an old commit
```
minigit log --oneline
minigit checkout 3fa21c9      # any unique prefix of 4+ characters
```
This is a **detached HEAD**. Return with `minigit checkout main`. Checkout and merge refuse to run with uncommitted changes, or when they would overwrite an untracked file.

---

## Ignoring files: `.minigitignore`

Create `.minigitignore` in the repository root, one pattern per line:
```
# comments and blank lines are skipped
build/        # a folder anywhere in the tree
*.log         # any file with this extension
notes.txt     # an exact file name or repo-relative path
```
`.git/`, `.minigit/`, `node_modules/` and the `minigit` / `minigit.exe` executable are always ignored. Only these three pattern forms are supported (no `!` negation or `**`).

---

## Visual report: `minigit html`

```
minigit html            # writes .minigit/report.html and opens it
minigit html --no-open  # write only
```
A **read-only snapshot** of the repository at the moment you ran the command: files, source-control changes, side-by-side diffs and the commit graph. Run the command again to refresh. The file sits inside `.minigit/`, so it is never an untracked file.

Styling loads from CDNs (Tailwind, Font Awesome, Google Fonts), so it needs an internet connection; offline the data loads but looks unstyled. Opening `templates/frontend.html` directly starts a self-contained demo with sample data.

**Editing the web UI:** change `templates/frontend.html`, run `make ui` (or `python tools/embed_frontend.py`), then rebuild. This regenerates `include/FrontendTemplate.h`, which compiles the page into the executable.

---

## Web Studio: `minigit ui` / `minigit serve`

```bash
minigit ui                              # http://127.0.0.1:8080, opens the browser
minigit serve --port 9000 --no-open     # pick a port, do not open a browser
minigit ui --lan                        # also allow LAN pairing and sync
```
Starts an embedded HTTP server (raw sockets, no external libraries). If the port is busy it tries the next 29 ports. Port numbers are strictly validated (1–65535). Without `--lan` it listens on `127.0.0.1` only.

What you can do in the browser:

- **Stage / unstage / discard** files and folders (discarding an untracked file deletes it, after a confirmation)
- **Edit and save** files in the built-in editor; binary files are protected as **read-only** ("binary file, not shown")
- **Delete files** directly from the editor toolbar or file tree items (calls `POST /api/file/delete`)
- **Unsaved-changes protection**:
  - Switching branches with unsaved buffer edits opens a modal with **Save**, **Discard**, or **Cancel** (Cancel aborts the switch; Discard only clears buffers after checkout succeeds)
  - Staging individual files, staging all, or committing while edits are unsaved prompts for confirmation
- **Caret preservation**: Polling and snapshot synchronization preserve textarea caret position (`selectionStart`/`selectionEnd`), so auto-refresh never interrupts typing
- **Commit** with a message
- **Branches:** create, switch, delete, or **branch at any commit** from the graph or commit view
- **Detached-HEAD badge**: Prominently displayed in header and branch dropdown (`(detached at <hash>)`) and highlighted on the commit graph
- **Merge conflict banner**: During conflicted merges, a top banner lists all unmerged paths with links to open each file, a **Mark Resolved** button (which stages the file), and an **Abort merge** button to cleanly cancel
- **Commit actions:** check out, branch from, or **revert** a commit; **undo** the last commit (keeps your changes in the working directory)
- **Stash:** save, list and pop (the API also accepts `includeUntracked`)
- **Reflog:** see every HEAD movement and restore any earlier state
- **Time travel:** scrub through commits and view files as they were
- **Repository health** banner and DAG graph visualizer
- **LAN pairing, pull and push** with interactive **button loading states** (spinning indicators and disabled state during sync) and the **Git Bridge**

**Frontend communication:** The Web Studio uses the native **Fetch API** (`async fetch()` with JSON payloads, i.e. modern AJAX) for all client-server communication, with zero `XMLHttpRequest` and no jQuery.

**Conditional polling with ETag / 304:** The page polls `/api/snapshot` every **2.5 seconds** using standard HTTP conditional headers (`If-None-Match: <ETag>`). If no changes have occurred in the repository, the server responds with `304 Not Modified` and an empty body, avoiding redundant JSON serialization and payload transfer while ensuring terminal changes appear within seconds. Host pending pair requests are polled every 1.5 seconds.

**Commit graph DAG:** Rather than a simple column, Mini-Git features an interactive **lane-based directed acyclic graph (DAG)**:
- **Topological ordering**: Commits are sorted in topological order (newest to oldest, respecting parent-child lineages, timestamps, and HEAD priority).
- **One coloured lane per branch**: Each branch is allocated a distinct colored vertical lane (`DAG_PALETTE`) with lane interval packing to avoid overlapping lines.
- **Merge commits**: Dual curved SVG bezier edges connect merge commits to both parent lanes, accompanied by an inner dot indicator.
- **Unreachable commit toggle**: When commits become unreachable (e.g. after undo, branch deletion, or detached reset), an interactive SVG toggle (`👁 Show N unreachable commit(s)` / `✕ Hide N unreachable commit(s)`) lets you reveal them with dashed borders and connections.
- **Dashboard mini graph**: A compact horizontal mini DAG on the main dashboard gives an instant topological view of recent commit flow.

---

## LAN pairing and P2P sync

1. The host runs `minigit ui --lan`. The Pair panel shows a **6-digit code** and the LAN address.
2. The teammate opens that address and enters the code.
3. The host sees the request (the list refreshes every 1.5 s) with the requester's **IP address**, **Origin**, **claimed name**, and the entered code, then clicks **Allow & Pair**.
4. The teammate's browser receives a **session token** and stores it in `localStorage`.
5. **Pull** downloads the host's repository as a JSON bundle and imports it into the teammate's own local Mini-Git; **Push** sends the teammate's bundle to the host. Pull and Push buttons indicate progress with real-time loading spinners and disabled states.

Security rules (enforced in `HttpServer` and `PairingManager`):

- **CSPRNG codes and tokens**: Pair codes are generated via `std::random_device` with rejection sampling (100000–999999). Session tokens are 32 cryptographically random bytes (64 hexadecimal characters).
- **Code expiration & single use**: A pair code is valid for **300 seconds** (5 minutes) and is single-use: once approved, it is marked used. Pending requests expire after 120 seconds.
- **Per-client lockout**: Keyed on client IP. A client that fails **5 attempts** is locked out (`403 Forbidden`) with exponential backoff (starting at 5 seconds) until the host generates a new code. Other clients remain unaffected.
- **Global rate limit (429)**: The server tracks failed attempts across all clients in a sliding 60-second window. If **10 failed attempts** occur within 60 seconds, all submissions return `429 Too Many Requests`.
- **DoS resilience without global code invalidation**: Repeated wrong attempts from malicious clients do not invalidate the code globally for legitimate peers. The host can manually click **Generate New Code** at any time.
- **Single-use session token delivery**: Approved session tokens (32 cryptographically random bytes / 64 hex characters) are handed out once to the requesting client IP and then deleted from pending requests.
- **Non-LAN & cross-origin refusal**: If `--lan` is not passed on startup, pairing requests return `403 Forbidden`. Cross-origin pairing over loopback returns `403 Forbidden`. Non-loopback clients can only access `/api/pair`, `/api/pair/status`, `/api/sync/bundle`, and `/api/sync/import-bundle` (`403 Forbidden` for all others).
- **Token authentication**: Sync endpoints require the `X-MiniGit-Token` header containing a valid session token; missing or revoked tokens return `401 Unauthorized`. The host can view paired peers and revoke any token at any time via `/api/pair/revoke`.

On import, every object's SHA-1 is recomputed and verified; if any hash does not match, the entire bundle is rejected and references remain untouched. Objects are saved in dependency order (blobs, trees, commits). For each branch: new branches are created, behind branches are fast-forwarded (refused if active branch working tree is dirty), and **diverged branches are kept as `peer-<branch>`** so nothing is overwritten. Binary files travel as Base64.

---

## Web security model

Mini-Git's Web Studio incorporates multi-layered security controls to protect local files and repository integrity:

- **Loopback binding by default**: Without `--lan`, `HttpServer` binds strictly to `127.0.0.1`. Network connections from other machines cannot reach the socket.
- **Host-header validation (DNS rebinding defense)**: Every HTTP request validates the `Host` header against the bound port and permitted hostnames (`127.0.0.1`, `localhost`, or IPv4 literal when `--lan` is active). External host headers return `403 Forbidden`, preventing malicious websites from exploiting DNS rebinding.
- **Origin / CSRF checks**: On loopback, all state-changing requests (`POST`, `PUT`, `DELETE`) verify that the `Origin` header matches `http://127.0.0.1:<port>` or `http://localhost:<port>`. Cross-origin requests from external web pages return `403 Forbidden`. Cross-origin pairing over loopback is rejected.
- **JSON Content-Type requirement**: State-changing loopback endpoints require `Content-Type: application/json` (`400 Bad Request` otherwise), preventing browser form submission attacks (`application/x-www-form-urlencoded` or `multipart/form-data`).
- **Token-protected sync endpoints**: Non-loopback access to `/api/sync/bundle` and `/api/sync/import-bundle` requires a valid 32-byte session token via `X-MiniGit-Token` (`401 Unauthorized` otherwise). All other endpoints return `403 Forbidden` to external clients.
- **Slowloris & timeout protection**: Enforces a strict 10-second per-request wall-clock deadline; connections are served from a thread pool (`HttpServerThreadPool`) with mutex synchronization protecting `Repository` and `PairingManager`.
- **Unauthenticated endpoint body limit (4 KB)**: Inbound request bodies on unauthenticated endpoints (`/api/pair`, `/api/pair/status`) are capped at 4 KB before JSON parsing.
- **50 MB payload limit**: Inbound request bodies are capped at 50 MB via `Content-Length` enforcement (`> 50 * 1024 * 1024`). Over-sized requests immediately receive `413 Payload Too Large`.
- **JSON nesting depth limit**: JSON parsing enforces a recursion depth limit of 64 levels, preventing stack overflow on maliciously crafted payloads.
- **Safe process execution without shell interpolation**: Browser opening and Git bridge operations use structured argv arrays via `CreateProcess` / `fork+execvp`, eliminating command injection vulnerabilities.
- **Symlink and path traversal protection**: Working tree traversal skips symlinks, and all operations verify paths with `ensureNoSymlinkComponents` to prevent traversal through symlinked parent directories.
- **Path traversal and input validation**: All repository paths are validated and normalized by `Repository::resolveRepoPath`:
  - Paths cannot be empty, `.`, or `..`.
  - Absolute paths are forbidden.
  - Paths escaping the repository root (e.g. `../../etc/passwd`) throw an error ("path is outside the repository").
  - Internal `.minigit` files and directories cannot be accessed.
  - Filenames containing illegal characters (`\`, `:`, `*`, `?`, `"`, `<`, `>`, `|`, control characters) are rejected.
  - In sync bundles, branch names are validated via `RefManager::isValidBranchName` (rejecting `..`, control characters, and `peer-` prefixes), and all object SHA-1 hashes are verified prior to writing.

---

## Git Bridge

From the Web Studio:

- **Export to Git:** runs `git init` (if needed), `git add -A` and `git commit -F <temp message file>` in the same folder, and adds `.minigit/` to `.gitignore`.
- **Import from Git:** stages the current working files and records them as one Mini-Git commit.

It transfers the current **snapshot**, not the complete history. Git must be installed and on PATH.

---

## Command reference

| Command | Usage | Notes |
|---|---|---|
| `init` | `minigit init` | Fails if already initialized |
| `add` | `minigit add <file\|dir\|.>...` | Several paths allowed; stages deletions |
| `commit` | `minigit commit -m "message"` | Needs staged changes; finishes a merge if `MERGE_HEAD` exists |
| `log` | `minigit log [--oneline] [--stat]` | Newest first; merge commits show `Merge:`; readable local date (`%a %b %d %H:%M:%S %Y %z`) |
| `status` | `minigit status` | Staged / modified / deleted / untracked; reports active merge state and unmerged paths |
| `diff` | `minigit diff [file]` | Working file vs staged version |
| `branch` | `minigit branch [name] \| minigit branch -d <name>` | List, create, or delete (`-d`); on empty repo displays `* <currentBranch>` |
| `checkout` | `minigit checkout <branch\|hash>` | Switch branch or detach HEAD; refuses if dirty; empty repo reports branch has no commits yet |
| `merge` | `minigit merge <branch>` / `minigit merge --abort` | Up to date, fast-forward, or three-way with conflict markers; `--abort` restores pre-merge state |
| `undo` | `minigit undo` | Undoes the last commit while keeping working directory changes |
| `revert` | `minigit revert <hash>` | Reverts specified commit and creates a revert commit |
| `stash` | `minigit stash [save [-u] [-m msg] \| pop \| list]` | Save (optional `-u` untracked, `-m`), pop, or list stashes |
| `reflog` | `minigit reflog` | Chronological reflog entries with action and description |
| `html` | `minigit html [--no-open]` | Writes `.minigit/report.html` |
| `ui` | `minigit ui [--port <port>] [--no-open] [--lan]` | Validates port 1–65535 (default 8080); throws usage error if invalid |
| `serve` | `minigit serve [--port <port>] [--no-open] [--lan]` | Alias of `ui` |

Exit codes: `0` success, `1` Mini-Git error, `2` unexpected error.

Commit author: `MINIGIT_AUTHOR`, otherwise `USER` / `USERNAME`, otherwise `unknown`.

---

## Project structure

```
minigit/
├── include/
│   ├── Exceptions.h        exception hierarchy (virtual category())
│   ├── Hash.h              Hash value class (operator overloading)
│   ├── Hasher.h            SHA-1
│   ├── FileUtil.h          read/write/split helpers
│   ├── Object.h            Object, Blob, Tree, Commit
│   ├── ObjectStore.h       object database (+ loadAs<T> template)
│   ├── Index.h             staging area
│   ├── RefManager.h        HEAD and branches
│   ├── DiffEngine.h        template LCS diff
│   ├── Repository.h        backend facade (+ Snapshot, Status, stash, reflog)
│   ├── HtmlExporter.h      snapshot -> JSON -> web page
│   ├── HttpServer.h        Web Studio server and REST API
│   ├── PairingManager.h    pair codes, approvals, session tokens
│   ├── Json.h              JsonValue parser/serialiser
│   ├── Base64.h            Base64 encode/decode
│   ├── FrontendTemplate.h  generated: web UI embedded as a string
│   ├── Commands.h          Command base + 16 commands
│   └── CLI.h               argument dispatcher
├── src/                    implementations (.cpp) + main.cpp
├── templates/frontend.html the web UI (edit this)
├── tools/embed_frontend.py regenerates FrontendTemplate.h
├── tests/
│   ├── demo.sh             end-to-end demo script
│   ├── test_cli_server.cpp CLI command suite, port validation, ETag, and server tests
│   ├── test_core_fixes.cpp core repository logic, merge state, and safety fixes (10 tests)
│   ├── test_fixes.cpp      unit, regression, and edge-case safety tests (12 tests)
│   └── test_p2p.cpp        P2P sync, token authentication, and bundle security tests (8 tests)
├── docs/diagrams/          Mermaid diagrams + PNG exports
├── Makefile
├── setup.bat / setup.sh    one-command build
├── architecture.md         architecture, flowcharts, OOP concept map
├── PROMPTS.md              development prompts log
└── README.md
```

## Design and diagrams

- [architecture.md](architecture.md): layers, classes, `.minigit/` layout, command flowcharts, OOP concept map
- [Architecture Diagram](docs/diagrams/architecture.md)
- [Class Diagram (UML)](docs/diagrams/class-diagram.md)
- [Exception Hierarchy](docs/diagrams/exceptions.md)
- [Object Model](docs/diagrams/object-model.md)
- [Sequence Diagram: Commit](docs/diagrams/sequence-commit.md)
- [Activity Diagram: Merge](docs/diagrams/activity-merge.md)
- [Sequence Diagram: LAN Pairing and Sync](docs/diagrams/sequence-pairing-sync.md)
- [Deployment Diagram](docs/diagrams/deployment.md)
- [Use-Case Diagram](docs/diagrams/use-case.md)
- [State Diagrams](docs/diagrams/state-diagram.md)

## How it works

```
CLI:        main -> CLI -> Command -> Repository -> ObjectStore / Index / RefManager -> .minigit/
Web Studio: browser -> HttpServer -> Repository -> ...           (JSON over HTTP)
```

- **Front ends**: `CLI` + `Command` classes print text; `HttpServer` returns JSON to the browser. Both catch `MiniGitException`.
- **Facade**: `Repository` holds all version-control logic and never prints.
- **Storage**:

```
.minigit/
├── HEAD                 "ref: refs/heads/main" (or a raw hash when detached)
├── index                "<blobhash> <path>" per line (staging area)
├── refs/heads/<branch>  latest commit hash of each branch
├── objects/<sha1>       blob, tree and commit objects
├── MERGE_HEAD           only during an unfinished conflicted merge
├── stash                stash entries
├── reflog               history of HEAD movements
└── report.html          written by `minigit html`
```

**Object model:** a *blob* stores file content, a *tree* maps file paths to blob hashes, and a *commit* points to a tree, its parent and, for merge commits, a second parent. Each object's name is the SHA-1 of its type and contents, so identical content is stored once.

## Error messages

| Message | Meaning | Fix |
|---|---|---|
| `error [repository]: not a mini-git repository (run 'minigit init')` | No `.minigit` here or in any parent directory | Run `minigit init` |
| `error [general]: repository already initialized: <dir>` | `init` run inside an existing repository | Nothing to do |
| `error [file]: file not found: <path>` | Path does not exist and is not tracked | Check the file path |
| `error [commit]: nothing to commit (working tree clean)` | No files staged in the index | Stage modified files with `add` first |
| `error [commit]: nothing to commit (staged changes equal last commit)` | Staged index tree is identical to HEAD tree | Make and stage new changes |
| `error [branch]: branch already exists: <name>` | Target branch name already exists | Choose a distinct branch name |
| `error [branch]: invalid branch name: <name>` | Branch name contains illegal characters or format | Use alphanumeric characters, `-`, `_`, `.` |
| `error [branch]: cannot delete currently checked-out branch: <name>` | Attempted to delete active branch | Switch to another branch first |
| `error [branch]: cannot create a branch before the first commit` | Repository has no commits yet | Create the initial commit first |
| `error [checkout]: you have uncommitted changes; commit them first` | Working tree or staging index has uncommitted modifications | Commit or stash changes |
| `error [checkout]: branch '<name>' has no commits yet` | Checkout attempted on an empty branch | Create a commit on the branch first |
| `error [general]: untracked file would be overwritten: <path>` | Checkout or merge would overwrite an untracked file | Commit, move, or remove the file |
| `error [general]: path is outside the repository: <path>` | Relative path resolves outside repo root (path traversal) | Provide path within repository |
| `error [merge]: Automatic merge failed; fix conflicts and then commit the result.` | Conflicting modifications between merged branches | Resolve conflicts in editor, `add`, and `commit` (or `merge --abort`) |
| `error [merge]: a merge is already in progress; commit or abort it first` | Unfinished merge state (`MERGE_HEAD`) detected | Commit resolved merge or run `minigit merge --abort` |
| `error [merge]: cannot merge while HEAD is detached` | Cannot initiate merge while in detached HEAD state | Check out a branch before merging |
| `error [object-store]: unknown revision: <prefix>` | No commit matches the provided hash prefix | Check commit hash with `minigit log` |
| `error [object-store]: ambiguous revision: <prefix>` | Short hash matches multiple objects | Supply more hexadecimal characters |
| `error [object-store]: revision prefix too short (need >= 4 hex digits): <prefix>` | Prefix has fewer than 4 characters | Provide at least 4 hex characters |

## Troubleshooting

- **`<filesystem>` compile errors:** g++ older than 9. Upgrade, or on g++ 8 add `-lstdc++fs`.
- **Undefined references to `WSAStartup` / `socket` (Windows):** add `-lws2_32` at the end of the g++ command, or use `make`.
- **`g++` not recognized (Windows):** use the MSYS2 terminal or add its `bin` folder to PATH.
- **`minigit` not recognized:** use `.\minigit` (PowerShell) or the full path.
- **Web Studio port busy:** Mini-Git tries the next ports automatically, or pass `--port <N>`.
- **Teammate cannot connect:** host must use `--lan`, both on the same network, and the firewall must allow the port.
- **Path with spaces:** wrap it in quotes, e.g. `cd "C:\My Folder\test"`.
- **`add .` staged my source code:** you ran `init` inside the source folder. Use a separate folder or `.minigitignore`.
- **Start over:** delete `.minigit` and run `init` again.

## Limitations

- No remotes over the internet, tags, rename detection, rebase or cherry-pick.
- `diff` compares the working file with the **staged** version, not HEAD.
- The diff uses an O(n·m) LCS table: fine for normal text files, slow for very large ones.
- Objects are stored uncompressed in `.minigit/objects/`.
- `.minigitignore` supports only folder (`name/`), extension (`*.ext`) and exact-name patterns scoped to the repository root.
- The Web Studio synchronizes via conditional ETag polling (2.5 s interval, 304 Not Modified when unchanged), not WebSocket push.
- The Git Bridge copies snapshots, not full historical commit chains.
- Pairing state is in memory: restarting the server resets active pair codes and paired peer tokens.
- `minigit html` styling loads from CDNs (Tailwind, Font Awesome, Google Fonts) and requires an internet connection for full styling.

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
