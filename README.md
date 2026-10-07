# Mini-Git

A small version control system written in C++17. It tracks changes to text files, hashes content with a hand-written SHA-1, and supports commits, branches, checkout, diff and fast-forward merge. All data lives in a `.minigit/` folder, the same idea as real Git.

---

## Table of Contents
1. [Features](#features)
2. [Requirements](#requirements)
3. [Build](#build)
4. [Tutorial: your first repository](#tutorial-your-first-repository)
5. [Tutorial: branches and merging](#tutorial-branches-and-merging)
6. [Command reference](#command-reference)
7. [Project structure](#project-structure)
8. [How it works](#how-it-works)
9. [Error messages](#error-messages)
10. [Troubleshooting](#troubleshooting)
11. [Limitations](#limitations)
12. [Team workflow](#team-workflow)

---

## Features

| Command | Purpose |
|---|---|
| `init` | Create a repository |
| `add` | Stage files or whole folders |
| `commit` | Save a snapshot with a message |
| `log` | Show commit history |
| `status` | Show staged, modified, deleted and untracked files |
| `diff` | Line-by-line changes since the last `add` |
| `branch` | List or create branches |
| `checkout` | Switch branch or go to a commit hash |
| `merge` | Fast-forward merge |

## Requirements

- A C++17 compiler: **g++ 9 or newer** (or recent clang / MSVC)
- `make` (optional; you can compile with one g++ command)

Check your compiler:
```
g++ --version
```

**Windows:** install [MSYS2](https://www.msys2.org/), then in the MSYS2 terminal run:
```
pacman -S mingw-w64-ucrt-x86_64-gcc make
```

## Build

From the project folder:

**Linux / macOS**
```bash
make
```

**Windows (cmd or PowerShell)**
```
cd path\to\minigit
g++ -std=c++17 -Iinclude src/*.cpp -o minigit.exe
```

**Any OS, no make**
```bash
g++ -std=c++17 -Iinclude src/*.cpp -o minigit
```

You now have an executable called `minigit` (`minigit.exe` on Windows).

### Quick self-test
```bash
bash tests/demo.sh
```
Runs a scripted session (init, add, commit, branch, checkout, merge, error cases) in a temporary folder. On Windows use Git Bash or the MSYS2 terminal.

---

## Tutorial: your first repository

> Always work in a **separate test folder**, not inside the source folder, so your source files are not staged.

### Step 0: set up a test folder

**Windows (PowerShell)**
```
# run from the folder that contains the minigit folder
mkdir test
copy minigit\minigit.exe test
cd test
```
Use `.\minigit` in PowerShell, or `minigit` in cmd. The examples below write `minigit`.

**Linux / macOS**
```bash
mkdir ~/test && cd ~/test
export PATH=$PATH:/path/to/minigit/folder
```

### Step 1: initialize
```
minigit init
```
Creates `.minigit/` with `objects/`, `refs/heads/`, `HEAD`.

### Step 2: create a file and check status
```
echo hello > a.txt
minigit status
```
```
On branch main
Untracked:
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
`status` shows `a.txt` under **Modified**. `diff` shows removed lines as `-N:` and added lines as `+N:`.

### Step 5: stage and commit again
```
minigit add a.txt
minigit commit -m "second commit"
```

### Step 6: view history
```
minigit log
minigit log --oneline
```

### Adding whole folders
```
minigit add .          # everything (except .minigit)
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
Merge the feature branch in:
```
minigit merge feature
```
```
Fast-forward to 8c1d4e2
```
`f.txt` is back, and `main` now points at the same commit as `feature`.

### Visiting an old commit
```
minigit log --oneline
minigit checkout 3fa21c9      # any unique prefix of 4+ characters
```
This puts you in a **detached HEAD** state. Return with `minigit checkout main`.

---

## Command reference

| Command | Usage | Notes |
|---|---|---|
| `init` | `minigit init` | Fails if already initialized |
| `add` | `minigit add <file\|dir\|.>...` | Accepts several paths |
| `commit` | `minigit commit -m "message"` | Needs staged changes |
| `log` | `minigit log [--oneline]` | Newest first |
| `status` | `minigit status` | Staged / Modified / Deleted / Untracked |
| `diff` | `minigit diff [file]` | Working file vs staged version |
| `branch` | `minigit branch [name]` | No name = list |
| `checkout` | `minigit checkout <branch\|hash>` | Refuses if uncommitted changes exist |
| `merge` | `minigit merge <branch>` | Fast-forward only |

Exit codes: `0` success, `1` Mini-Git error, `2` unexpected error.

Optional: set the commit author name with the `MINIGIT_AUTHOR` environment variable (otherwise `USER` / `USERNAME` is used).

---

## Project structure

```
minigit/
├── include/
│   ├── Exceptions.h     exception hierarchy
│   ├── Hash.h           Hash value class (operator overloading)
│   ├── Hasher.h         SHA-1 interface
│   ├── FileUtil.h       read/write/split helpers
│   ├── Object.h         Object, Blob, Tree, Commit
│   ├── ObjectStore.h    object database (+ loadAs<T> template)
│   ├── Index.h          staging area
│   ├── RefManager.h     HEAD and branches
│   ├── DiffEngine.h     template LCS diff
│   ├── Repository.h     backend facade
│   ├── Commands.h       Command base + 9 commands
│   └── CLI.h            argument dispatcher
├── src/                 implementations (.cpp) + main.cpp
├── tests/demo.sh        end-to-end demo script
├── Makefile
├── architecture.txt     architecture and flowcharts
└── README.md
```

## How it works

```
main -> CLI -> Command -> Repository -> ObjectStore / Index / RefManager -> .minigit/ files
```

- **Frontend** (`CLI`, `Commands`): parses arguments, prints output, handles exceptions.
- **Backend** (`Repository` and the classes it owns): all logic, no printing.
- **Storage** (`.minigit/`):

```
.minigit/
├── HEAD                 "ref: refs/heads/main"
├── index                "<blobhash> <path>" per line (staging area)
├── refs/heads/<branch>  latest commit hash of each branch
└── objects/<sha1>       blob, tree and commit objects
```

**Object model:** a *blob* stores file content, a *tree* maps file paths to blob hashes, and a *commit* points to a tree and its parent commit. Each object's name is the SHA-1 of its contents, so identical content is stored once. See `architecture.txt` for flowcharts of every command.

## Error messages

| Message | Meaning | Fix |
|---|---|---|
| `error [repository]: not a mini-git repository` | No `.minigit` in this or any parent folder | Run `minigit init` |
| `error [repository]: repository already initialized` | `init` run twice | Nothing to do |
| `error [file]: file not found` | Path does not exist or is not tracked | Check the name |
| `error [commit]: nothing to commit` | Nothing staged, or staged content equals the last commit | `add` changed files first |
| `error [branch]: branch already exists` / `invalid branch name` | Duplicate or illegal name | Use letters, digits, `-`, `_`, `.` |
| `error [branch]: cannot create a branch before the first commit` | No commits yet | Commit first |
| `error [checkout]: you have uncommitted changes` | Staged or modified files present | Commit them first |
| `error [merge]: branches have diverged` | Not a fast-forward case | Not supported (see Limitations) |
| `error [object-store]: unknown or ambiguous revision` | Hash prefix too short or not unique | Use more characters (min 4) |

## Troubleshooting

- **`<filesystem>` compile errors:** your g++ is older than 9. Upgrade, or on g++ 8 add `-lstdc++fs`.
- **`g++` not recognized (Windows):** install MSYS2 and use its terminal, or add its `bin` folder to PATH.
- **`minigit` not recognized:** use `.\minigit` (PowerShell) or the full path to the executable.
- **Path with spaces:** wrap it in quotes, e.g. `cd "C:\My Folder\test"`.
- **`add .` staged my source code:** you ran `init` inside the source folder. Use a separate test folder.
- **Want to start over:** delete the `.minigit` folder in the test directory and run `init` again.

## Limitations

- Merge is **fast-forward only**; diverged branches are rejected.
- `diff` compares the working file against the **staged** version (like `git diff`), not against HEAD.
- No `.gitignore`-style ignore file, no remotes, no tags, no file rename detection.
- The diff uses an O(n·m) table, fine for normal text files but not huge ones.
- Intended for text files; no compression of stored objects.