# Mini-Git — Architecture Guide
> **C++17 command-line version control system**

---

## 1. Layered Overview

```text
User types:  minigit commit -m "msg"
      |
      v
+--------------------------------------------------------------+
| FRONTEND                                                     |
|   main.cpp -> CLI -> Command (abstract)                      |
|                      InitCmd AddCmd CommitCmd LogCmd         |
|                      StatusCmd DiffCmd BranchCmd             |
|                      CheckoutCmd MergeCmd                    |
|   (parses args, prints output, catches exceptions)           |
+------------------------------+-------------------------------+
                               |  calls
                               v
+--------------------------------------------------------------+
| BACKEND FACADE                                               |
|   Repository  (all logic, never prints)                      |
|     owns:  ObjectStore  Index  RefManager      (composition) |
|     uses:  DiffEngine<T>  Hasher                             |
+------------------------------+-------------------------------+
                               |  reads / writes
                               v
+--------------------------------------------------------------+
| STORAGE (files on disk, .minigit/)                           |
|   objects/<sha1>   refs/heads/<branch>   HEAD   index        |
+--------------------------------------------------------------+
```

---

## 2. Class Responsibilities

| Class / Component | Type | Responsibility |
| :--- | :--- | :--- |
| `Hash` | Value Type | Hex digest representation; overloads `==`, `!=`, `<`, `<<` |
| `Hasher` | Utility | Static `sha1(string) -> hex` (custom hand-written SHA-1 implementation) |
| `Object` | Abstract Base | Defines `type()`, `serialize()`, `hash()`, and static `deserialize()` factory |
| `Blob` | Derived Object | Encapsulates raw file content |
| `Tree` | Derived Object | `std::map<path, Hash>` snapshot of tracked repository files |
| `Commit` | Derived Object | Tree hash, parent commit, author, timestamp, commit message; overloads `==`, `<<` |
| `ObjectStore` | Subsystem | Content-addressed object store (`save`, `load`); templated `loadAs<T>()`; `resolvePrefix()` for short hashes |
| `Index` | Subsystem | Staging area: `std::map<path, Hash>`, serialization to disk |
| `RefManager` | Subsystem | HEAD pointer and branch references (handles attached and detached HEAD) |
| `DiffEngine<T>` | Template | Longest Common Subsequence (LCS) diff algorithm yielding `DiffOp{Keep, Add, Remove}` |
| `Repository` | Facade | Central engine coordinating init, add, commit, log, status, diff, branch, checkout, and merge |
| `Status` | Data Struct | Aggregates status lists: staged, modified, deleted, and untracked files |
| `Command` | Abstract Base | Command pattern interface: `name()`, `usage()`, `needsRepo()`, and `execute()` |
| `CLI` | Dispatcher | Command registry (`std::map<string, unique_ptr<Command>>`) and execution runner `run(argc, argv)` |

### Exception Hierarchy

```text
std::exception
  └── std::runtime_error
        └── MiniGitException (base with virtual category())
              ├── NotARepoException
              ├── FileNotFoundException
              ├── InvalidObjectException
              ├── BranchException
              ├── NothingToCommitException
              ├── CheckoutException
              └── MergeException
```

---

## 3. Class Relationships & Design Patterns

- **Inheritance (`--\|>`)**:
  - `Blob`, `Tree`, `Commit` inherit from `Object`
  - 9 concrete command classes inherit from `Command`
  - 7 custom exceptions inherit from `MiniGitException` (which inherits from `std::runtime_error`)
- **Composition (`*--`)**:
  - `Repository` composes `ObjectStore`, `Index`, and `RefManager`
  - `CLI` owns instances of `Command` via `std::unique_ptr`
- **Association (`-->`)**:
  - `Tree` associates with `Hash` (blob hashes)
  - `Commit` associates with `Hash` (tree hash and parent commit hash)
  - `Index` associates with `Hash`
