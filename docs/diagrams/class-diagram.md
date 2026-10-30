# Class Diagram (UML)

Every main class with its key members. Filled diamonds are composition (a part owned by its whole), hollow triangles are inheritance, dotted arrows are dependencies. Static members are underlined, abstract methods are in italics.

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
    class UndoCmd
    class RevertCmd
    class StashCmd
    class ReflogCmd
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
        +createBranch(name, at)
        +deleteBranch(name)
        +branches() vector~string~
        +currentBranch() string
        +head() Hash
        +branchTip(name) Hash
        +branchExists(name) bool
        +checkout(target)
        +findMergeBase(h1, h2) Hash
        +merge(branch) string
        +mergeAbort() string
        +isMerging() bool
        +conflictedPaths() vector~string~
        +undo() string
        +stashSave(msg, untracked) string
        +stashPop() string
        +hasStash() bool
        +stashList() vector~StashEntry~
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
        -time_point codeCreatedAt_
        -int failedAttempts_
        -map~string, int~ failedAttemptsByClient_
        -vector~time_point~ failedAttemptTimestamps_
        -bool isLocked_
        -bool isUsed_
        -bool isInvalidated_
        -int currentCodeFailures_
        -int expirationSeconds_
        -map~string, PendingPairRequest~ pendingRequests_
        -vector~PairedPeer~ pairedPeers_
        +generateNewCode() string
        +currentCode() string
        +remainingSeconds() int
        +isExpired() bool
        +isInvalidated() bool
        +isLocked() bool
        +isClientLocked(clientKey) bool
        +lockedClientsCount() int
        +isUsed() bool
        +failedAttempts(clientKey) int
        +submitCode(code, ip, name, origin, isCrossLoopback) PairSubmitResult
        +pendingRequests() vector~PendingPairRequest~
        +getRequestStatus(requestId) PendingPairRequest
        +decideRequest(id, allow, label) bool
        +isValidSessionToken(token) bool
        +listPeers() vector~PairedPeer~
        +revokePeer(idOrToken) bool
        +generate32ByteSessionToken()$ string
        +generate6DigitCode()$ string
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

    CLI *-- "16" Command : owns
    Command <|-- InitCmd
    Command <|-- AddCmd
    Command <|-- CommitCmd
    Command <|-- LogCmd
    Command <|-- StatusCmd
    Command <|-- DiffCmd
    Command <|-- BranchCmd
    Command <|-- CheckoutCmd
    Command <|-- MergeCmd
    Command <|-- UndoCmd
    Command <|-- RevertCmd
    Command <|-- StashCmd
    Command <|-- ReflogCmd
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

Image version: [png/class-diagram.png](png/class-diagram.png)

[Back to all diagrams](README.md)
