#include "PairingManager.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <cstdint>

namespace {

std::string generateRandomHex(size_t numChars) {
    static const char hexChars[] = "0123456789abcdef";
    std::random_device rd;
    std::string s;
    s.reserve(numChars);
    size_t i = 0;
    while (i < numChars) {
        unsigned int r = rd();
        for (size_t b = 0; b < sizeof(r) * 2 && i < numChars; ++b) {
            s += hexChars[(r >> (b * 4)) & 0x0F];
            i++;
        }
    }
    return s;
}

std::string currentTimestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    return std::string(buf);
}

} // namespace

PairingManager::PairingManager() {
    generateNewCode();
}

std::string PairingManager::generate6DigitCode() {
    std::random_device rd;
    const uint32_t range = 900000;
    const uint32_t limit = (0xFFFFFFFFu / range) * range;
    uint32_t val = 0;
    do {
        val = rd();
    } while (val >= limit);
    uint32_t code = 100000 + (val % range);
    return std::to_string(code);
}

std::string PairingManager::generate32ByteSessionToken() {
    // 32 random bytes represented as 64 hex characters
    return generateRandomHex(64);
}

std::string PairingManager::generateNewCode() {
    currentCode_ = generate6DigitCode();
    codeCreatedAt_ = std::chrono::steady_clock::now();
    failedAttempts_ = 0;
    failedAttemptsByClient_.clear();
    failedAttemptTimestamps_.clear();
    isLocked_ = false;
    isUsed_ = false;
    return currentCode_;
}

std::string PairingManager::currentCode() const {
    return currentCode_;
}

int PairingManager::remainingSeconds() const {
    if (isUsed_) return 0;
    auto now = std::chrono::steady_clock::now();
    int elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - codeCreatedAt_).count());
    int rem = expirationSeconds_ - elapsed;
    return (rem > 0) ? rem : 0;
}

bool PairingManager::isExpired() const {
    return remainingSeconds() <= 0;
}

bool PairingManager::isLocked() const {
    if (isLocked_) return true;
    for (const auto& [ip, fails] : failedAttemptsByClient_) {
        if (fails >= 5) return true;
    }
    return false;
}

int PairingManager::lockedClientsCount() const {
    int count = 0;
    for (const auto& [ip, fails] : failedAttemptsByClient_) {
        if (fails >= 5) count++;
    }
    return count;
}

bool PairingManager::isClientLocked(const std::string& clientKey) const {
    if (clientKey.empty()) return false;
    auto it = failedAttemptsByClient_.find(clientKey);
    return (it != failedAttemptsByClient_.end() && it->second >= 5);
}

bool PairingManager::isUsed() const {
    return isUsed_;
}

int PairingManager::failedAttempts(const std::string& clientKey) const {
    if (!clientKey.empty()) {
        auto it = failedAttemptsByClient_.find(clientKey);
        return (it != failedAttemptsByClient_.end()) ? it->second : 0;
    }
    int maxFails = failedAttempts_;
    for (const auto& [ip, fails] : failedAttemptsByClient_) {
        if (fails > maxFails) maxFails = fails;
    }
    return maxFails;
}

