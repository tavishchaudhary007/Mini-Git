#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <map>

struct PairedPeer {
    std::string id;           // Unique identifier for the paired peer
    std::string sessionToken; // 32 random bytes (64 hex characters)
    std::string clientIp;     // Peer IPv4 address
    std::string peerName;     // Optional user-friendly peer label
    std::string pairedAt;     // ISO or formatted timestamp
};

struct PendingPairRequest {
    std::string requestId;    // e.g. "req_1a2b3c..."
    std::string clientIp;     // Requesting IPv4
    std::string peerName;     // Device / browser name
    std::string code;         // Code entered
    std::chrono::steady_clock::time_point requestedAt;
    enum Status { PENDING, APPROVED, DENIED } status = PENDING;
    std::string sessionToken; // Populated when APPROVED
};

class PairingManager {
public:
    PairingManager();

    // 6-digit pair code lifecycle
    std::string generateNewCode();
    std::string currentCode() const;
    int remainingSeconds() const;
    bool isExpired() const;
    bool isLocked() const;
    bool isUsed() const;
    int failedAttempts() const;

    // Submitting a pair code from a peer
    struct PairSubmitResult {
        bool success;
        bool locked;
        int remainingAttempts;
        std::string requestId;
        std::string error;
    };
    PairSubmitResult submitCode(const std::string& code, const std::string& clientIp, const std::string& peerName = "");

    // Managing pending requests (Host Allow/Deny)
    std::vector<PendingPairRequest> pendingRequests() const;
    PendingPairRequest getRequestStatus(const std::string& requestId) const;
    bool decideRequest(const std::string& requestId, bool allow, const std::string& customLabel = "");

    // Session token validation & paired-peers list with Revoke
    bool isValidSessionToken(const std::string& token) const;
    std::vector<PairedPeer> listPeers() const;
    bool revokePeer(const std::string& peerIdOrToken);

    // Testing and configuration helpers
    void setExpirationSeconds(int sec) { expirationSeconds_ = sec; }

    static std::string generate32ByteSessionToken();
    static std::string generate6DigitCode();

private:
    std::string currentCode_;
    std::chrono::steady_clock::time_point codeCreatedAt_;
    int failedAttempts_ = 0;
    bool isLocked_ = false;
    bool isUsed_ = false;
    int expirationSeconds_ = 300; // 5 minutes

    std::map<std::string, PendingPairRequest> pendingRequests_;
    std::vector<PairedPeer> pairedPeers_;
};
