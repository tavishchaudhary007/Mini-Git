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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
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
#if defined(_WIN32)
    std::string cmd = "start \"\" \"" + url + "\"";
#elif defined(__APPLE__)
    std::string cmd = "open \"" + url + "\"";
#else
    std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1";
#endif
    return std::system(cmd.c_str()) == 0;
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

    while (true) {
        sockaddr_in clientAddr{};
#ifdef _WIN32
        int clientLen = sizeof(clientAddr);
#else
        socklen_t clientLen = sizeof(clientAddr);
#endif
        socket_t clientSock = accept(serverSock, (struct sockaddr*)&clientAddr, &clientLen);
        if (!ISVALIDSOCKET(clientSock)) break;

        char clientIpStr[INET_ADDRSTRLEN] = "127.0.0.1";
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIpStr, sizeof(clientIpStr));

        uint32_t clientIp = ntohl(clientAddr.sin_addr.s_addr);
        bool isLoopback = ((clientIp >> 24) == 127) || (clientIp == 0);

        std::string request;
        char buf[8192];
        int bytesRead = 0;
        bool payloadTooLarge = false;

        while ((bytesRead = recv(clientSock, buf, sizeof(buf) - 1, 0)) > 0) {
            buf[bytesRead] = '\0';
            request.append(buf, bytesRead);
            if (request.find("\r\n\r\n") != std::string::npos) {
                // Header complete. Check Content-Length case-insensitively
                std::string clStr = getHeaderValue(request, "Content-Length");
                if (!clStr.empty()) {
                    try {
                        size_t cl = std::stoull(clStr);
                        if (cl > 50 * 1024 * 1024) { // 50 MB limit
                            payloadTooLarge = true;
                            break;
                        }
                        size_t bodyStart = request.find("\r\n\r\n") + 4;
                        if (request.size() - bodyStart < cl) {
                            continue; // read more bytes
                        }
                    } catch (...) {}
                }
                break;
            }
        }

        if (payloadTooLarge) {
            sendHttpResponse(clientSock, 413, "Payload Too Large", "application/json",
                             "{\"ok\":false,\"error\":\"Payload Too Large (max 50MB)\"}");
            CLOSESOCKET(clientSock);
            continue;
        }

        if (request.empty()) {
            CLOSESOCKET(clientSock);
            continue;
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
            continue;
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
            continue;
        }

        std::string clientToken = getHeaderValue(request, "X-MiniGit-Token");
        bool hasValidToken = !clientToken.empty() && pairingMgr.isValidSessionToken(clientToken);

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
                continue;
            }
            if (isSyncEndpoint && !hasValidToken) {
                sendHttpResponse(clientSock, 401, "Unauthorized", "application/json",
                                 "{\"ok\":false,\"error\":\"Unauthorized: invalid or revoked session token. Please pair with host.\"}",
                                 corsOrigin);
                CLOSESOCKET(clientSock);
                continue;
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
                    continue;
                }
            }
            std::string ct = getHeaderValue(request, "Content-Type");
            if (ct.find("application/json") == std::string::npos) {
                sendHttpResponse(clientSock, 400, "Bad Request", "application/json",
                                 "{\"ok\":false,\"error\":\"Content-Type must be application/json\"}", corsOrigin);
                CLOSESOCKET(clientSock);
                continue;
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
                continue;
            }
        }

        auto getSnapJson = [&]() {
            try {
                Snapshot s = repo->snapshot();
                s.lan = lan;
                s.lanUrl = lanUrl;
                return HtmlExporter::toJson(s, repoName);
            } catch (...) {
                return std::string("{}");
            }
        };

        try {
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
                                            ",\"isLocked\":" + (pairingMgr.isLocked() ? "true" : "false") +
                                            ",\"isUsed\":" + (pairingMgr.isUsed() ? "true" : "false") +
                                            ",\"lan\":" + (lan ? "true" : "false") +
                                            ",\"lanUrl\":\"" + lanUrl + "\"}";
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", tokenJson, corsOrigin);
                }
            } else if (method == "GET" && path == "/api/pair/code") {
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", corsOrigin);
                } else {
                    std::string codeJson = "{\"ok\":true,\"code\":\"" + pairingMgr.currentCode() +
                                           "\",\"remainingSeconds\":" + std::to_string(pairingMgr.remainingSeconds()) +
                                           ",\"isExpired\":" + (pairingMgr.isExpired() ? "true" : "false") +
                                           ",\"isLocked\":" + (pairingMgr.isLocked() ? "true" : "false") +
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
                                           ",\"isExpired\":false,\"isLocked\":false,\"isUsed\":false,\"failedAttempts\":0}";
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", codeJson, corsOrigin);
                }
            } else if (method == "POST" && path == "/api/pair") {
                if (!lan && !isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"LAN mode is not enabled on host\"}", corsOrigin);
                } else {
                    JsonValue bodyJson;
                    std::string parseErr;
                    if (!parseRequestBody(body, bodyJson, parseErr)) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString("Invalid JSON: " + parseErr) + "}", corsOrigin);
                        CLOSESOCKET(clientSock);
                        continue;
                    }
                    std::string code = getOptionalString(bodyJson, "code");
                    std::string peerName = getOptionalString(bodyJson, "peerName");
                    if (peerName.empty()) peerName = getOptionalString(bodyJson, "clientName");
                    auto res = pairingMgr.submitCode(code, clientIpStr, peerName);
                    if (res.success) {
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
                }
            } else if (method == "GET" && path == "/api/pair/status") {
                std::string reqId = getQueryParam(query, "requestId");
                if (reqId.empty() && !body.empty()) {
                    JsonValue bj;
                    std::string pe;
                    if (parseRequestBody(body, bj, pe)) reqId = getOptionalString(bj, "requestId");
                }
                auto statusReq = pairingMgr.getRequestStatus(reqId);
                if (statusReq.status == PendingPairRequest::PENDING) {
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
                        continue;
                    }
                    std::string reqId = getOptionalString(bodyJson, "requestId");
                    std::string action = getOptionalString(bodyJson, "action");
                    std::string label = getOptionalString(bodyJson, "label");
                    bool allowed = (action == "allow" || action == "approve");
                    bool ok = pairingMgr.decideRequest(reqId, allowed, label);
                    sendHttpResponse(clientSock, ok ? 200 : 400, ok ? "OK" : "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":" + std::string(ok ? "true" : "false") + ",\"action\":\"" + action + "\"}",
                                     corsOrigin);
                }
            } else if (method == "GET" && path == "/api/pair/peers") {
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", corsOrigin);
                } else {
                    auto peers = pairingMgr.listPeers();
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
                        continue;
                    }
                    std::string id = getOptionalString(bodyJson, "id");
                    std::string tok = getOptionalString(bodyJson, "token");
                    bool ok = pairingMgr.revokePeer(!id.empty() ? id : tok);
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
                            continue;
                        }
                        std::error_code ec;
                        fs::remove(full, ec);
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
                                continue;
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

    CLOSESOCKET(serverSock);
#ifdef _WIN32
    WSACleanup();
#endif
}
