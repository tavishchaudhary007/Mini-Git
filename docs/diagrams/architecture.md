# Architecture Diagram

The three layers of Mini-Git: two front ends (command line and Web Studio) call one facade, `Repository`, which uses the storage classes and services that read and write `.minigit/`.

```mermaid
flowchart TB
    User(["Developer"])
    Browser["Browser<br/>Web Studio page (frontend.html)"]
    Terminal["Terminal"]

    subgraph FE["Layer 1: Front ends"]
        direction LR
        subgraph CLIF["Command line"]
            Main["main.cpp"] --> CLI["CLI<br/>command registry"] --> Cmds["16 Command classes<br/>Commands.cpp"]
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
    Browser <-->|"HTTP JSON API<br/>ETag / 304 polling"| Server
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

Image version: [png/architecture.png](png/architecture.png)

[Back to all diagrams](README.md)
