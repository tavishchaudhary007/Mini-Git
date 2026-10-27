# Sequence Diagram: Commit

The calls made when the user runs `minigit commit -m "msg"`, from `main` down to the object store and refs.

```mermaid
sequenceDiagram
    actor U as User
    participant M as main.cpp / CLI
    participant C as CommitCmd
    participant R as Repository
    participant I as Index
    participant S as ObjectStore
    participant H as Hasher
    participant F as RefManager

    U->>M: minigit commit -m "msg"
    M->>M: look up "commit" in commands_ map
    M->>R: findRoot(cwd), construct Repository
    M->>C: execute(repo, args)
    C->>R: commit("msg")
    R->>I: entries()
    alt index empty
        R-->>C: throw NothingToCommitException
    end
    R->>R: build Tree from index entries
    R->>F: headCommit()  (parent)
    R->>R: read MERGE_HEAD if present (parent2)
    R->>S: loadAs<Commit>(parent)
    alt tree hash equals parent's tree and no parent2
        R-->>C: throw NothingToCommitException
    end
    R->>S: save(tree)
    S->>H: sha1(raw)
    S-->>R: tree hash (file written only if new)
    R->>S: save(Commit(tree, parent, author, msg, time, parent2))
    S-->>R: commit hash
    R->>F: updateHead(hash)  (moves branch or detached HEAD)
    R->>R: remove MERGE_HEAD, recordReflog("commit" / "merge")
    R-->>C: hash
    C-->>U: [main abc1234] msg
```

Image version: [png/sequence-commit.png](png/sequence-commit.png)

[Back to all diagrams](README.md)
