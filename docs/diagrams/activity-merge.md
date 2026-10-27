# Activity Diagram: Merge

How `Repository::merge` decides between up to date, fast-forward and a three-way merge, how each file is resolved using base (B), ours (O) and theirs (T), and how conflicts are finished or aborted.

```mermaid
flowchart TD
    A(["minigit merge branch"]) --> P{"On a branch, branch exists,<br/>clean tree, no MERGE_HEAD,<br/>both sides have commits?"}
    P -- no --> E1["throw MergeException"]
    P -- yes --> Q{"tgt == cur, or<br/>tgt is ancestor of cur?"}
    Q -- yes --> U["Already up to date"]
    Q -- no --> FF{"cur is ancestor of tgt?"}
    FF -- yes --> F1["Untracked-overwrite check<br/>restoreCommit(tgt)<br/>updateHead(tgt)"] --> F2["Fast-forward (no new commit)"]
    FF -- no --> B["findMergeBase: BFS from ours,<br/>then BFS from theirs"]
    B --> L["For every path in base, ours, theirs<br/>compare hashes B, O, T"]
    L --> R1{"O == T?"}
    R1 -- yes --> K1["keep (same on both sides)"]
    R1 -- no --> R2{"O == B?"}
    R2 -- yes --> K2["take theirs (or delete)"]
    R2 -- no --> R3{"T == B?"}
    R3 -- yes --> K3["keep ours"]
    R3 -- no --> K4["CONFLICT for this file"]
    K1 & K2 & K3 & K4 --> C{"Any conflicts?"}
    C -- no --> M1["Write files, update index,<br/>save tree + Commit(parent=cur, parent2=tgt),<br/>updateHead"] --> M2["Merged (3-way)"]
    C -- yes --> X1["Apply clean changes<br/>text: write conflict markers<br/>binary: keep ours<br/>write MERGE_HEAD"] --> X2["throw MergeException<br/>(server replies 409)"]
    X2 --> Y{"User choice"}
    Y -- "fix files, add, commit" --> Y1["commit() reads MERGE_HEAD<br/>as parent2, deletes it"]
    Y -- "merge --abort" --> Y2["delete MERGE_HEAD<br/>restoreCommit(HEAD)"]
```

Image version: [png/activity-merge.png](png/activity-merge.png)

[Back to all diagrams](README.md)
