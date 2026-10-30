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

    Note over PM: CSPRNG generates 6-digit pair code<br/>(valid 300 s, single use)
    HO->>HS: GET /api/pair/code
    HS-->>HO: code, seconds left, LAN URL
    Note over HO,T: Host shares LAN address and 6-digit code with teammate

    T->>HS: POST /api/pair {code, peerName} (Origin, IP)
    alt Host not in --lan mode or cross-origin over loopback
        HS-->>T: 403 Forbidden
    else Global rate limit exceeded (>= 10 fails in 60s across clients)
        HS-->>T: 429 Too Many Requests (rate limited)
    else Client locked (>= 5 failed attempts for this client IP)
        HS-->>T: 403 Forbidden (pairing locked)
    else Code invalidated (>= 10 total code failures)
        HS-->>T: 401 Unauthorized (code invalidated, new code needed)
    else Code expired or already used
        HS-->>T: 401 Unauthorized (expired / used)
    else Wrong code (< 5 attempts)
        HS-->>T: 401 Unauthorized (remaining attempts shown)
    else Correct code & unlocked client
        HS->>PM: submitCode(code, ip, peerName, origin)
        PM-->>HS: requestId (status: PENDING)
        HS-->>T: 200 OK {status: "pending", requestId}
    end

    loop every 1.5 s
        HO->>HS: GET /api/pair/pending
        HS->>PM: pendingRequests() (PENDING, < 120 s old)
    end
    Note over HO: Approval modal displays requester IP,<br/>Origin, claimed name, and entered code
    HO->>HS: POST /api/pair/decide {requestId, allow: true, customLabel}
    HS->>PM: decideRequest()
    PM->>PM: code marked used (single use),<br/>CSPRNG generates 32-byte session token (64 hex),<br/>records PairedPeer

    loop until approved or denied
        T->>HS: GET /api/pair/status?requestId=...
    end
    HS-->>T: 200 OK {status: "approved", token: "64-hex"}
    T->>T: Store token in localStorage

    Note over T,TR: Pull
    T->>HS: GET /api/sync/bundle (X-MiniGit-Token: token)
    HS->>PM: isValidSessionToken(token)
    HS->>HR: exportBundleJson()
    HS-->>T: bundle JSON (objects, branches, base64 binaries)
    T->>TS: POST /api/sync/import-bundle
    TS->>TR: importBundleJson()
    TR->>TR: verify every object SHA-1 hash (mismatch rejects bundle, refs untouched)
    TR->>TR: save blobs, trees, commits in dependency order
    TR->>TR: per branch: create / fast-forward / up to date / diverged kept as peer-<branch>
    TS-->>T: result + new snapshot

    Note over T,HR: Push: Teammate exports bundle, POSTs to Host /api/sync/import-bundle with X-MiniGit-Token
```

Image version: [png/sequence-pairing-sync.png](png/sequence-pairing-sync.png)

[Back to all diagrams](README.md)
