# Object Model

How stored objects point to each other: HEAD names a branch, a branch names a commit, a commit names its tree and parent(s), a tree maps paths to blobs. Identical content (blob b1, b3) is stored once and shared. c4 is a merge commit with `parent2`.

```mermaid
flowchart LR
    HEAD["HEAD<br/>ref: refs/heads/main"] --> MAIN["refs/heads/main"]
    FEAT["refs/heads/feature"] --> C3
    MAIN --> C4

    C4["Commit c4 (merge)<br/>tree t4<br/>parent c2<br/>parent2 c3"]
    C3["Commit c3<br/>tree t3<br/>parent c1"]
    C2["Commit c2<br/>tree t2<br/>parent c1"]
    C1["Commit c1<br/>tree t1<br/>parent -"]

    C4 -->|parent| C2
    C4 -->|parent2| C3
    C3 -->|parent| C1
    C2 -->|parent| C1

    C1 --> T1["Tree t1<br/>a.txt to b1"]
    C2 --> T2["Tree t2<br/>a.txt to b2"]
    C3 --> T3["Tree t3<br/>a.txt to b1<br/>src/f.txt to b3"]
    C4 --> T4["Tree t4<br/>a.txt to b2<br/>src/f.txt to b3"]

    T1 --> B1[("Blob b1")]
    T2 --> B2[("Blob b2")]
    T3 --> B1
    T3 --> B3[("Blob b3")]
    T4 --> B2
    T4 --> B3
```

Image version: [png/object-model.png](png/object-model.png)

[Back to all diagrams](README.md)
