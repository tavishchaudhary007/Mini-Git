# Deployment Diagram

What runs where. Without `--lan` the server listens only on 127.0.0.1. With `--lan`, another machine can reach only the pairing and sync endpoints, and sync needs the session token.

```mermaid
flowchart LR
    subgraph HOST["Host machine: minigit ui --lan"]
        direction TB
        HB["Host browser"]
        subgraph PROC["minigit process"]
            S["HttpServer<br/>bound to 0.0.0.0:8080<br/>(127.0.0.1 without --lan)"]
            PM["PairingManager<br/>(in memory)"]
            R["Repository"]
        end
        D[(".minigit/")]
        HB -->|"loopback: every endpoint"| S
        S --- PM
        S --> R --> D
    end

    subgraph PEER["Teammate machine: minigit ui"]
        direction TB
        TB["Teammate browser"]
        TS["Own HttpServer<br/>127.0.0.1:8080"]
        TD[(".minigit/")]
        TB --> TS --> TD
    end

    TB -->|"POST /api/pair<br/>GET /api/pair/status"| S
    TB -->|"GET /api/sync/bundle<br/>POST /api/sync/import-bundle<br/>header X-MiniGit-Token"| S
    TB -.->|"any other path: 403<br/>sync without token: 401"| S
```

Image version: [png/deployment.png](png/deployment.png)

[Back to all diagrams](README.md)
