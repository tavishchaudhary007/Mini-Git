# Sequence Diagram: LAN Pairing and Sync

Pairing a teammate with a 6-digit code and host approval, then pulling the host's repository. Push is the same in reverse.

```mermaid
sequenceDiagram
    actor T as Teammate browser
    participant HS as Host HttpServer (--lan)
    participant PM as PairingManager
    actor HO as Host browser (localhost)
    participant HR as Host Repository
    participant TS as Teammate local server
    participant TR as Teammate Repository

    Note over PM: constructor generates 6-digit code<br/>(valid 300 s)
    HO->>HS: GET /api/pair/code
    HS-->>HO: code, seconds left, LAN URL
    Note over HO,T: host tells teammate the URL and code

    T->>HS: POST /api/pair {code, peerName}
    HS->>PM: submitCode(code, ip, name)
    alt locked / used / expired / wrong (5 wrong attempts lock)
        PM-->>HS: error
        HS-->>T: 403 if locked, else 401
    else correct
        PM-->>HS: requestId (PENDING)
        HS-->>T: requestId
    end

    loop every 1.5 s
        HO->>HS: GET /api/pair/pending
        HS->>PM: pendingRequests() (PENDING, under 120 s old)
    end
    HO->>HS: POST /api/pair/decide {id, allow: true}
    HS->>PM: decideRequest()
    PM->>PM: code marked used, 64-hex token, add PairedPeer

    loop until approved or denied
        T->>HS: GET /api/pair/status?requestId=...
    end
    HS-->>T: APPROVED + token
    T->>T: store token in localStorage

    Note over T,TR: Pull
    T->>HS: GET /api/sync/bundle (X-MiniGit-Token)
    HS->>PM: isValidSessionToken()
    HS->>HR: exportBundleJson()
    HS-->>T: bundle JSON (objects, branches, base64 binaries)
    T->>TS: POST /api/sync/import-bundle
    TS->>TR: importBundleJson()
    TR->>TR: verify every object hash (any mismatch: write nothing)
    TR->>TR: save blobs, trees, commits in order
    TR->>TR: per branch: create / fast-forward / up to date / peer-<branch>
    TS-->>T: result + new snapshot

    Note over T,HR: Push is the reverse: teammate's bundle is POSTed to the host's /api/sync/import-bundle with the token
```

Image version: [png/sequence-pairing-sync.png](png/sequence-pairing-sync.png)

[Back to all diagrams](README.md)
