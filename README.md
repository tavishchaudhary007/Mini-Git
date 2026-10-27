# Mini-Git

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
12. [Git Bridge](#git-bridge)
13. [Command reference](#command-reference)
14. [Project structure](#project-structure)
15. [Design and diagrams](#design-and-diagrams)
16. [How it works](#how-it-works)
17. [Error messages](#error-messages)
18. [Troubleshooting](#troubleshooting)
19. [Limitations](#limitations)

---

## Features

**Command line (12 commands)**

| Command | Purpose |
|---|---|
| `init` | Create a repository |
| `add` | Stage files, folders, or deletions |
| `commit` | Save a snapshot with a message |
| `log` | Show history (`--oneline`, `--stat`) |
| `status` | Staged, modified, deleted and untracked files |
| `diff` | Line-by-line changes (LCS) since the last `add` |
| `branch` | List or create branches |
| `checkout` | Switch branch, or go to a commit (detached HEAD) |
| `merge` | Fast-forward or three-way merge; `--abort` cancels a conflicted merge |
| `html` | Write a read-only visual report and open it |
| `ui` | Start the interactive Web Studio |
| `serve` | Same as `ui` |

**Web Studio only** (through the local REST API): edit and save files, unstage, discard changes, delete branches, revert a commit, undo the last commit, stash save/pop/list, reflog view and restore, time-travel through commits, repository health banner, LAN pairing with a 6-digit code, pull/push sync bundles, and Git export/import.

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
Builds and runs `tests/test_fixes.cpp` (regression and edge-case tests) and `tests/test_p2p.cpp` (P2P sync, token authentication and network security tests).

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
`--stat` adds a per-file summary of insertions and deletions. In the full `log`, `Date:` is printed as a raw Unix timestamp.

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
Starts an embedded HTTP server (raw sockets, no external libraries). If the port is busy it tries the next 29 ports. Without `--lan` it listens on `127.0.0.1` only.

What you can do in the browser:

- **Stage / unstage / discard** files and folders (discarding an untracked file deletes it, after a confirmation)
- **Edit and save** files in the built-in editor
- **Commit** with a message
- **Branches:** create, switch, delete, merge (conflicts are reported and can be resolved, committed or aborted)
- **Commit actions:** check out, branch from, or **revert** a commit; **undo** the last commit (keeps your changes in the working directory)
- **Stash:** save, list and pop (the API also accepts `includeUntracked`)
- **Reflog:** see every HEAD movement and restore any earlier state
- **Time travel:** scrub through commits and view files as they were
- **Repository health** banner and commit graph
- **LAN pairing, pull and push** (next section) and the **Git Bridge**

The page polls `/api/snapshot` every **2.5 seconds**, so changes made from the terminal show up within a few seconds.

The commit graph is a single time-sorted column of commits with branch labels; it does not draw parent edges or one lane per branch.

---

## LAN pairing and P2P sync

1. The host runs `minigit ui --lan`. The Pair panel shows a **6-digit code** and the LAN address.
2. The teammate opens that address and enters the code.
3. The host sees the request (the list refreshes every 1.5 s) and clicks **Allow**.
4. The teammate's browser receives a **session token** and stores it.
5. **Pull** downloads the host's repository as a JSON bundle and imports it into the teammate's own local Mini-Git; **Push** sends the teammate's bundle to the host.

Security rules (enforced in `HttpServer` and `PairingManager`):

- A code is valid for **300 seconds** and is used once; **5 wrong attempts** lock it until a new one is generated.
- Pending requests expire after 120 seconds. The host can revoke a paired peer at any time.
- From another machine, **only** `/api/pair`, `/api/pair/status`, `/api/sync/bundle` and `/api/sync/import-bundle` are reachable; everything else returns `403`.
- Sync endpoints need the `X-MiniGit-Token` header; a missing or revoked token returns `401`.

On import, every object's SHA-1 is recomputed; if any hash does not match, nothing is written. Objects are saved in dependency order (blobs, trees, commits). For each branch: new branches are created, behind branches are fast-forwarded, and **diverged branches are kept as `peer-<branch>`** so nothing is overwritten. Binary files travel as Base64.

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
| `log` | `minigit log [--oneline] [--stat]` | Newest first; merge commits show `Merge:` |
| `status` | `minigit status` | Staged / modified / deleted / untracked |
| `diff` | `minigit diff [file]` | Working file vs staged version |
| `branch` | `minigit branch [name]` | No name = list |
| `checkout` | `minigit checkout <branch\|hash>` | Hash prefix of 4+ characters; refuses with uncommitted changes |
| `merge` | `minigit merge <branch>` / `minigit merge --abort` | Up to date, fast-forward, or three-way |
| `html` | `minigit html [--no-open]` | Writes `.minigit/report.html` |
| `ui` | `minigit ui [--port <N>] [--no-open] [--lan]` | Default port 8080 |
| `serve` | `minigit serve [--port <N>] [--no-open] [--lan]` | Alias of `ui` |

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
│   ├── Commands.h          Command base + 12 commands
│   └── CLI.h               argument dispatcher
├── src/                    implementations (.cpp) + main.cpp
├── templates/frontend.html the web UI (edit this)
├── tools/embed_frontend.py regenerates FrontendTemplate.h
├── tests/
│   ├── demo.sh             end-to-end demo script
│   ├── test_fixes.cpp      unit and regression tests
│   └── test_p2p.cpp        P2P sync and security tests
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
| `error [repository]: not a mini-git repository (run 'minigit init')` | No `.minigit` here or in any parent | Run `minigit init` |
| `error [general]: repository already initialized` | `init` run twice | Nothing to do |
| `error [file]: file not found: <path>` | Path does not exist and is not tracked | Check the name |
| `error [commit]: nothing to commit (...)` | Nothing staged, or staged content equals the last commit | `add` changed files first |
| `error [branch]: branch already exists` / `invalid branch name` | Duplicate or illegal name | Use letters, digits, `-`, `_`, `.` |
| `error [branch]: cannot create a branch before the first commit` | No commits yet | Commit first |
| `error [checkout]: you have uncommitted changes; commit them first` | Staged or modified files | Commit (or stash in the Web Studio) |
| `error [general]: untracked file '<f>' would be overwritten; ...` | Checkout/merge would replace an untracked file | Move or commit it |
| `error [merge]: Automatic merge failed; fix conflicts ...` | Both branches changed the same file | Fix, `add`, `commit`, or `merge --abort` |
| `error [merge]: a merge is already in progress; commit or abort it first` | `MERGE_HEAD` exists | Finish or abort the merge |
| `error [merge]: cannot merge while HEAD is detached` | Not on a branch | `checkout` a branch |
| `error [object-store]: unknown or ambiguous revision` | Hash prefix too short (under 4) or not unique | Use more characters |

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
- Objects are stored uncompressed.
- `.minigitignore` supports only folder (`name/`), extension (`*.ext`) and exact-name patterns.
- The Web Studio refreshes by polling (2.5 s), not instant push.
- The commit graph is a single time-sorted column without parent edges.
- The Git Bridge copies snapshots, not full history.
- Pairing state is in memory: restarting the server forgets codes and paired peers.
- `minigit html` needs internet for styling.
- Full `log` prints `Date:` as a raw Unix timestamp.
