#include "HttpServer.h"
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <random>
#include "HtmlExporter.h"
#include "Exceptions.h"
#include "Json.h"
#include "PairingManager.h"
#include "FileUtil.h"
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <functional>
#ifndef _WIN32
#include <poll.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
using socket_t = SOCKET;
#define ISVALIDSOCKET(s) ((s) != INVALID_SOCKET)
#define CLOSESOCKET(s) closesocket(s)
#define GETSOCKETERR() WSAGetLastError()
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>
using socket_t = int;
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#define ISVALIDSOCKET(s) ((s) >= 0)
#define CLOSESOCKET(s) close(s)
#define GETSOCKETERR() (errno)
#endif

namespace {

bool openBrowserUrl(const std::string& url) {
    return openInBrowser(url);
}

std::string getLocalLanIp() {
    socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (ISVALIDSOCKET(s)) {
        sockaddr_in loopback{};
        loopback.sin_family = AF_INET;
        loopback.sin_addr.s_addr = inet_addr("8.8.8.8");
        loopback.sin_port = htons(53);
        if (connect(s, (struct sockaddr*)&loopback, sizeof(loopback)) == 0) {
            sockaddr_in name{};
#ifdef _WIN32
            int len = sizeof(name);
#else
            socklen_t len = sizeof(name);
#endif
            if (getsockname(s, (struct sockaddr*)&name, &len) == 0) {
                char ipBuf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &name.sin_addr, ipBuf, sizeof(ipBuf));
                CLOSESOCKET(s);
                return ipBuf;
            }
        }
        CLOSESOCKET(s);
    }
    return "127.0.0.1";
}

std::string getHeaderValue(const std::string& request, const std::string& headerName) {
    std::string target = "\n" + headerName + ":";
    auto it = std::search(request.begin(), request.end(), target.begin(), target.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    if (it == request.end()) {
        std::string targetStart = headerName + ":";
        if (request.size() >= targetStart.size()) {
            bool match = true;
            for (size_t i = 0; i < targetStart.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(request[i])) !=
                    std::tolower(static_cast<unsigned char>(targetStart[i]))) {
                    match = false;
                    break;
                }
            }
            if (match) it = request.begin() - 1;
        }
    }
    if (it == request.end()) return "";

    size_t pos = (it - request.begin()) + 1 + headerName.size() + 1;
    size_t lineEnd = request.find("\r\n", pos);
    if (lineEnd == std::string::npos) lineEnd = request.find("\n", pos);
    if (lineEnd == std::string::npos) lineEnd = request.size();

    std::string val = request.substr(pos, lineEnd - pos);
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r')) val.pop_back();
    return val;
}

std::string getQueryParam(const std::string& query, const std::string& key) {
    std::string needle = key + "=";
    size_t pos = query.find(needle);
    if (pos == std::string::npos) return "";
    size_t start = pos + needle.size();
    size_t end = query.find('&', start);
    if (end == std::string::npos) end = query.size();
    return query.substr(start, end - start);
}

bool parseRequestBody(const std::string& body, JsonValue& val, std::string& errMsg) {
    val = JsonValue::parse(body, &errMsg);
    if (val.is_null() && !errMsg.empty()) {
        return false;
    }
    return true;
}

bool getRequiredString(const JsonValue& obj, const std::string& key, std::string& out, std::string& errMsg) {
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_string()) {
        errMsg = "Missing or invalid required field: '" + key + "'";
        return false;
    }
    out = obj[key].as_string();
    return true;
}

std::string getOptionalString(const JsonValue& obj, const std::string& key, const std::string& defaultVal = "") {
    if (obj.is_object() && obj.contains(key) && obj[key].is_string()) {
        return obj[key].as_string();
    }
    return defaultVal;
}

void sendAll(socket_t sock, const std::string& data) {
    size_t total = 0;
    while (total < data.size()) {
        int sent = send(sock, data.data() + total, static_cast<int>(data.size() - total), 0);
        if (sent <= 0) break;
        total += sent;
    }
}

static bool isIPv4Literal(const std::string& s) {
    if (s.empty()) return false;
    int parts = 0;
    size_t i = 0;
    while (i < s.size()) {
        if (parts > 3) return false;
        size_t nextDot = s.find('.', i);
        std::string part = (nextDot == std::string::npos) ? s.substr(i) : s.substr(i, nextDot - i);
        if (part.empty() || part.size() > 3) return false;
        for (char c : part) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        if (part.size() > 1 && part[0] == '0') return false;
        int val = std::stoi(part);
        if (val < 0 || val > 255) return false;
        parts++;
        if (nextDot == std::string::npos) break;
        i = nextDot + 1;
        if (i >= s.size()) return false;
    }
    return parts == 4;
}

void sendHttpResponse(socket_t sock, int statusCode, const std::string& statusText,
                      const std::string& contentType, const std::string& body,
                      const std::string& corsOrigin = "", const std::string& extraHeaders = "") {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n";
    if (!extraHeaders.empty()) {
        oss << extraHeaders;
    }
    if (!corsOrigin.empty()) {
        oss << "Access-Control-Allow-Origin: " << corsOrigin << "\r\n"
            << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            << "Access-Control-Allow-Headers: Content-Type, X-MiniGit-Token\r\n"
            << "Vary: Origin\r\n";
    }
    oss << "Connection: close\r\n\r\n"
        << body;
    sendAll(sock, oss.str());
}

} // namespace


class HttpServerThreadPool {
public:
    explicit HttpServerThreadPool(size_t threads) : stop_(false) {
        for (size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this]() {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queueMutex_);
                        cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                }
            });
        }
    }

    bool enqueue(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (stop_) return false;
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
        return true;
    }

    ~HttpServerThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queueMutex_;
    std::condition_variable cv_;
    bool stop_;
};