- **Dependency (`..>`)**:
  - `Repository` depends on `DiffEngine<std::string>`, `Blob`, `Tree`, and `Commit`
  - `Object` depends on `Hasher`
  - `ObjectStore` creates `Object` instances via static factory
- **Polymorphism**:
  - Dynamic dispatch on `Object*` (`serialize()`, `type()`)
  - Dynamic dispatch on `Command*` (`execute()`)
  - Dynamic dispatch on `MiniGitException&` (`category()`)

---

## 4. On-Disk Layout

```text
.minigit/
|-- HEAD                 "ref: refs/heads/main"   (or raw hash if detached)
|-- index                "<blobhash> <path>" one per line
|-- refs/heads/
|     |-- main           latest commit hash on main
|     '-- feature        latest commit hash on feature
'-- objects/
      |-- 7f59f3...      blob   : "blob\n<file content>"
      |-- a91c02...      tree   : "tree\n<hash> <path>\n..."
      '-- e1dfc6...      commit : "commit\ntree ..\nparent ..\nauthor ..\ntime ..\n\n<message>"
```

> **Note**: Object ID is the SHA-1 digest of the stored text (`<type>\n<body>`).

---

## 5. Top-Level Control Flow (Every Command)

```text
 main()
   |
   v
 CLI::run(argc, argv)
   |
   v
 argc < 2 ? ----yes----> print usage, return 1
   |no
   v
 command in registry? --no--> "unknown command", return 1
   |yes
   v
 +------------------- try ---------------------+
 | needsRepo()?                                |
 |   yes: Repository::findRoot(cwd)            |
 |          walks up looking for .minigit      |
 |          not found -> throw NotARepo        |
 |   no : (init) skip                          |
 | command->execute(repo, args)                |
 +----------------------+----------------------+
                        |
      +-----------------+------------------+
      |                 |                  |
   success      MiniGitException      std::exception
   return 0     "error [category]:    "fatal: ..."
                 msg"  return 1        return 2
```

---

## 6. Flowchart: `init`

```text
 minigit init
      |
      v
 .minigit exists? --yes--> throw "already initialized"
      |no
      v
 create .minigit/objects
 create .minigit/refs/heads
 write HEAD = "ref: refs/heads/main"
      |
      v
 print "Initialized empty Mini-Git repository"
```

---

## 7. Flowchart: `add <path>`

```text
 minigit add <path>
      |
      v
 relPath(path)  (must be inside repo root, else throw)
      |
      v
 +--------- what is path? ---------+
 |                                 |
directory                   regular file            missing
 |                              |                     |
 v                              v                     v
workingFiles() under       read file bytes        in index?
prefix (skip .minigit)     Blob(content)          yes: remove from index
 |                        store.save(blob)             (stage deletion)
 v                        index.set(path, hash)  no : throw FileNotFound
for each file: addFile          |
tracked files now gone          |
from disk: remove from index    |
 |                              |
 '--------------+---------------'
                v
          index.save()
```

---

## 8. Flowchart: `commit -m "msg"`

```text
 minigit commit -m "msg"
      |
      v
 message empty? --yes--> usage error
      |no
      v
 index empty? --yes--> throw NothingToCommit
      |no
      v
 build Tree from index entries
      |
      v
 HEAD has parent commit?
      |yes
      v
 parent.tree == new tree hash? --yes--> throw NothingToCommit
      |no                                (no changes staged)
      v
 store.save(tree)
 commit = Commit(tree, parent, author, msg, now)
 store.save(commit)
      |
      v
 refs.updateHead(commitHash)
   on branch  -> write refs/heads/<branch>
   detached   -> write HEAD
      |
      v
 print "[branch shorthash] msg"
```

---

## 9. Flowchart: `status`

```text
 headTree  = files in last commit
 index     = staged files
 working   = files on disk (excluding .minigit)

 for each file in index:
      differs from headTree or new?  ----> STAGED
      missing on disk?               ----> DELETED
      disk hash != index hash?       ----> MODIFIED
 for each file in headTree not in index  -> STAGED (deleted)
 for each file on disk not in index      -> UNTRACKED
```