PairingManager::PairSubmitResult PairingManager::submitCode(const std::string& code,
                                                           const std::string& clientIp,
                                                           const std::string& peerName,
                                                           const std::string& origin,
                                                           bool isCrossLoopback) {
    PairSubmitResult res{};
    res.success = false;

    auto now = std::chrono::steady_clock::now();
    // Prune failed attempt timestamps older than 60 seconds
    failedAttemptTimestamps_.erase(
        std::remove_if(failedAttemptTimestamps_.begin(), failedAttemptTimestamps_.end(),
                       [&](const auto& tp) {
                           return std::chrono::duration_cast<std::chrono::seconds>(now - tp).count() >= 60;
                       }),
        failedAttemptTimestamps_.end());

    // Global limit across all clients: 10 failed attempts a minute; above it, return 429
    if (failedAttemptTimestamps_.size() >= 10) {
        res.rateLimited = true;
        res.error = "Too many failed pairing attempts across clients. Rate limit exceeded (10 per minute).";
        return res;
    }

    std::string clientKey = clientIp;
    if (!origin.empty()) {
        clientKey += "|" + origin;
    }

    bool clientLocked = isClientLocked(clientKey);
    res.locked = clientLocked;
    int fails = 0;
    auto it = failedAttemptsByClient_.find(clientKey);
    if (it != failedAttemptsByClient_.end()) fails = it->second;
    res.remainingAttempts = std::max(0, 5 - fails);

    if (clientLocked) {
        failedAttemptTimestamps_.push_back(now);
        res.error = "Pairing locked after 5 failed attempts. Please generate a new code on the host.";
        return res;
    }

    if (isUsed_) {
        res.error = "Pair code has already been used. Please request a new code from the host.";
        return res;
    }

    if (isExpired()) {
        res.error = "Pair code has expired (valid for 5 minutes). Please request a new code from the host.";
        return res;
    }

    if (code.size() != 6 || code != currentCode_) {
        failedAttemptTimestamps_.push_back(now);
        int newFails = ++failedAttemptsByClient_[clientKey];
        if (newFails >= 5) {
            res.locked = true;
            res.remainingAttempts = 0;
            res.error = "Pairing locked after 5 failed attempts. Please generate a new code on the host.";
        } else {
            res.remainingAttempts = 5 - newFails;
            res.error = "Invalid pair code (" + std::to_string(res.remainingAttempts) + " attempt" +
                        (res.remainingAttempts == 1 ? "" : "s") + " remaining).";
        }
        return res;
    }

    // If code is correct, but cross-origin pairing arriving over loopback -> refuse
    if (isCrossLoopback) {
        res.crossOriginLoopbackRefused = true;
        res.error = "Forbidden: cross-origin pairing over loopback refused";
        return res;
    }

    // Code matches and is valid! Real code is never locked out for non-locked clients.
    res.success = true;
    res.locked = false;
    res.remainingAttempts = std::max(0, 5 - fails);
    res.requestId = "req_" + generateRandomHex(16);

    PendingPairRequest req{};
    req.requestId = res.requestId;
    req.clientIp = clientIp.empty() ? "unknown" : clientIp;
    req.origin = origin.empty() ? "local" : origin;
    req.peerName = peerName.empty() ? ("Teammate (" + req.clientIp + ")") : peerName;
    req.code = code;
    req.requestedAt = now;
    req.status = PendingPairRequest::PENDING;

    pendingRequests_[res.requestId] = req;
    return res;
}

std::vector<PendingPairRequest> PairingManager::pendingRequests() const {
    std::vector<PendingPairRequest> list;
    auto now = std::chrono::steady_clock::now();
    for (const auto& [id, req] : pendingRequests_) {
        if (req.status == PendingPairRequest::PENDING) {
            // Only return requests made within the last 120 seconds
            auto sec = std::chrono::duration_cast<std::chrono::seconds>(now - req.requestedAt).count();
            if (sec <= 120) {
                list.push_back(req);
            }
        }
    }
    return list;
}

PendingPairRequest PairingManager::getRequestStatus(const std::string& requestId) const {
    auto it = pendingRequests_.find(requestId);
    if (it != pendingRequests_.end()) {
        return it->second;
    }
    PendingPairRequest notFound{};
    notFound.status = PendingPairRequest::DENIED;
    return notFound;
}

bool PairingManager::decideRequest(const std::string& requestId, bool allow, const std::string& customLabel) {
    auto it = pendingRequests_.find(requestId);
    if (it == pendingRequests_.end()) {
        return false;
    }

    if (it->second.status != PendingPairRequest::PENDING) {
        return false;
    }

    if (allow) {
        // Single-use: once approved, mark the pair code as used
        isUsed_ = true;

        std::string sessionToken = generate32ByteSessionToken();
        it->second.status = PendingPairRequest::APPROVED;
        it->second.sessionToken = sessionToken;

        PairedPeer peer{};
        peer.id = "peer_" + generateRandomHex(12);
        peer.sessionToken = sessionToken;
        peer.clientIp = it->second.clientIp;
        peer.peerName = customLabel.empty() ? it->second.peerName : customLabel;
        peer.pairedAt = currentTimestamp();

        pairedPeers_.push_back(peer);
    } else {
        it->second.status = PendingPairRequest::DENIED;
    }

    return true;
}

bool PairingManager::isValidSessionToken(const std::string& token) const {
    if (token.empty()) return false;
    for (const auto& peer : pairedPeers_) {
        if (!peer.sessionToken.empty() && peer.sessionToken == token) {
            return true;
        }
    }
    return false;
}

std::vector<PairedPeer> PairingManager::listPeers() const {
    return pairedPeers_;
}

bool PairingManager::revokePeer(const std::string& peerIdOrToken) {
    if (peerIdOrToken.empty()) return false;
    auto it = std::remove_if(pairedPeers_.begin(), pairedPeers_.end(),
                             [&](const PairedPeer& p) {
                                 return p.id == peerIdOrToken || p.sessionToken == peerIdOrToken;
                             });
    if (it != pairedPeers_.end()) {
        pairedPeers_.erase(it, pairedPeers_.end());
        return true;
    }
    return false;
}