static void handleClientConnection(socket_t clientSock, sockaddr_in clientAddr, int boundPort, bool lan,
                                   PairingManager& pairingMgr, Repository* repo,
                                   std::recursive_mutex& repoMutex, std::recursive_mutex& pairingMutex,
                                   const std::string& repoName, const std::string& lanUrl) {

#ifdef _WIN32
        DWORD sockTimeout = 5000;
        setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&sockTimeout, sizeof(sockTimeout));
        setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sockTimeout, sizeof(sockTimeout));
#else
        struct timeval sockTv{};
        sockTv.tv_sec = 5;
        sockTv.tv_usec = 0;
        setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, &sockTv, sizeof(sockTv));
        setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, &sockTv, sizeof(sockTv));
#endif

        char clientIpStr[INET_ADDRSTRLEN] = "127.0.0.1";
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIpStr, sizeof(clientIpStr));

        uint32_t clientIp = ntohl(clientAddr.sin_addr.s_addr);
        bool isLoopback = ((clientIp >> 24) == 127) || (clientIp == 0);

        std::string request;
        char buf[8192];
        int bytesRead = 0;
        bool payloadTooLarge = false;
        bool headersTooLarge = false;
        bool requestComplete = false;

        auto reqStart = std::chrono::steady_clock::now();
        const auto reqDeadline = reqStart + std::chrono::seconds(10);

        while (true) {
            auto now = std::chrono::steady_clock::now();
            if (now >= reqDeadline) {
                break;
            }
            int remainingMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(reqDeadline - now).count());
            if (remainingMs <= 0) break;
            if (remainingMs > 5000) remainingMs = 5000;

#ifdef _WIN32
            WSAPOLLFD pfd{};
            pfd.fd = clientSock;
            pfd.events = POLLIN;
            int pRes = WSAPoll(&pfd, 1, remainingMs);
#else
            struct pollfd pfd{};
            pfd.fd = clientSock;
            pfd.events = POLLIN;
            int pRes = poll(&pfd, 1, remainingMs);
