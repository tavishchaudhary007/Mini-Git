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
   - [merge](#67-flowchart-minigit-merge-branch)
7. [Object Graph Evolution](#7-object-graph-evolution)
8. [OOP & C++ Concept Map](#8-oop--c-concept-map)
9. [Second Frontend: Visual Web Studio (`minigit html`)](#9-second-frontend-visual-web-studio-minigit-html)
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
    B -- Yes --> ErrMsg["Usage Error"]
    B -- No --> C{"Index empty?"}
    C -- Yes --> ErrEmpty["Throw NothingToCommitException"]
    C -- No --> D["Build Tree object from index entries"]
    D --> E{"HEAD has parent commit?"}

    E -- Yes --> F{"parent.tree == new tree hash?"}
    F -- Yes --> ErrNoChange["Throw NothingToCommitException (no staged changes)"]
    F -- No --> G["store.save(tree)"]
    E -- No --> G

    G --> H["Create Commit(tree, parent, author, timestamp, message)"]
    H --> I["store.save(commit)"]
    I --> J["refs.updateHead(commitHash)"]
    J --> K{"On branch or detached?"}
    K -- On Branch --> L["Write refs/heads/&lt;branch&gt; = commitHash"]
    K -- Detached --> M["Write HEAD = commitHash"]
    L --> Out["Print [branch shorthash] msg"]
    M --> Out
```

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

### 6.7 Flowchart: `minigit merge <branch>`

```mermaid
flowchart TD
    A["minigit merge &lt;branch&gt;"] --> B{"HEAD detached?"}
    B -- Yes --> E1["Throw MergeException"]
    B -- No --> C{"Branch exists?"}
    C -- No --> E2["Throw BranchException"]
    C -- Yes --> D{"Working copy dirty?"}
    D -- Yes --> E3["Throw MergeException"]
    D -- No --> F["cur = current tip, tgt = target branch tip"]
    F --> G{"cur == tgt OR tgt is ancestor of cur?"}
    G -- Yes --> H["Print 'Already up to date.'"]
    G -- No --> I{"cur is ancestor of tgt?"}
    I -- Yes --> J["Fast-Forward: restoreCommit(tgt), move current branch pointer to tgt"]
    I -- No --> K["Throw MergeException ('branches have diverged; only fast-forward supported')"]
```

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

## 9. Second Frontend: Visual Web Studio (`minigit html`)

Mini-Git provides two frontends sharing the same backend:

```mermaid
flowchart TD
    User["User runs: minigit html [--no-open]"] --> CLI["CLI -> HtmlCmd::execute()"]
    CLI --> Snap["Repository::snapshot()"]
    Snap --> Exporter["HtmlExporter::toJson(snapshot)"]
    Exporter --> Template["Inject into embedded HTML (FrontendTemplate.h)"]
    Template --> WriteFile["Write .minigit/report.html"]
    WriteFile --> Browser["Launch default OS browser (Windows: start, Mac: open, Linux: xdg-open)"]

    subgraph Dashboard ["Liquid Glass Web Dashboard"]
        VFS["In-memory Virtual Filesystem"]
        TreeViewer["Interactive Working Directory Explorer"]
        DiffViewer["Side-by-Side Unified Diff Visualizer"]
        DAGViewer["Interactive Commit Graph (DAG)"]
        Console["In-Browser CLI Console"]
    end

    Browser --> Dashboard
```

- **Live Snapshot**: The export writes directly inside `.minigit/report.html`, ensuring it never appears as an untracked file in the repository.
- **Apple Liquid Glass Interface**: Styled with frosted glass layers (`backdrop-blur-28px`), tactile segmented pill switchers, and an atmospheric sky backdrop.
- **Client-Side SHA-1 Verification**: File statuses are verified in the browser using the Web Crypto API, matching the C++ backend.

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
