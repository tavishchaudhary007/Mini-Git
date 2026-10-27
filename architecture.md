# Mini-Git Architecture & Design Specification

> **C++17 Command-Line Version Control System & Visual Web Studio**

---

## Table of Contents
1. [Layered Overview](#1-layered-overview)
2. [Class Responsibilities](#2-class-responsibilities)
3. [Relationships & UML Mapping](#3-relationships--uml-mapping)
4. [On-Disk Layout (`.minigit/`)](#4-on-disk-layout-minigit)
5. [Top-Level Control Flow](#5-top-level-control-flow)
6. [Command Flowcharts](#6-command-flowcharts)
   - [init](#61-flowchart-minigit-init)
   - [add](#62-flowchart-minigit-add-path)
   - [commit](#63-flowchart-minigit-commit--m-msg)
   - [status](#64-flowchart-minigit-status)
   - [diff](#65-flowchart-minigit-diff-file)
   - [branch & checkout](#66-flowchart-minigit-branch--checkout)
   - [merge](#67-flowchart-minigit-merge-branch--minigit-merge---abort)
7. [Object Graph Evolution](#7-object-graph-evolution)
8. [OOP & C++ Concept Map](#8-oop--c-concept-map)
9. [Second Frontend: Web Studio (`minigit html`, `minigit ui`)](#9-second-frontend-web-studio-minigit-html-minigit-ui)
10. [More Diagrams](#10-more-diagrams)

---

## 1. Layered Overview

Mini-Git is organized into three decoupled layers: **Frontends (CLI and Web Studio)**, **Backend Facade (`Repository`)**, and **Storage Subsystems (`.minigit/`)**.

```mermaid
flowchart TB
    User(["Developer"])
    Browser["Browser<br/>Web Studio page (frontend.html)"]
    Terminal["Terminal"]

    subgraph FE["Layer 1: Front ends"]
        direction LR
        subgraph CLIF["Command line"]
            Main["main.cpp"] --> CLI["CLI<br/>command registry"] --> Cmds["12 Command classes<br/>Commands.cpp"]
        end
        subgraph WEB["Web Studio"]
            Server["HttpServer<br/>raw sockets, REST routes"]
            Exporter["HtmlExporter<br/>Snapshot to JSON / HTML"]
            Tmpl["FrontendTemplate.h<br/>embedded UI"]
            Pair["PairingManager<br/>codes, approval, tokens"]
        end
    end

    subgraph CORE["Layer 2: Facade"]
        Repo["Repository<br/>all version-control logic<br/>(no printing)"]
    end

    subgraph SVC["Layer 3: Storage and services"]
        direction LR
        Store["ObjectStore"]
        Idx["Index"]
        Refs["RefManager"]
        Hasher["Hasher (SHA-1)"]
        Diff["DiffEngine T (LCS)"]
        Json["JsonValue"]
        B64["Base64"]
    end

    subgraph DISK[".minigit/ on disk"]
        direction LR
        O[("objects/")]
        I[("index")]
        H[("HEAD, refs/heads/")]
        X[("MERGE_HEAD, stash,<br/>reflog, report.html")]
    end

    User --> Terminal --> Main
    User --> Browser
    Browser <-->|"HTTP JSON API<br/>poll every 2.5 s"| Server
    Server --> Exporter --> Tmpl
    Server --> Pair
    Cmds --> Repo
    Server --> Repo
    Repo --> Store --> O
    Repo --> Idx --> I
    Repo --> Refs --> H
    Repo --> X
    Store --> Hasher
    Repo --> Diff
    Repo --> Json
    Repo --> B64
```

| Layer | Components | Responsibility |
|---|---|---|
| **Frontend** | `main.cpp`, `CLI`, `Command` hierarchy; `HttpServer`, `HtmlExporter`, `PairingManager`, embedded web UI | Parses arguments or HTTP requests, prints output or returns JSON, catches exceptions. |
| **Backend Facade** | `Repository` | Contains all business logic, never prints to `stdout`/`stderr`, owns storage abstractions. |
| **Storage Subsystems** | `ObjectStore`, `Index`, `RefManager` (plus helpers `Hasher`, `DiffEngine<T>`, `JsonValue`, `Base64`) | Reads and writes immutable objects, staging index, and branch references under `.minigit/`. |

---

## 2. Class Responsibilities

```mermaid
classDiagram
    direction TB

    class CLI {
        -map~string, unique_ptr~Command~~ commands_
        -registerCommand(unique_ptr~Command~ c)
        +run(argc, argv) int
    }
    class Command {
        <<abstract>>
        +name() string*
        +usage() string*
        +needsRepo() bool
        +execute(Repository* repo, Args args)*
    }
    class InitCmd
    class AddCmd
    class CommitCmd
    class LogCmd
    class StatusCmd
    class DiffCmd
    class BranchCmd
    class CheckoutCmd
    class MergeCmd
    class HtmlCmd
    class UiCmd
    class ServeCmd

    class Repository {
        <<Facade>>
        -fs::path root_
        -fs::path gitDir_
        -ObjectStore store_
        -Index index_
        -RefManager refs_
        +init(dir)$
        +findRoot(start)$ fs::path
        +add(path)
        +unstage(path)
        +discard(path)
        +commit(message) Hash
        +revert(hash) Hash
        +log() vector~Commit~
        +status() Status
        +diff(path) string
        +commitStats(hash) CommitStat
        +snapshot() Snapshot
        +createBranch(name)
        +checkout(target)
        +findMergeBase(h1, h2) Hash
        +merge(branch) string
        +mergeAbort() string
        +undo() string
        +stashSave(msg, untracked) string
        +stashPop() string
        +reflog() vector~ReflogEntry~
        +exportBundleJson() string
        +importBundleJson(json) string
        +exportToGit() string
        +importFromGit() string
        -restoreCommit(hash)
        -isAncestor(a, d) bool
        -isDirty() bool
    }

    class ObjectStore {
        -fs::path dir_
        +save(Object obj) Hash
        +load(Hash h) unique_ptr~Object~
        +loadAs~T~(Hash h) unique_ptr~T~
        +exists(Hash h) bool
        +resolvePrefix(prefix) Hash
        +listAll() vector~Hash~
    }
    class Index {
        -fs::path file_
        -map~string, Hash~ entries_
        +set(path, hash)
        +remove(path)
        +has(path) bool
        +replace(entries)
        +save()
    }
    class RefManager {
        -fs::path gitDir_
        +currentBranch() string
        +headCommit() Hash
        +updateHead(hash)
        +createBranch(name, at)
        +setBranch(name, at)
        +switchToBranch(name)
        +detach(hash)
        +isValidBranchName(name)$ bool
    }

    class Object {
        <<abstract>>
        +type() string*
        +serialize() string*
        +raw() string
        +hash() Hash
        +deserialize(raw)$ unique_ptr~Object~
    }
    class Blob {
        -string content_
    }
    class Tree {
        -map~string, Hash~ entries_
        +parse(body)$ Tree
    }
    class Commit {
        -Hash tree_
        -Hash parent_
        -Hash parent2_
        -string author_
        -string message_
        -time_t time_
        +parse(body)$ Commit
        +parent2() Hash
    }

    class Hash {
        -string value_
        +str() string
        +shortStr(n) string
        +operator==(Hash) bool
        +operator<(Hash) bool
    }
    class Hasher {
        +sha1(data)$ string
    }
    class DiffEngine~T~ {
        +diff(vector~T~ a, vector~T~ b)$ vector~DiffOp~T~~
    }

    class HttpServer {
        +run(repo, port, open, lan)$
    }
    class HtmlExporter {
        +toJson(snap, name, isSyncBundle)$ string
        +render(snap, name)$ string
    }
    class PairingManager {
        -string currentCode_
        -int failedAttempts_
        -bool isLocked_
        -map~string, PendingPairRequest~ pendingRequests_
        -vector~PairedPeer~ pairedPeers_
        +generateNewCode() string
        +submitCode(code, ip, name) PairSubmitResult
        +decideRequest(id, allow, label) bool
        +isValidSessionToken(token) bool
        +revokePeer(idOrToken) bool
    }
    class JsonValue {
        -Type type_
        -vector~JsonValue~ arrVal_
        -map~string, JsonValue~ objVal_
        +parse(text, err)$ JsonValue
        +serialize() string
        +operator[](key) JsonValue
    }
    class Base64 {
        <<namespace>>
        +encode(in) string
        +decode(in, out, err) bool
    }

    class MiniGitException {
        +category() const char*
    }
    class runtime_error {
        <<std>>
    }

    CLI *-- "12" Command : owns
    Command <|-- InitCmd
    Command <|-- AddCmd
    Command <|-- CommitCmd
    Command <|-- LogCmd
    Command <|-- StatusCmd
    Command <|-- DiffCmd
    Command <|-- BranchCmd
    Command <|-- CheckoutCmd
    Command <|-- MergeCmd
    Command <|-- HtmlCmd
    Command <|-- UiCmd
    Command <|-- ServeCmd
    Command ..> Repository : uses

    Repository *-- ObjectStore
    Repository *-- Index
    Repository *-- RefManager
    Repository ..> DiffEngine~T~ : DiffEngine~string~
    Repository ..> HtmlExporter
    Repository ..> JsonValue
    Repository ..> Base64

    ObjectStore ..> Object : creates via deserialize
    Object <|-- Blob
    Object <|-- Tree
    Object <|-- Commit
    Object ..> Hasher : hash()
    Tree --> Hash
    Commit --> Hash
    Index --> Hash

    UiCmd ..> HttpServer
    ServeCmd ..> UiCmd
    HtmlCmd ..> HtmlExporter
    HttpServer ..> Repository
    HttpServer *-- PairingManager : local object in run()
    HttpServer ..> HtmlExporter
    HttpServer ..> JsonValue

    runtime_error <|-- MiniGitException
```

The exception classes are drawn separately in [docs/diagrams/exceptions.md](docs/diagrams/exceptions.md).

### Core Data & Object Models
- **`Hash`**: Value type wrapping a 40-character hexadecimal SHA-1 digest. Overloads `==`, `!=`, `<`, and `<<`.
- **`Hasher`**: SHA-1 cryptographic engine implementing the Merkle-Damgård construction.
- **`Object`** *(Abstract Base)*: Defines `type()`, `serialize()`, `hash()`, and factory `deserialize()`.
  - **`Blob`**: Stores raw uncompressed file contents.
  - **`Tree`**: Maps relative file paths to `Hash` identifiers (`map<string, Hash>`), representing snapshot directories.
  - **`Commit`**: Encapsulates a root `Tree` hash, parent commit `Hash`, an optional second parent `parent2` (merge commits only), author string, timestamp, and log message.

### Engine Services
- **`ObjectStore`**: Content-addressable object store located at `.minigit/objects/`. Implements template method `loadAs<T>()` and prefix resolver `resolvePrefix()` for short hashes.
- **`Index`**: Staging area manager mapping relative paths to staged blob hashes; synchronizes with `.minigit/index`.
- **`RefManager`**: Manages `.minigit/HEAD` and `.minigit/refs/heads/` (supporting attached branches and detached HEAD).
- **`DiffEngine<T>`** *(Template)*: Computes Longest Common Subsequence (LCS) line diffs, producing `DiffOp` operations (`Keep`, `Add`, `Remove`).
- **`Repository`**: Central facade pattern mediating all version control operations.
- **`Status`**: Struct packaging lists of `staged`, `modified`, `deleted`, and `untracked` paths.
- **`Snapshot`**: Complete read-only memory capture of repository state (objects, branches, working files, index).
- **`HtmlExporter`**: Converts a `Snapshot` into JSON (or a sync bundle with Base64 binaries) and injects it into the embedded web UI.
- **`HttpServer`**: Socket-based local server for the Web Studio REST API; binds to 127.0.0.1, or 0.0.0.0 with `--lan`.
- **`PairingManager`**: 6-digit pair codes, host approval, session tokens and revocation for LAN sync.
- **`JsonValue`** / **`Base64`**: Hand-written JSON parser/serialiser and Base64 codec used by the API and sync bundles.

### Commands & Dispatcher
- **`Command`** *(Abstract Base)*: Declares `name()`, `usage()`, `needsRepo()`, and `execute(repo, args)`.
- **`CLI`**: Registry storing `map<string, unique_ptr<Command>>` dispatching command execution.
- **`MiniGitException`** *(Exception Base)*: Provides `virtual category()` caught uniformly by `CLI::run`.

---

## 3. Relationships & UML Mapping

- **Inheritance**:
  - `Blob`, `Tree`, `Commit` $\rightarrow$ `Object`
  - Concrete commands (`InitCmd`, `AddCmd`, `CommitCmd`, `LogCmd`, `StatusCmd`, `DiffCmd`, `BranchCmd`, `CheckoutCmd`, `MergeCmd`, `HtmlCmd`, `UiCmd`, `ServeCmd`) $\rightarrow$ `Command`
  - Custom exceptions (`NotARepoException`, `FileNotFoundException`, `InvalidObjectException`, `BranchException`, `NothingToCommitException`, `CheckoutException`, `MergeException`) $\rightarrow$ `MiniGitException` $\rightarrow$ `std::runtime_error`
- **Composition**:
  - `Repository` owns `ObjectStore`, `Index`, `RefManager`
  - `CLI` owns `map<string, unique_ptr<Command>>`
  - `HttpServer::run` owns a local `PairingManager`
- **Association**:
  - `Tree` associates with `Hash` (blob identifiers)
  - `Commit` associates with `Hash` (tree & parent commit)
  - `Index` associates with `Hash` (staged blob IDs)
- **Dependency**:
  - `Repository` $\rightarrow$ `DiffEngine<string>`, `Blob`, `Tree`, `Commit`, `Snapshot`
  - `HtmlCmd` $\rightarrow$ `HtmlExporter`, `Repository`, `Snapshot`
  - `UiCmd` $\rightarrow$ `HttpServer` $\rightarrow$ `Repository`, `PairingManager`, `HtmlExporter`, `JsonValue`
  - `Repository` $\rightarrow$ `JsonValue`, `Base64` (sync bundles)
  - `ObjectStore` $\rightarrow$ `Object` (instantiates through `Object::deserialize`)

---

## 4. On-Disk Layout (`.minigit/`)

```text
.minigit/
├── HEAD                 "ref: refs/heads/main"  (or raw hash when detached)
├── index                "<blobhash> <filepath>" (one line per staged entry)
├── MERGE_HEAD           other branch's tip while a conflicted merge is unfinished
├── stash                one line per stash: <commit> <index tree> <message>
├── reflog               time|action|old|new|description, one line per HEAD move
├── report.html          page written by `minigit html`
├── refs/
│   └── heads/
│       ├── main         latest 40-char commit hash on 'main'
│       └── feature      latest 40-char commit hash on 'feature'
└── objects/
    ├── 7f59f3...        Blob:   "blob\n<raw file content>"
    ├── a91c02...        Tree:   "tree\n<blobhash> <filepath>\n..."
    └── e1dfc6...        Commit: "commit\ntree <treehash>\nparent <parenthash>\n[parent2 <hash>]\nauthor ...\ntime ...\n\n<message>"
```

> [!NOTE]
> Every object is addressed by the SHA-1 hash of its header and body: `SHA-1(type + "\n" + content)`.

---

## 5. Top-Level Control Flow

```mermaid
flowchart TD
    Start(["main(argc, argv)"]) --> CLIEntry["CLI::run(argc, argv)"]
    CLIEntry --> CheckArgs{"argc &lt; 2?"}
    CheckArgs -- Yes --> PrintUsage["printUsage() -> return 1"]
    CheckArgs -- No --> CheckCmd{"Command in registry?"}
    CheckCmd -- No --> UnknownCmd["Print 'unknown command' -> return 1"]
    CheckCmd -- Yes --> NeedsRepo{"needsRepo() == true?"}
    
    NeedsRepo -- Yes --> FindRoot["Repository::findRoot(cwd)"]
    FindRoot --> FoundRepo{"Found .minigit?"}
    FoundRepo -- No --> ThrowNotRepo["Throw NotARepoException"]
    FoundRepo -- Yes --> ExecCmd["cmd->execute(repo, args)"]
    NeedsRepo -- No --> ExecCmd

    ExecCmd --> Success["Exit code 0"]
    ThrowNotRepo --> CatchMiniGit["Catch MiniGitException -> stderr 'error [category]: ...' -> return 1"]
    ExecCmd -.-> CatchMiniGit
    ExecCmd -.-> CatchStd["Catch std::exception -> stderr 'fatal: ...' -> return 2"]
```

---

## 6. Command Flowcharts

### 6.1 Flowchart: `minigit init`

```mermaid
flowchart TD
    A["minigit init"] --> B{".minigit exists?"}
    B -- Yes --> C["Throw 'already initialized'"]
    B -- No --> D["Create .minigit/objects/"]
    D --> E["Create .minigit/refs/heads/"]
    E --> F["Write HEAD = 'ref: refs/heads/main'"]
    F --> G["Print 'Initialized empty Mini-Git repository'"]
```

---

### 6.2 Flowchart: `minigit add <path>`

```mermaid
flowchart TD
    A["minigit add &lt;path&gt;"] --> B["relPath(path) inside repository root?"]
    B -- No --> C["Throw 'path outside repository'"]
    B -- Yes --> D{"What is &lt;path&gt;?"}

    D -- Directory --> E["Iterate workingFiles() under directory"]
    E --> F["For each file: compute Blob hash, store.save(blob), index.set(path, hash)"]
    F --> G["Detect deleted files: if in index but not on disk, index.remove(path)"]

    D -- Regular File --> H["Read file bytes -> create Blob"]
    H --> I["store.save(blob) -> index.set(path, hash)"]

    D -- Missing File --> J{"Exists in index?"}
    J -- Yes --> K["index.remove(path) (Stage Deletion)"]
    J -- No --> L["Throw FileNotFoundException"]

    G --> SaveIdx["index.save()"]
    I --> SaveIdx
    K --> SaveIdx
```

---

### 6.3 Flowchart: `minigit commit -m "msg"`

```mermaid
flowchart TD
    A["minigit commit -m 'msg'"] --> B{"Message empty?"}
    B -- Yes --> ErrMsg["Throw MiniGitException (commit message must not be empty)"]
    B -- No --> C{"Index empty?"}
    C -- Yes --> ErrEmpty["Throw NothingToCommitException (staging area is empty)"]
    C -- No --> D["Build Tree object from index entries"]
    D --> P["parent = refs.headCommit()"]
    P --> MH{".minigit/MERGE_HEAD exists?"}
    MH -- Yes --> P2["parent2 = hash in MERGE_HEAD"]
    MH -- No --> P0["parent2 = empty"]
    P2 --> G
    P0 --> F{"parent exists AND parent.tree == new tree?"}
    F -- Yes --> ErrNoChange["Throw NothingToCommitException (no changes staged)"]
    F -- No --> G["store.save(tree)"]
    G --> H["store.save(Commit(tree, parent, author, time, message, parent2))"]
    H --> J["refs.updateHead(commitHash)<br/>on a branch: refs/heads/branch = hash<br/>detached: HEAD = hash"]
    J --> K{"parent2 set?"}
    K -- Yes --> M1["Delete MERGE_HEAD<br/>recordReflog('merge')"]
    K -- No --> M2["recordReflog('commit')"]
    M1 --> Out["Print [branch shorthash] msg"]
    M2 --> Out
```

- A commit made while `MERGE_HEAD` exists finishes a conflicted merge: it gets two parents even if the tree equals the parent's tree.
- The author comes from `MINIGIT_AUTHOR`, then `USER` / `USERNAME`, else `unknown`.

---

### 6.4 Flowchart: `minigit status`

```mermaid
flowchart TD
    A["minigit status"] --> B["Collect headTree, index, working directory files"]
    B --> C["Examine each file in index"]
    C --> D{"Differs from headTree or not in headTree?"}
    D -- Yes --> Staged["Mark as STAGED"]
    D -- No --> E{"Missing on disk?"}
    E -- Yes --> Deleted["Mark as DELETED"]
    E -- No --> F{"Disk content hash != index hash?"}
    F -- Yes --> Modified["Mark as MODIFIED"]
    F -- No --> Clean["Clean"]

    B --> G["Examine files in headTree not present in index"]
    G --> StagedDel["Mark as STAGED (deleted)"]

    B --> H["Examine files on disk not present in index"]
    H --> Untracked["Mark as UNTRACKED"]
```

---

### 6.5 Flowchart: `minigit diff [file]`

```mermaid
flowchart TD
    A["minigit diff [file]"] --> B["Identify target files (single file or all indexed files)"]
    B --> C["For each target: oldText = blob(index[path]), newText = readFile(disk)"]
    C --> D{"oldText == newText?"}
    D -- Yes --> Skip["Skip file"]
    D -- No --> E["splitLines(oldText), splitLines(newText)"]
    E --> F["DiffEngine&lt;string&gt;::diff(oldLines, newLines)"]
    F --> G["Build dynamic programming LCS table dp[i][j]"]
    G --> H["Traverse backtrack path -> classify Keep / Remove / Add"]
    H --> I["Print removed lines with -N: and added lines with +N:"]
```

---

### 6.6 Flowchart: `minigit branch` & `checkout`

```mermaid
flowchart TD
    subgraph BranchCmd ["minigit branch &lt;name&gt;"]
        B1["Check HEAD commit exists"] --> B2{"Valid & non-existent name?"}
        B2 -- No --> B_Err["Throw BranchException"]
        B2 -- Yes --> B3["Write refs/heads/&lt;name&gt; = HEAD hash"]
    end

    subgraph CheckoutCmd ["minigit checkout &lt;target&gt;"]
        C1["isDirty()? (staged / modified / deleted)"] --> C2{"Working copy clean?"}
        C2 -- No --> C_Err["Throw CheckoutException (uncommitted changes)"]
        C2 -- Yes --> C3{"Is target a branch name?"}
        C3 -- Yes --> C4["dest = branchTip(&lt;target&gt;)"]
        C3 -- No --> C5["dest = resolvePrefix(&lt;target_hash&gt;)"]
        C4 --> C6["loadAs&lt;Commit&gt;(dest)"]
        C5 --> C6
        C6 --> C7["restoreCommit(dest): overwrite working files with target Tree, update index"]
        C7 --> C8{"Was branch?"}
        C8 -- Yes --> C9["HEAD = 'ref: refs/heads/&lt;name&gt;'"]
        C8 -- No --> C10["HEAD = dest (Detached HEAD)"]
    end
```

---

### 6.7 Flowchart: `minigit merge <branch>` / `minigit merge --abort`

```mermaid
flowchart TD
    A["minigit merge branch"] --> B{"HEAD detached?"}
    B -- Yes --> E1["Throw MergeException"]
    B -- No --> MH{"MERGE_HEAD exists?"}
    MH -- Yes --> E0["Throw MergeException (merge already in progress)"]
    MH -- No --> C{"Branch exists?"}
    C -- No --> E2["Throw BranchException"]
    C -- Yes --> D{"Working copy dirty?"}
    D -- Yes --> E3["Throw MergeException (uncommitted changes)"]
    D -- No --> F["cur = current tip, tgt = branch tip<br/>(either empty: MergeException)"]
    F --> G{"cur == tgt OR tgt is ancestor of cur?"}
    G -- Yes --> H["'Already up to date.'"]
    G -- No --> I{"cur is ancestor of tgt?"}
    I -- Yes --> J["Untracked-overwrite check<br/>restoreCommit(tgt), updateHead(tgt)<br/>reflog 'merge'<br/>'Fast-forward to abc1234'"]
    I -- No --> K["base = findMergeBase(cur, tgt)"]
    K --> L["For each path in base, ours, theirs:<br/>B, O, T = blob hashes"]
    L --> R{"Decide per file"}
    R -- "O == T" --> K1["keep"]
    R -- "O == B" --> K2["take theirs (or delete)"]
    R -- "T == B" --> K3["keep ours"]
    R -- "all differ" --> K4["conflict"]
    K1 & K2 & K3 & K4 --> Q{"Any conflicts?"}
    Q -- No --> M["Write files, update index,<br/>save Tree + Commit(parent=cur, parent2=tgt),<br/>updateHead, reflog 'merge'<br/>'Merged branch (3-way)'"]
    Q -- Yes --> X["Apply clean changes<br/>text: write conflict markers<br/>binary: keep ours<br/>write MERGE_HEAD = tgt"]
    X --> XE["Throw MergeException<br/>(Automatic merge failed ... Conflicted files: ...)<br/>Web Studio replies 409"]
    XE --> U{"User"}
    U -- "edit, add, commit" --> UC["commit() uses MERGE_HEAD as parent2"]
    U -- "merge --abort" --> UA["Delete MERGE_HEAD<br/>restoreCommit(HEAD)<br/>'Merge aborted.'"]
```

Conflict markers in a text file:

```text
<<<<<<< ours
our version
=======
their version
>>>>>>> feature
```

- The merge base is found with a breadth-first search over `parent` and `parent2` links.
- `merge --abort` fails with `no merge in progress to abort` when there is no `MERGE_HEAD`.

---

## 7. Object Graph Evolution

Commit trees represent full snapshots. Unchanged files reuse existing blob hashes:

```mermaid
flowchart LR
    subgraph Commit_A ["Commit A (Initial)"]
        CA["Commit A"] --> TA["Tree A"]
        TA --> BA1["Blob a.txt (v1)"]
    end

    subgraph Commit_B ["Commit B (Modified a.txt)"]
        CB["Commit B"] --> TB["Tree B"]
        TB --> BA2["Blob a.txt (v2)"]
        CB -->|parent| CA
    end

    subgraph Commit_C ["Commit C (Added f.txt on feature)"]
        CC["Commit C"] --> TC["Tree C"]
        TC --> BA2_shared["Blob a.txt (v2) [Shared]"]
        TC --> BF["Blob f.txt"]
        CC -->|parent| CB
    end
```

---

## 8. OOP & C++ Concept Map

| Concept | Implementation in Mini-Git |
|---|---|
| **Encapsulation** | Strict private members with accessors in `Commit`, `Hash`, `Index`, `RefManager`, and `Repository`. |
| **Inheritance** | Polymorphic hierarchies: `Object` (`Blob`, `Tree`, `Commit`), `Command` (12 concrete commands), and `MiniGitException`. |
| **Polymorphism** | Dynamic dispatch via pure virtual methods (`execute()`, `serialize()`, `type()`, `category()`). |
| **Abstract Classes** | `Object` and `Command` enforce interface contracts via pure virtual methods (`= 0`). |
| **Templates** | Generic LCS algorithm `DiffEngine<T>` and type-safe object loading `ObjectStore::loadAs<T>()`. |
| **Exception Safety** | Structured error propagation throwing domain exceptions in the backend, caught once in `CLI::run`. |
| **Operator Overloading** | Canonical relational operators (`==`, `!=`, `<`) on `Hash` and stream output `<<` for `Hash` and `Commit`. |
| **Modern Memory Management** | Zero raw pointers/manual deletions; strict RAII utilizing `std::unique_ptr` throughout. |
| **Modern File I/O** | Portable filesystem manipulation using `std::filesystem` and robust stream utilities. |
| **Design Patterns** | **Command Pattern** (`CLI` & `Command`), **Facade Pattern** (`Repository`), and **Factory Pattern** (`Object::deserialize`). |

---

## 9. Second Frontend: Web Studio (`minigit html`, `minigit ui`)

Both commands use the same embedded page (`templates/frontend.html`, compiled into `include/FrontendTemplate.h`) and the same backend.

```mermaid
flowchart TD
    subgraph Static ["minigit html [--no-open]"]
        H1["HtmlCmd::execute()"] --> H2["Repository::snapshot()"]
        H2 --> H3["HtmlExporter::render(): snapshot JSON injected into page"]
        H3 --> H4["Write .minigit/report.html, open browser"]
    end

    subgraph Live ["minigit ui / serve [--port N] [--no-open] [--lan]"]
        U1["UiCmd::execute()"] --> U2["HttpServer::run(repo, port, open, lan)"]
        U2 --> U3["Bind 127.0.0.1 (0.0.0.0 with --lan)<br/>try port .. port+29"]
        U3 --> U4["Accept loop: parse request, route"]
        U4 --> U5["GET / : rendered page"]
        U4 --> U6["/api/* : call Repository,<br/>reply with JSON + fresh snapshot"]
        U4 --> U7["/api/pair* : PairingManager"]
        U4 --> U8["/api/sync/* : exportBundleJson / importBundleJson"]
        U4 --> U9["/api/git/* : exportToGit / importFromGit"]
    end

    subgraph Browser ["Browser page"]
        B1["Files, editor, staging, commit"]
        B2["Branches, merge, revert, undo"]
        B3["Stash, reflog, time travel"]
        B4["Commit graph, health banner"]
        B5["Pair panel, pull / push, Git Bridge"]
    end

    H4 --> Browser
    U5 --> Browser
    Browser -->|"fetch, poll /api/snapshot every 2.5 s"| U6
```

**REST API (`HttpServer.cpp`)**

| Area | Endpoints |
|---|---|
| Page and state | `GET /`, `GET /api/snapshot` |
| Working tree | `POST /api/file/write`, `/api/stage`, `/api/unstage`, `/api/discard`, `/api/commit` |
| Branches and history | `POST /api/branch/create`, `/api/branch/delete`, `/api/checkout`, `/api/merge`, `/api/revert`, `/api/undo` |
| Stash and reflog | `POST /api/stash/save`, `/api/stash/pop`, `GET /api/stash/list`, `GET /api/reflog`, `POST /api/reflog/restore` |
| Pairing | `GET /api/token`, `GET /api/pair/code`, `POST /api/pair/generate-code`, `POST /api/pair`, `GET /api/pair/status`, `GET /api/pair/pending`, `POST /api/pair/decide`, `GET /api/pair/peers`, `POST /api/pair/revoke` |
| Sync | `GET /api/sync/bundle`, `POST /api/sync/import-bundle` |
| Git Bridge | `GET /api/git/status`, `POST /api/git/export`, `POST /api/git/import` |

- **Static vs live:** `report.html` is a read-only snapshot taken when the command ran; it lives inside `.minigit/` so it is never untracked. `minigit ui` is live and every action goes through `Repository`.
- **Refresh:** the page polls `/api/snapshot` every 2.5 s; the host's pending pair requests are polled every 1.5 s.
- **Commit graph:** a single column of commits sorted by time, with branch labels. It does not draw parent edges or one lane per branch.
- **Network access:** loopback clients can use every endpoint. Non-loopback clients (only possible with `--lan`) can reach only `/api/pair`, `/api/pair/status`, `/api/sync/bundle` and `/api/sync/import-bundle` (anything else returns 403); the sync endpoints also need a valid `X-MiniGit-Token` (401 otherwise).
- **Pairing:** 6-digit code, valid 300 s, single use, locked after 5 wrong attempts; host approval issues a 64-hex session token; peers can be revoked.
- **Hashing:** real repositories are always hashed by the C++ SHA-1 `Hasher`. Only the offline demo mode (opening `templates/frontend.html` directly) simulates a repository in the browser, using SHA-256 from the Web Crypto API.
- **Styling** loads from CDNs (Tailwind, Font Awesome, Google Fonts).

---

## 10. More Diagrams

Each diagram below has its own page in [`docs/diagrams/`](docs/diagrams/README.md), with a PNG copy in `docs/diagrams/png/`.

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