#endif
            if (pRes <= 0) break;
            if (!(pfd.revents & POLLIN)) break;

            bytesRead = recv(clientSock, buf, sizeof(buf) - 1, 0);
            if (bytesRead <= 0) break;

            buf[bytesRead] = '\0';
            request.append(buf, bytesRead);

            size_t headerEnd = request.find("\r\n\r\n");
            if (headerEnd == std::string::npos) {
                if (request.size() > 64 * 1024) {
                    headersTooLarge = true;
                    break;
                }
            } else {
                if (headerEnd > 64 * 1024) {
                    headersTooLarge = true;
                    break;
                }
                // Header complete. Check Content-Length case-insensitively
                std::string clStr = getHeaderValue(request, "Content-Length");
                size_t cl = 0;
                if (!clStr.empty()) {
                    try {
                        cl = std::stoull(clStr);
                    } catch (...) {
                        cl = 0;
                    }
                    if (cl > 50 * 1024 * 1024) { // 50 MB limit
                        payloadTooLarge = true;
                        break;
                    }
                }
                size_t bodyStart = headerEnd + 4;
                if (request.size() - bodyStart >= cl) {
                    requestComplete = true;
                    break;
                }
            }
        }

        if (headersTooLarge) {
            sendHttpResponse(clientSock, 431, "Request Header Fields Too Large", "application/json",
                             "{\"ok\":false,\"error\":\"Request Header Fields Too Large (max 64KB)\"}");
            CLOSESOCKET(clientSock);
            return;
        }

        if (payloadTooLarge) {
            sendHttpResponse(clientSock, 413, "Payload Too Large", "application/json",
                             "{\"ok\":false,\"error\":\"Payload Too Large (max 50MB)\"}");
            CLOSESOCKET(clientSock);
            return;
        }

        if (!requestComplete) {
            auto now = std::chrono::steady_clock::now();
            if (now >= reqDeadline) {
                sendHttpResponse(clientSock, 408, "Request Timeout", "application/json",
                                 "{\"ok\":false,\"error\":\"Request Timeout (Slowloris protection)\"}");
            }
            CLOSESOCKET(clientSock);
            return;
        }

        // Parse Method and URI
        std::istringstream reqStream(request);
        std::string method, uri, version;
        reqStream >> method >> uri >> version;

        std::string path = uri;
        std::string query;
        size_t qPos = uri.find('?');
        if (qPos != std::string::npos) {
            path = uri.substr(0, qPos);
            query = uri.substr(qPos + 1);
        }

        // Extract body
        std::string body;
        size_t bodyPos = request.find("\r\n\r\n");
        if (bodyPos != std::string::npos) {
            body = request.substr(bodyPos + 4);
        }

        // Host header validation (DNS rebinding protection)
        std::string host = getHeaderValue(request, "Host");
        std::string portStr = std::to_string(boundPort);
        std::string hostPart = host, hostPort = "";
        size_t colon = host.rfind(':');
        if (colon != std::string::npos) {
            hostPart = host.substr(0, colon);
            hostPort = host.substr(colon + 1);
        }
        bool validPort = (hostPort.empty() || hostPort == portStr);
        bool validHost = false;
        if (validPort) {
            if (hostPart == "127.0.0.1" || hostPart == "localhost") {
                validHost = true;
            } else if (lan && isIPv4Literal(hostPart)) {
                validHost = true;
            }
        }
        if (!validHost) {
            sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                             "{\"ok\":false,\"error\":\"Forbidden: invalid Host header\"}");
            CLOSESOCKET(clientSock);
            return;
        }

        std::string origin = getHeaderValue(request, "Origin");
        bool isSyncEndpoint = (path == "/api/sync/bundle" || path == "/api/sync/import-bundle");
        bool isPairEndpoint = (path == "/api/pair" || path == "/api/pair/status");
        bool isSyncOrPairEndpoint = (isSyncEndpoint || isPairEndpoint);

        // OPTIONS handling (no token required)
        if (method == "OPTIONS") {
            std::string optCorsOrigin = "";
            if (isSyncOrPairEndpoint && !origin.empty() && origin != "null") {
                optCorsOrigin = origin;
            }
            sendHttpResponse(clientSock, 204, "No Content", "text/plain", "", optCorsOrigin);
            CLOSESOCKET(clientSock);
            return;
        }

        std::string clientToken = getHeaderValue(request, "X-MiniGit-Token");
        bool hasValidToken = false;
        {
            std::lock_guard<std::recursive_mutex> pLock(pairingMutex);
            hasValidToken = !clientToken.empty() && pairingMgr.isValidSessionToken(clientToken);
        }

        std::string corsOrigin = "";
        if (!origin.empty() && origin != "null") {
            if (origin == ("http://127.0.0.1:" + std::to_string(boundPort)) ||
                origin == ("http://localhost:" + std::to_string(boundPort))) {
                corsOrigin = origin;
            } else if (isSyncOrPairEndpoint) {
                corsOrigin = origin;
            }
        }

        if (!isLoopback) {
            if (!isSyncOrPairEndpoint) {
                sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                 "{\"ok\":false,\"error\":\"Forbidden: non-loopback access restricted to sync/pair endpoints\"}",
                                 corsOrigin);
                CLOSESOCKET(clientSock);
                return;
            }
            if (isSyncEndpoint && !hasValidToken) {
                sendHttpResponse(clientSock, 401, "Unauthorized", "application/json",
                                 "{\"ok\":false,\"error\":\"Unauthorized: invalid or revoked session token. Please pair with host.\"}",
                                 corsOrigin);
                CLOSESOCKET(clientSock);
                return;
            }
        }

        if (isLoopback && method != "GET" && method != "OPTIONS") {
            bool isExempt = (path == "/api/pair") || (isSyncEndpoint && hasValidToken);
            if (!isExempt) {
                if (!origin.empty() &&
                    origin != ("http://127.0.0.1:" + std::to_string(boundPort)) &&
                    origin != ("http://localhost:" + std::to_string(boundPort))) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden: invalid origin\"}", corsOrigin);
                    CLOSESOCKET(clientSock);
                    return;
                }
            }
            std::string ct = getHeaderValue(request, "Content-Type");
            if (ct.find("application/json") == std::string::npos) {
                sendHttpResponse(clientSock, 400, "Bad Request", "application/json",
                                 "{\"ok\":false,\"error\":\"Content-Type must be application/json\"}", corsOrigin);
                CLOSESOCKET(clientSock);
                return;
            }
        }

        if (path == "/api/sync/bundle") {
            bool isLocalOrigin = origin.empty() ||
                                 origin == ("http://127.0.0.1:" + std::to_string(boundPort)) ||
                                 origin == ("http://localhost:" + std::to_string(boundPort));
            if (!isLocalOrigin && !hasValidToken) {
                sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                 "{\"ok\":false,\"error\":\"Forbidden: cross-origin bundle access refused\"}", corsOrigin);
                CLOSESOCKET(clientSock);
                return;
            }
        }

        auto getSnapJson = [&]() {
            try {
                std::lock_guard<std::recursive_mutex> rLock(repoMutex);
                Snapshot s = repo->snapshot();
                s.lan = lan;
                s.lanUrl = lanUrl;
                return HtmlExporter::toJson(s, repoName);
            } catch (...) {
                return std::string("{}");
            }
        };

        try {
            std::lock_guard<std::recursive_mutex> rLock(repoMutex);
            repo->reload();
            if (method == "GET" && (path == "/" || path == "/index.html")) {
                Snapshot snap = repo->snapshot();
                snap.lan = lan;
                snap.lanUrl = lanUrl;
                std::string html = HtmlExporter::render(snap, repoName);
                sendHttpResponse(clientSock, 200, "OK", "text/html; charset=utf-8", html, corsOrigin);
            } else if (method == "GET" && path == "/api/token") {
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", corsOrigin);
                } else {
                    std::string tokenJson = "{\"ok\":true,\"code\":\"" + pairingMgr.currentCode() +
                                            "\",\"remainingSeconds\":" + std::to_string(pairingMgr.remainingSeconds()) +
                                            ",\"isExpired\":" + (pairingMgr.isExpired() ? "true" : "false") +
                                            ",\"isInvalidated\":" + (pairingMgr.isInvalidated() ? "true" : "false") +
                                            ",\"isLocked\":" + (pairingMgr.isLocked() ? "true" : "false") +
                                            ",\"lockedClientsCount\":" + std::to_string(pairingMgr.lockedClientsCount()) +
                                            ",\"isUsed\":" + (pairingMgr.isUsed() ? "true" : "false") +
                                            ",\"lan\":" + (lan ? "true" : "false") +
                                            ",\"lanUrl\":\"" + lanUrl + "\"}";
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", tokenJson, corsOrigin);
                }
            } else if (method == "GET" && path == "/api/pair/code") {
                std::lock_guard<std::recursive_mutex> pLock(pairingMutex);
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", corsOrigin);
                } else {
                    std::string codeJson = "{\"ok\":true,\"code\":\"" + pairingMgr.currentCode() +
                                           "\",\"remainingSeconds\":" + std::to_string(pairingMgr.remainingSeconds()) +
                                           ",\"isExpired\":" + (pairingMgr.isExpired() ? "true" : "false") +
                                           ",\"isInvalidated\":" + (pairingMgr.isInvalidated() ? "true" : "false") +
                                           ",\"isLocked\":" + (pairingMgr.isLocked() ? "true" : "false") +
                                           ",\"lockedClientsCount\":" + std::to_string(pairingMgr.lockedClientsCount()) +
                                           ",\"isUsed\":" + (pairingMgr.isUsed() ? "true" : "false") +
                                           ",\"failedAttempts\":" + std::to_string(pairingMgr.failedAttempts()) +
                                           ",\"lan\":" + (lan ? "true" : "false") +
                                           ",\"lanUrl\":\"" + lanUrl + "\"}";
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", codeJson, corsOrigin);
                }
            } else if (method == "POST" && path == "/api/pair/generate-code") {
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", corsOrigin);
                } else {
                    std::string newCode = pairingMgr.generateNewCode();
                    std::string codeJson = "{\"ok\":true,\"code\":\"" + newCode +
                                           "\",\"remainingSeconds\":" + std::to_string(pairingMgr.remainingSeconds()) +
                                           ",\"isExpired\":false,\"isLocked\":false,\"lockedClientsCount\":0,\"isUsed\":false,\"failedAttempts\":0}";
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", codeJson, corsOrigin);
                }
            } else if (method == "POST" && path == "/api/pair") {
                if (body.size() > 4096) {
                    sendHttpResponse(clientSock, 413, "Payload Too Large", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Payload Too Large (max 4KB)\"}", corsOrigin);
                    CLOSESOCKET(clientSock);
                    return;
                }
                if (!lan) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"LAN mode is not enabled on host\"}", corsOrigin);
                    CLOSESOCKET(clientSock);
                    return;
                }
                if (isLoopback) {
                    bool isLocalOrigin = origin.empty() || origin == "null" ||
                                         origin == ("http://127.0.0.1:" + std::to_string(boundPort)) ||
                                         origin == ("http://localhost:" + std::to_string(boundPort));
                    if (!isLocalOrigin) {
                        sendHttpResponse(clientSock, 403, "Forbidden", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":\"Forbidden: cross-origin pairing over loopback refused\"}", corsOrigin);
                        CLOSESOCKET(clientSock);
                        return;
                    }
                }
                JsonValue bodyJson;
                std::string parseErr;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString("Invalid JSON: " + parseErr) + "}", corsOrigin);
                    CLOSESOCKET(clientSock);
                    return;
                }
                std::string code = getOptionalString(bodyJson, "code");
                std::string peerName = getOptionalString(bodyJson, "peerName");
                if (peerName.empty()) peerName = getOptionalString(bodyJson, "clientName");
                bool isCrossLoopback = isLoopback && !origin.empty() && origin != "null" &&
                                       origin != ("http://127.0.0.1:" + std::to_string(boundPort)) &&
                                       origin != ("http://localhost:" + std::to_string(boundPort));
                PairingManager::PairSubmitResult res;
                {
                    std::lock_guard<std::recursive_mutex> pLock(pairingMutex);
                    res = pairingMgr.submitCode(code, clientIpStr, peerName, origin, isCrossLoopback);
                }
                if (res.rateLimited) {
                    sendHttpResponse(clientSock, 429, "Too Many Requests", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString(res.error) + ",\"rateLimited\":true}",
                                     corsOrigin);
                } else if (res.crossOriginLoopbackRefused) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString(res.error) + "}",
                                     corsOrigin);
                } else if (res.success) {
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                     "{\"ok\":true,\"status\":\"pending\",\"requestId\":\"" + res.requestId +
                                     "\",\"message\":\"Pairing request submitted. Waiting for host approval.\"}",
                                     corsOrigin);
                } else {
                    int sc = res.locked ? 403 : 401;
                    sendHttpResponse(clientSock, sc, sc == 403 ? "Forbidden" : "Unauthorized", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString(res.error) + ",\"locked\":" + (res.locked ? "true" : "false") +
                                     ",\"remainingAttempts\":" + std::to_string(res.remainingAttempts) + "}",
                                     corsOrigin);
                }
            } else if ((method == "GET" || method == "POST") && path == "/api/pair/status") {
                if (body.size() > 4096) {
                    sendHttpResponse(clientSock, 413, "Payload Too Large", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Payload Too Large (max 4KB)\"}", corsOrigin);
                    CLOSESOCKET(clientSock);
                    return;
                }
                if (!lan) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"LAN mode is not enabled on host\"}", corsOrigin);
                } else {
                    std::string reqId = getQueryParam(query, "requestId");
                    if (reqId.empty() && !body.empty()) {
                        JsonValue bj;
                        std::string pe;
                        if (parseRequestBody(body, bj, pe)) reqId = getOptionalString(bj, "requestId");
                    }
                    PendingPairRequest statusReq;
                    {
                        std::lock_guard<std::recursive_mutex> pLock(pairingMutex);
                        statusReq = pairingMgr.getRequestStatus(reqId);
                    }
                    if (reqId.empty() || statusReq.requestId.empty()) {
                        sendHttpResponse(clientSock, 404, "Not Found", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"status\":\"unknown\",\"error\":\"Unknown or expired pairing request\"}",
                                         corsOrigin);
                    } else if (statusReq.status == PendingPairRequest::PENDING) {
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"status\":\"pending\"}", corsOrigin);
                    } else if (statusReq.status == PendingPairRequest::APPROVED) {
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"status\":\"approved\",\"token\":\"" + statusReq.sessionToken + "\"}",
                                         corsOrigin);
                    } else {
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"status\":\"denied\",\"error\":\"Host denied the pairing request.\"}",
                                         corsOrigin);
                    }
                }
            } else if (method == "GET" && path == "/api/pair/pending") {
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", corsOrigin);
                } else {
                    auto pending = pairingMgr.pendingRequests();
                    std::ostringstream oss;
                    oss << "{\"ok\":true,\"pending\":[";
                    for (size_t i = 0; i < pending.size(); ++i) {
                        if (i > 0) oss << ",";
                        oss << "{\"id\":" << jsonString(pending[i].requestId)
                            << ",\"ip\":" << jsonString(pending[i].clientIp)
                            << ",\"origin\":" << jsonString(pending[i].origin)
                            << ",\"peerName\":" << jsonString(pending[i].peerName)
                            << ",\"code\":" << jsonString(pending[i].code) << "}";
                    }
                    oss << "]}";
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", oss.str(), corsOrigin);
                }
            } else if (method == "POST" && path == "/api/pair/decide") {
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", corsOrigin);
                } else {
                    JsonValue bodyJson;
                    std::string parseErr;
                    if (!parseRequestBody(body, bodyJson, parseErr)) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString("Invalid JSON: " + parseErr) + "}", corsOrigin);
                        CLOSESOCKET(clientSock);
                        return;
                    }
                    std::string reqId = getOptionalString(bodyJson, "requestId");
                    std::string action = getOptionalString(bodyJson, "action");
                    std::string label = getOptionalString(bodyJson, "label");
                    bool isAllow = (action == "allow" || action == "approve");
                    bool isDeny = (action == "deny" || action == "reject");
                    if (!isAllow && !isDeny) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":\"Missing or unknown action. Must be 'allow' or 'deny'\"}",
                                         corsOrigin);
                        CLOSESOCKET(clientSock);
                        return;
                    }
                    bool ok = false;
                    {
                        std::lock_guard<std::recursive_mutex> pLock(pairingMutex);
                        ok = pairingMgr.decideRequest(reqId, isAllow, label);
                    }
                    sendHttpResponse(clientSock, ok ? 200 : 400, ok ? "OK" : "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":" + std::string(ok ? "true" : "false") + ",\"action\":\"" + action + "\"}",
                                     corsOrigin);
                }
            } else if (method == "GET" && path == "/api/pair/peers") {
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", corsOrigin);
                } else {
                    std::vector<PairedPeer> peers;
                    {
                        std::lock_guard<std::recursive_mutex> pLock(pairingMutex);
                        peers = pairingMgr.listPeers();
                    }
                    std::ostringstream oss;
                    oss << "{\"ok\":true,\"peers\":[";
                    for (size_t i = 0; i < peers.size(); ++i) {
                        if (i > 0) oss << ",";
                        oss << "{\"id\":" << jsonString(peers[i].id)
                            << ",\"ip\":" << jsonString(peers[i].clientIp)
                            << ",\"peerName\":" << jsonString(peers[i].peerName)
                            << ",\"pairedAt\":" << jsonString(peers[i].pairedAt) << "}";
                    }
                    oss << "]}";
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", oss.str(), corsOrigin);
                }
            } else if (method == "POST" && path == "/api/pair/revoke") {
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", corsOrigin);
                } else {
                    JsonValue bodyJson;
                    std::string parseErr;
                    if (!parseRequestBody(body, bodyJson, parseErr)) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString("Invalid JSON: " + parseErr) + "}", corsOrigin);
                        CLOSESOCKET(clientSock);
                        return;
                    }
                    std::string id = getOptionalString(bodyJson, "id");
                    std::string tok = getOptionalString(bodyJson, "token");
                    bool ok = false;
                    {
                        std::lock_guard<std::recursive_mutex> pLock(pairingMutex);
                        ok = pairingMgr.revokePeer(!id.empty() ? id : tok);
                    }
                    sendHttpResponse(clientSock, ok ? 200 : 404, ok ? "OK" : "Not Found", "application/json; charset=utf-8",
                                     "{\"ok\":" + std::string(ok ? "true" : "false") + ",\"message\":\"" +
                                     (ok ? "Peer access revoked" : "Peer not found") + "\"}",
                                     corsOrigin);
                }
            } else if (method == "GET" && path == "/api/snapshot") {
                Snapshot snap = repo->snapshot();
                snap.lan = lan;
                std::string ver = HtmlExporter::computeVersion(snap);
                std::string quotedEtag = "\"" + ver + "\"";
                std::string ifNoneMatch = getHeaderValue(request, "If-None-Match");
                if (!ifNoneMatch.empty() && (ifNoneMatch == quotedEtag || ifNoneMatch == ver || ifNoneMatch == "W/" + quotedEtag || ifNoneMatch == "*")) {
                    std::ostringstream oss;
                    oss << "HTTP/1.1 304 Not Modified\r\n"
                        << "ETag: " << quotedEtag << "\r\n"
                        << "Content-Length: 0\r\n"
                        << "Connection: close\r\n\r\n";
                    sendAll(clientSock, oss.str());
                } else {
                    std::string json = HtmlExporter::toJson(snap, repoName);
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", json, corsOrigin, "ETag: " + quotedEtag + "\r\n");
                }
            } else if (method == "POST" && (path == "/api/file/save" || path == "/api/file/write")) {
                JsonValue bodyJson;
                std::string parseErr, p;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString("Invalid JSON: " + parseErr) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else if (!getRequiredString(bodyJson, "path", p, parseErr) || p.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Path is required\",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else {
                    try {
                        std::string rel = repo->resolveRepoPath(p, true);
                        std::string content = getOptionalString(bodyJson, "content");
                        fs::path full = repo->root() / rel;
                        if (full.has_parent_path()) {
                            std::error_code ec;
                            fs::create_directories(full.parent_path(), ec);
                        }
                        writeFile(full, content);
                        std::string snap = getSnapJson();
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"message\":\"saved\",\"snapshot\":" + snap + "}", corsOrigin);
                    } catch (const std::exception& e) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                    }
                }
            } else if (method == "POST" && path == "/api/file/delete") {
                JsonValue bodyJson;
                std::string parseErr, p;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString("Invalid JSON: " + parseErr) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else if (!getRequiredString(bodyJson, "path", p, parseErr) || p.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Path is required\",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else {
                    try {
                        std::string rel = repo->resolveRepoPath(p, true);
                        fs::path full = repo->root() / rel;
                        if (!fs::exists(full)) {
                            sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                             "{\"ok\":false,\"error\":\"File does not exist\",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                            CLOSESOCKET(clientSock);
                            return;
                        }
                        if (fs::is_directory(full)) {
                            sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                             "{\"ok\":false,\"error\":\"Cannot delete directory with file delete\",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                            CLOSESOCKET(clientSock);
                            return;
                        }
                        std::error_code ec;
                        fs::remove(full, ec);
                        if (ec) {
                            sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                             "{\"ok\":false,\"error\":" + jsonString(ec.message()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                            CLOSESOCKET(clientSock);
                            return;
                        }
                        std::string snap = getSnapJson();
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"message\":\"deleted\",\"snapshot\":" + snap + "}", corsOrigin);
                    } catch (const std::exception& e) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                    }
                }
            } else if (method == "POST" && path == "/api/stage") {
                JsonValue bodyJson;
                std::string parseErr, p;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString("Invalid JSON: " + parseErr) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else if (!getRequiredString(bodyJson, "path", p, parseErr) || p.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Path is required\",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else {
                    try {
                        repo->add(p, true);
                        std::string snap = getSnapJson();
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"message\":\"staged\",\"snapshot\":" + snap + "}", corsOrigin);
                    } catch (const std::exception& e) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                    }
                }
            } else if (method == "POST" && path == "/api/unstage") {
                JsonValue bodyJson;
                std::string parseErr, p;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString("Invalid JSON: " + parseErr) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else {
                    try {
                        p = getOptionalString(bodyJson, "path");
                        if (!p.empty() && p != ".") {
                            std::string rel = repo->resolveRepoPath(p, true);
                            fs::path full = repo->root() / rel;
                            Snapshot s = repo->snapshot();
                            bool existsInStaged = (s.staged.find(rel) != s.staged.end());
                            bool existsInStagedDeleted = (std::find(s.stagedDeleted.begin(), s.stagedDeleted.end(), rel) != s.stagedDeleted.end());
                            bool existsOnDisk = fs::exists(full);
                            if (!existsOnDisk && !existsInStaged && !existsInStagedDeleted) {
                                sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                                 "{\"ok\":false,\"error\":\"Path does not exist\",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                                CLOSESOCKET(clientSock);
                                return;
                            }
                        }
                        repo->unstage(p, true);
                        std::string snap = getSnapJson();
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"message\":\"unstaged\",\"snapshot\":" + snap + "}", corsOrigin);
                    } catch (const std::exception& e) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                    }
                }
            } else if (method == "POST" && path == "/api/discard") {
                JsonValue bodyJson;
                std::string parseErr, p;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString("Invalid JSON: " + parseErr) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else if (!getRequiredString(bodyJson, "path", p, parseErr) || p.empty() || p == "." || p == ".." || fs::path(p).is_absolute()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Invalid or missing path for discard\",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else {
                    try {
                        repo->discard(p, true);
                        std::string snap = getSnapJson();
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"message\":\"discarded\",\"snapshot\":" + snap + "}", corsOrigin);
                    } catch (const std::exception& e) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                    }
                }
            } else if (method == "POST" && path == "/api/commit") {
                JsonValue bodyJson;
                std::string parseErr, msg;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString("Invalid JSON: " + parseErr) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else if (!getRequiredString(bodyJson, "message", msg, parseErr) || msg.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Commit message cannot be empty\",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else {
                    try {
                        Hash h = repo->commit(msg);
                        std::string snap = getSnapJson();
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"hash\":\"" + h.str() + "\",\"snapshot\":" + snap + "}", corsOrigin);
                    } catch (const std::exception& e) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                    }
                }
            } else if (method == "POST" && path == "/api/branch/create") {
                JsonValue bodyJson;
                std::string parseErr, name;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString("Invalid JSON: " + parseErr) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else if (!getRequiredString(bodyJson, "name", name, parseErr) || name.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString("Branch name cannot be empty") + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else {
                    try {
                        Hash atHash;
                        if (bodyJson.contains("at") && bodyJson["at"].is_string()) {
                            std::string atStr = bodyJson["at"].as_string();
                            if (!atStr.empty()) {
                                ObjectStore store(repo->root() / ".minigit" / "objects");
                                atHash = store.resolvePrefix(atStr);
                            }
                        }
                        repo->createBranch(name, atHash);
                        std::string snap = getSnapJson();
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"message\":\"created branch\",\"snapshot\":" + snap + "}", corsOrigin);
                    } catch (const std::exception& e) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                    }
                }
            } else if (method == "POST" && path == "/api/branch/delete") {
                JsonValue bodyJson;
                std::string parseErr, name;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString("Invalid JSON: " + parseErr) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else if (!getRequiredString(bodyJson, "name", name, parseErr) || name.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString("Branch name cannot be empty") + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else {
                    try {
                        repo->deleteBranch(name);
                        std::string snap = getSnapJson();
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"message\":\"deleted branch\",\"snapshot\":" + snap + "}", corsOrigin);
                    } catch (const std::exception& e) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                    }
                }
            } else if (method == "POST" && path == "/api/checkout") {
                JsonValue bodyJson;
                std::string parseErr, target;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString("Invalid JSON: " + parseErr) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else if (!getRequiredString(bodyJson, "target", target, parseErr) || target.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString("Target cannot be empty") + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else {
                    try {
                        repo->checkout(target);
                        std::string snap = getSnapJson();
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"message\":\"checked out\",\"snapshot\":" + snap + "}", corsOrigin);
                    } catch (const MiniGitException& e) {
                        std::string snap = getSnapJson();
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + snap + "}", corsOrigin);
                    }
                }
            } else if (method == "POST" && path == "/api/merge") {
                JsonValue bodyJson;
                std::string parseErr, branch;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString("Invalid JSON: " + parseErr) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else {
                    bool isAbort = (bodyJson.contains("abort") && bodyJson["abort"].is_bool() && bodyJson["abort"].as_bool());
                    if (!isAbort && bodyJson.contains("branch") && bodyJson["branch"].is_string() && bodyJson["branch"].as_string() == "--abort") {
                        isAbort = true;
                    }
                    if (isAbort) {
                        try {
                            std::string msg = repo->mergeAbort();
                            std::string snap = getSnapJson();
                            sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                             "{\"ok\":true,\"result\":" + jsonString(msg) + ",\"snapshot\":" + snap + "}", corsOrigin);
                        } catch (const MiniGitException& e) {
                            sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                             "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                        }
                    } else if (!getRequiredString(bodyJson, "branch", branch, parseErr) || branch.empty()) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":\"Branch cannot be empty\",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                    } else {
                        try {
                            std::string msg = repo->merge(branch);
                            std::string snap = getSnapJson();
                            sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                             "{\"ok\":true,\"result\":" + jsonString(msg) + ",\"snapshot\":" + snap + "}", corsOrigin);
                        } catch (const MiniGitException& e) {
                            std::string snap = getSnapJson();
                            sendHttpResponse(clientSock, 409, "Conflict", "application/json; charset=utf-8",
                                             "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + snap + "}", corsOrigin);
                        }
                    }
                }
            } else if (method == "POST" && path == "/api/revert") {
                JsonValue bodyJson;
                std::string parseErr, hash;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString("Invalid JSON: " + parseErr) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else if (!getRequiredString(bodyJson, "hash", hash, parseErr) || hash.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Hash cannot be empty\",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else {
                    try {
                        Hash h = repo->revert(Hash(hash));
                        std::string snap = getSnapJson();
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"hash\":\"" + h.str() + "\",\"snapshot\":" + snap + "}", corsOrigin);
                    } catch (const std::exception& e) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                    }
                }
            } else if (method == "POST" && path == "/api/undo") {
                try {
                    std::string resMsg = repo->undo();
                    std::string snap = getSnapJson();
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                     "{\"ok\":true,\"message\":" + jsonString(resMsg) + ",\"snapshot\":" + snap + "}", corsOrigin);
                } catch (const std::exception& e) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                }
            } else if (method == "POST" && path == "/api/stash/save") {
                JsonValue bodyJson;
                std::string parseErr, msg;
                bool includeUntracked = false;
                if (!body.empty() && parseRequestBody(body, bodyJson, parseErr)) {
                    msg = getOptionalString(bodyJson, "message");
                    if (bodyJson.contains("includeUntracked") && bodyJson["includeUntracked"].as_bool()) {
                        includeUntracked = true;
                    }
                }
                try {
                    std::string resMsg = repo->stashSave(msg, includeUntracked);
                    std::string snap = getSnapJson();
                    if (resMsg == "no local changes to save") {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(resMsg) + ",\"snapshot\":" + snap + "}", corsOrigin);
                    } else {
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"message\":" + jsonString(resMsg) + ",\"snapshot\":" + snap + "}", corsOrigin);
                    }
                } catch (const std::exception& e) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                }
            } else if (method == "POST" && path == "/api/stash/pop") {
                try {
                    std::string resMsg = repo->stashPop();
                    std::string snap = getSnapJson();
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                     "{\"ok\":true,\"message\":" + jsonString(resMsg) + ",\"snapshot\":" + snap + "}", corsOrigin);
                } catch (const std::exception& e) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                }
            } else if (method == "GET" && path == "/api/stash/list") {
                auto list = repo->stashList();
                std::ostringstream oss;
                oss << "{\"ok\":true,\"stashes\":[";
                for (size_t i = 0; i < list.size(); ++i) {
                    if (i > 0) oss << ",";
                    oss << "{\"index\":" << list[i].index
                        << ",\"hash\":\"" << list[i].commitHash.str() << "\""
                        << ",\"message\":" << jsonString(list[i].message)
                        << ",\"timestamp\":" << jsonString(list[i].timestamp) << "}";
                }
                oss << "]}";
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", oss.str(), corsOrigin);
            } else if (method == "GET" && path == "/api/reflog") {
                auto entries = repo->reflog();
                std::ostringstream json;
                json << "{\"ok\":true,\"reflog\":[";
                bool first = true;
                for (const auto& r : entries) {
                    if (!first) json << ',';
                    first = false;
                    json << "{\"timestamp\":" << jsonString(r.timestamp)
                         << ",\"action\":" << jsonString(r.action)
                         << ",\"oldHash\":\"" << r.oldHash << "\""
                         << ",\"newHash\":\"" << r.newHash << "\""
                         << ",\"description\":" << jsonString(r.description) << "}";
                }
                json << "]}";
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", json.str(), corsOrigin);
            } else if (method == "POST" && path == "/api/reflog/restore") {
                JsonValue bodyJson;
                std::string parseErr, hash;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString("Invalid JSON: " + parseErr) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else if (!getRequiredString(bodyJson, "hash", hash, parseErr) || hash.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Hash cannot be empty\",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                } else {
                    try {
                        repo->checkout(hash);
                        std::string snap = getSnapJson();
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"message\":\"Restored to checkpoint " + hash.substr(0, 7) + "\",\"snapshot\":" + snap + "}", corsOrigin);
                    } catch (const std::exception& e) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                    }
                }
            } else if (method == "GET" && path == "/api/sync/bundle") {
                std::string bundle = repo->exportBundleJson();
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", bundle, corsOrigin);
            } else if (method == "POST" && path == "/api/sync/import-bundle") {
                std::string resJson = repo->importBundleJson(body);
                JsonValue parsedRes = JsonValue::parse(resJson);
                bool isOk = parsedRes.is_object() && parsedRes.contains("ok") && parsedRes["ok"].as_bool();
                std::string peerRefsJson = (parsedRes.is_object() && parsedRes.contains("peerRefs")) ? parsedRes["peerRefs"].serialize() : "[]";
                std::string snap = getSnapJson();
                if (isOk) {
                    std::string msg = (parsedRes.is_object() && parsedRes.contains("message")) ? parsedRes["message"].as_string() : "Import succeeded";
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                     "{\"ok\":true,\"message\":" + jsonString(msg) + ",\"peerRefs\":" + peerRefsJson + ",\"result\":" + resJson + ",\"snapshot\":" + snap + "}",
                                     corsOrigin);
                } else {
                    std::string err = (parsedRes.is_object() && parsedRes.contains("error")) ? parsedRes["error"].as_string() : "Import failed";
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString(err) + ",\"peerRefs\":" + peerRefsJson + ",\"result\":" + resJson + ",\"snapshot\":" + snap + "}",
                                     corsOrigin);
                }
            } else if (method == "GET" && path == "/api/git/status") {
                std::string statusJson = repo->gitStatus();
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", statusJson, corsOrigin);
            } else if (method == "POST" && path == "/api/git/export") {
                try {
                    std::string resMsg = repo->exportToGit();
                    std::string snap = getSnapJson();
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                     "{\"ok\":true,\"message\":" + jsonString(resMsg) + ",\"snapshot\":" + snap + "}", corsOrigin);
                } catch (const std::exception& e) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                }
            } else if (method == "POST" && path == "/api/git/import") {
                try {
                    std::string resMsg = repo->importFromGit();
                    std::string snap = getSnapJson();
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                     "{\"ok\":true,\"message\":" + jsonString(resMsg) + ",\"snapshot\":" + snap + "}", corsOrigin);
                } catch (const std::exception& e) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + getSnapJson() + "}", corsOrigin);
                }
            } else {
                sendHttpResponse(clientSock, 404, "Not Found", "application/json", "{\"ok\":false,\"error\":\"endpoint not found\"}", corsOrigin);
            }
        } catch (const MiniGitException& e) {
            std::string snapField = "";
            try { snapField = ",\"snapshot\":" + getSnapJson(); } catch (...) {}
            sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                             "{\"ok\":false,\"error\":" + jsonString(e.what()) + snapField + "}", corsOrigin);
        } catch (const std::exception& e) {
            std::string snapField = "";
            try { snapField = ",\"snapshot\":" + getSnapJson(); } catch (...) {}
            sendHttpResponse(clientSock, 500, "Internal Server Error", "application/json; charset=utf-8",
                             "{\"ok\":false,\"error\":" + jsonString(e.what()) + snapField + "}", corsOrigin);
        }

        CLOSESOCKET(clientSock);
}

