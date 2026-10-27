# Mini-Git Diagrams

All diagrams are written in Mermaid, which GitHub draws automatically. A PNG copy of each is in [`png/`](png/) for reports and slides.

| Diagram | What it shows |
|---|---|
| [Architecture Diagram](architecture.md) | The three layers of Mini-Git: two front ends (command line and Web Studio) call one facade, `Repository`, which uses the storage classes and services that read and write `.minigit/`. |
| [Class Diagram (UML)](class-diagram.md) | Every main class with its key members. |
| [Exception Hierarchy](exceptions.md) | All Mini-Git errors derive from `MiniGitException` (itself a `std::runtime_error`). |
| [Object Model](object-model.md) | How stored objects point to each other: HEAD names a branch, a branch names a commit, a commit names its tree and parent(s), a tree maps paths to blobs. |
| [Sequence Diagram: Commit](sequence-commit.md) | The calls made when the user runs `minigit commit -m "msg"`, from `main` down to the object store and refs. |
| [Activity Diagram: Merge](activity-merge.md) | How `Repository::merge` decides between up to date, fast-forward and a three-way merge, how each file is resolved using base (B), ours (O) and theirs (T), and how conflicts are finished or aborted. |
| [Sequence Diagram: LAN Pairing and Sync](sequence-pairing-sync.md) | Pairing a teammate with a 6-digit code and host approval, then pulling the host's repository. |
| [Deployment Diagram](deployment.md) | What runs where. |
| [Use-Case Diagram](use-case.md) | What the developer and a LAN teammate can do with Mini-Git. |
| [State Diagrams](state-diagram.md) | Left: the life cycle of a file (untracked, staged, committed, modified, deleted). |

The full design write-up, with command flowcharts and the OOP concept map, is in [architecture.md](../../architecture.md).