---

## 10. Flowchart: `diff [file]`

```text
 pick targets (one file or all in index)
      |
      v
 for each target:
      oldText = blob content from index hash
      newText = file on disk (empty if missing)
      same?  --yes--> skip
      |no
      v
 splitLines(old), splitLines(new)
      |
      v
 DiffEngine<string>::diff
   build LCS table dp[i][j]
   walk both lists: Keep / Remove / Add
      |
      v
 print "-N: line" for removed, "+N: line" for added
```

---

## 11. Flowchart: `branch` / `checkout`

### `branch <name>`
```text
 branch <name>
      |
      v
 HEAD commit exists? --no--> throw "no commits yet"
      |yes
      v
 name valid? --no--> throw BranchException
 exists?     --yes-> throw BranchException
      |
      v
 write refs/heads/<name> = HEAD commit
```

### `checkout <target>`
```text
 checkout <target>
      |
      v
 isDirty() (staged / modified / deleted)? --yes--> throw CheckoutException
      |no
      v
 target is a branch?
    yes: dest = branch tip           no: dest = resolvePrefix(hash)
      |                                   (unknown/ambiguous -> throw)
      '---------------+-------------------'
                      v
 load dest as Commit (validates it is a commit)
                      |
                      v
 restoreCommit(dest):
     delete tracked files not in target tree
     write every file of target tree to disk
     index = target tree
                      |
                      v
 target was branch? yes: HEAD -> "ref: refs/heads/<name>"
                    no : HEAD = raw hash (detached)
```

---

## 12. Flowchart: `merge <branch>` (Fast-Forward Only)

```text
 HEAD detached?           --yes--> throw MergeException
 branch missing?          --yes--> throw BranchException
 working tree dirty?      --yes--> throw MergeException
      |
      v
 cur = current tip, tgt = branch tip
      |
      v
 cur == tgt, or tgt is ancestor of cur? --yes--> "Already up to date."
      |no
      v
 cur is ancestor of tgt? 
      |yes                                   |no
      v                                      v
 restoreCommit(tgt)                     throw MergeException
 move current branch to tgt             "branches have diverged"
 "Fast-forward to <hash>"
```

---

## 13. Example: Commit History After a Session

```text
init; add a.txt; commit "A"; commit "B" (after edit);
branch feature; checkout feature; commit "C"; checkout main; merge feature

C (feature, main after merge)
|
B
|
A

Each commit -> tree -> blobs, linked only by hashes:

Commit C --tree--> Tree C --a.txt--> Blob(a.txt v2)
   |                      '--f.txt--> Blob(f.txt)
   '--parent--> Commit B --tree--> Tree B --a.txt--> Blob(a.txt v2)
```

---

## 14. OOP / C++ Concept Map

| Concept | Implementation in Mini-Git |
| :--- | :--- |
| **Encapsulation** | Private members + accessors (`Commit`, `Hash`, `Index`, `Repository`) |
| **Inheritance** | `Object`, `Command`, `MiniGitException` class hierarchies |
| **Polymorphism** | Virtual functions: `execute()`, `serialize()`, `type()`, `category()` |
| **Abstract Classes** | Pure virtual methods in `Object` and `Command` |
| **Templates** | `DiffEngine<T>`, `ObjectStore::loadAs<T>()` |
| **Exception Handling** | Exceptions thrown in backend subsystems, caught centrally in `CLI::run` |
| **Operator Overloading** | `Hash` (`==`, `!=`, `<`, `<<`), `Commit` (`==`, `<<`) |
| **Memory Management** | RAII with `std::unique_ptr`, no manual `new`/`delete` leaks |
| **File I/O** | `FileUtil.h`, `std::filesystem` directory iterations and file streams |
| **Design Patterns** | Command pattern (`Command`), Facade (`Repository`), Factory (`Object::deserialize`) |
| **Separation of Concerns** | Commands handle CLI I/O; `Repository` handles domain logic and returns data |
