# State Diagrams

Left: the life cycle of a file (untracked, staged, committed, modified, deleted). Right: HEAD attached to a branch vs detached at a commit.

```mermaid
stateDiagram-v2
    direction LR
    state "File life cycle" as FL {
        [*] --> Untracked : create file
        Untracked --> Staged : add
        Staged --> Committed : commit
        Committed --> Modified : edit on disk
        Modified --> Staged : add
        Modified --> Committed : discard
        Staged --> Modified : unstage
        Committed --> Deleted : delete on disk
        Deleted --> Staged : add (stages deletion)
    }
    state "HEAD" as HD {
        [*] --> Attached : init (ref: refs/heads/main)
        Attached --> Detached : checkout a commit hash
        Detached --> Attached : checkout a branch
        Attached --> Attached : commit moves branch
        Detached --> Detached : commit moves HEAD only
    }
```

Image version: [png/state-diagram.png](png/state-diagram.png)

[Back to all diagrams](README.md)