void HttpServer::run(Repository* repo, int preferredPort, bool openBrowser, bool lan) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        throw MiniGitException("failed to initialize Windows sockets");
    }
#endif

    PairingManager pairingMgr;

    socket_t serverSock = INVALID_SOCKET;
    int boundPort = -1;

    for (int port = preferredPort; port < preferredPort + 30; ++port) {
        socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (!ISVALIDSOCKET(s)) continue;

        int opt = 1;
#ifdef _WIN32
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        if (lan) {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        }
        addr.sin_port = htons(static_cast<uint16_t>(port));

        if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == 0 && listen(s, 10) == 0) {
            serverSock = s;
            boundPort = port;
            break;
        }
        CLOSESOCKET(s);
    }

    if (!ISVALIDSOCKET(serverSock)) {
#ifdef _WIN32
        WSACleanup();
#endif
        throw MiniGitException("failed to bind server to a local port in range " +
                               std::to_string(preferredPort) + "-" + std::to_string(preferredPort + 30));
    }

    std::string repoName = repo->root().filename().string();
    std::string serverUrl = "http://127.0.0.1:" + std::to_string(boundPort) + "/";
    std::string lanIp = lan ? getLocalLanIp() : "127.0.0.1";
    std::string lanUrl = "http://" + lanIp + ":" + std::to_string(boundPort) + "/";

    std::cout << "\n============================================================\n"
              << "  Mini-Git Web Studio & Interactive Local Server\n"
              << "============================================================\n"
              << "  * Repository: " << repoName << "\n"
              << "  * Studio URL: " << serverUrl << "\n";
    if (lan) {
        std::cout << "  * P2P / LAN:  Enabled (0.0.0.0:" << boundPort << ")\n"
                  << "  * LAN URL:    " << lanUrl << "\n"
                  << "  * Pair Code:  " << pairingMgr.currentCode() << " (expires in 5 min)\n";
    } else {
        std::cout << "  * P2P / LAN:  Disabled (local only). Pass --lan to enable.\n";
    }
    std::cout << "  * Mode:       Point-and-click (Zero CLI commands required)\n"
              << "------------------------------------------------------------\n"
              << "  Ready! You can stage, commit, branch, diff and merge\n"
              << "  directly from your browser.\n"
              << "  Press Ctrl+C in terminal to stop server.\n"
              << "============================================================\n\n";

    if (openBrowser) {
        openBrowserUrl(serverUrl);
    }

    std::recursive_mutex repoMutex;
    std::recursive_mutex pairingMutex;
    std::atomic<int> activeConns{0};
    static constexpr int MAX_CONCURRENT_CONNS = 32;

    HttpServerThreadPool pool(8);

    while (true) {
        sockaddr_in clientAddr{};
#ifdef _WIN32
        int clientLen = sizeof(clientAddr);
#else
        socklen_t clientLen = sizeof(clientAddr);
#endif
        socket_t clientSock = accept(serverSock, (struct sockaddr*)&clientAddr, &clientLen);
        if (!ISVALIDSOCKET(clientSock)) break;

        if (activeConns.load() >= MAX_CONCURRENT_CONNS) {
            sendHttpResponse(clientSock, 503, "Service Unavailable", "application/json",
                             "{\"ok\":false,\"error\":\"Server busy, maximum connections reached\"}");
            CLOSESOCKET(clientSock);
            continue;
        }

        activeConns++;
        pool.enqueue([clientSock, clientAddr, boundPort, lan, &pairingMgr, repo, &repoMutex, &pairingMutex, repoName, lanUrl, &activeConns]() {
            handleClientConnection(clientSock, clientAddr, boundPort, lan, pairingMgr, repo, repoMutex, pairingMutex, repoName, lanUrl);
            activeConns--;
        });
    }

    CLOSESOCKET(serverSock);
#ifdef _WIN32
    WSACleanup();
#endif
}
