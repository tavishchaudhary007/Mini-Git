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

void sendHttpResponse(socket_t sock, int statusCode, const std::string& statusText,
                      const std::string& contentType, const std::string& body,
                      const std::string& origin = "", bool isSyncOrPairEndpoint = false,
                      bool lan = false) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n";
    if (lan && isSyncOrPairEndpoint && !origin.empty()) {
        oss << "Access-Control-Allow-Origin: " << origin << "\r\n"
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

        std::string origin = getHeaderValue(request, "Origin");
        bool isSyncEndpoint = (path == "/api/sync/bundle" || path == "/api/sync/import-bundle");
        bool isPairEndpoint = (path == "/api/pair" || path == "/api/pair/status");
        bool isSyncOrPairEndpoint = (isSyncEndpoint || isPairEndpoint);

        if (method == "OPTIONS") {
            sendHttpResponse(clientSock, 204, "No Content", "text/plain", "", origin, isSyncOrPairEndpoint, lan);
            CLOSESOCKET(clientSock);
            continue;
        }

        if (!isLoopback) {
            if (!isSyncOrPairEndpoint) {
                sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                 "{\"ok\":false,\"error\":\"Forbidden: non-loopback access restricted to sync/pair endpoints\"}",
                                 origin, isSyncOrPairEndpoint, lan);
                CLOSESOCKET(clientSock);
                continue;
            }
            if (isSyncEndpoint) {
                std::string clientToken = getHeaderValue(request, "X-MiniGit-Token");
                if (!pairingMgr.isValidSessionToken(clientToken)) {
                    sendHttpResponse(clientSock, 401, "Unauthorized", "application/json",
                                     "{\"ok\":false,\"error\":\"Unauthorized: invalid or revoked session token. Please pair with host.\"}",
                                     origin, isSyncOrPairEndpoint, lan);
                    CLOSESOCKET(clientSock);
                    continue;
                }
            }
        }

        try {
            if (method == "GET" && (path == "/" || path == "/index.html")) {
                std::string html = HtmlExporter::render(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "text/html; charset=utf-8", html, origin, isSyncOrPairEndpoint, lan);
            } else if (method == "GET" && path == "/api/token") {
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    std::string tokenJson = "{\"ok\":true,\"code\":\"" + pairingMgr.currentCode() +
                                            "\",\"remainingSeconds\":" + std::to_string(pairingMgr.remainingSeconds()) +
                                            ",\"isExpired\":" + (pairingMgr.isExpired() ? "true" : "false") +
                                            ",\"isLocked\":" + (pairingMgr.isLocked() ? "true" : "false") +
                                            ",\"isUsed\":" + (pairingMgr.isUsed() ? "true" : "false") +
                                            ",\"lan\":" + (lan ? "true" : "false") +
                                            ",\"lanUrl\":\"" + lanUrl + "\"}";
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", tokenJson, origin, isSyncOrPairEndpoint, lan);
                }
            } else if (method == "GET" && path == "/api/pair/code") {
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    std::string codeJson = "{\"ok\":true,\"code\":\"" + pairingMgr.currentCode() +
                                           "\",\"remainingSeconds\":" + std::to_string(pairingMgr.remainingSeconds()) +
                                           ",\"isExpired\":" + (pairingMgr.isExpired() ? "true" : "false") +
                                           ",\"isLocked\":" + (pairingMgr.isLocked() ? "true" : "false") +
                                           ",\"isUsed\":" + (pairingMgr.isUsed() ? "true" : "false") +
                                           ",\"failedAttempts\":" + std::to_string(pairingMgr.failedAttempts()) +
                                           ",\"lan\":" + (lan ? "true" : "false") +
                                           ",\"lanUrl\":\"" + lanUrl + "\"}";
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", codeJson, origin, isSyncOrPairEndpoint, lan);
                }
            } else if (method == "POST" && path == "/api/pair/generate-code") {
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    std::string newCode = pairingMgr.generateNewCode();
                    std::string codeJson = "{\"ok\":true,\"code\":\"" + newCode +
                                           "\",\"remainingSeconds\":" + std::to_string(pairingMgr.remainingSeconds()) +
                                           ",\"isExpired\":false,\"isLocked\":false,\"isUsed\":false,\"failedAttempts\":0}";
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", codeJson, origin, isSyncOrPairEndpoint, lan);
                }
            } else if (method == "POST" && path == "/api/pair") {
                if (!lan) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"LAN mode is not enabled on host\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    JsonValue bodyJson;
                    std::string parseErr;
                    if (!parseRequestBody(body, bodyJson, parseErr)) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":\"Invalid JSON: " + parseErr + "\"}", origin, isSyncOrPairEndpoint, lan);
                        CLOSESOCKET(clientSock);
                        continue;
                    }
                    std::string code = getOptionalString(bodyJson, "code");
                    std::string peerName = getOptionalString(bodyJson, "peerName");
                    auto res = pairingMgr.submitCode(code, clientIpStr, peerName);
                    if (res.success) {
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"status\":\"pending\",\"requestId\":\"" + res.requestId +
                                         "\",\"message\":\"Pairing request submitted. Waiting for host approval.\"}",
                                         origin, isSyncOrPairEndpoint, lan);
                    } else {
                        int sc = res.locked ? 403 : 401;
                        sendHttpResponse(clientSock, sc, sc == 403 ? "Forbidden" : "Unauthorized", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":\"" + res.error + "\",\"locked\":" + (res.locked ? "true" : "false") +
                                         ",\"remainingAttempts\":" + std::to_string(res.remainingAttempts) + "}",
                                         origin, isSyncOrPairEndpoint, lan);
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
                                     "{\"ok\":true,\"status\":\"pending\"}", origin, isSyncOrPairEndpoint, lan);
                } else if (statusReq.status == PendingPairRequest::APPROVED) {
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                     "{\"ok\":true,\"status\":\"approved\",\"token\":\"" + statusReq.sessionToken + "\"}",
                                     origin, isSyncOrPairEndpoint, lan);
                } else {
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"status\":\"denied\",\"error\":\"Host denied the pairing request.\"}",
                                     origin, isSyncOrPairEndpoint, lan);
                }
            } else if (method == "GET" && path == "/api/pair/pending") {
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    auto pending = pairingMgr.pendingRequests();
                    std::ostringstream oss;
                    oss << "{\"ok\":true,\"pending\":[";
                    for (size_t i = 0; i < pending.size(); ++i) {
                        if (i > 0) oss << ",";
                        oss << "{\"id\":\"" << pending[i].requestId << "\",\"ip\":\"" << pending[i].clientIp
                            << "\",\"peerName\":\"" << pending[i].peerName << "\",\"code\":\"" << pending[i].code << "\"}";
                    }
                    oss << "]}";
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", oss.str(), origin, isSyncOrPairEndpoint, lan);
                }
            } else if (method == "POST" && path == "/api/pair/decide") {
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    JsonValue bodyJson;
                    std::string parseErr;
                    if (!parseRequestBody(body, bodyJson, parseErr)) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":\"Invalid JSON: " + parseErr + "\"}", origin, isSyncOrPairEndpoint, lan);
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
                                     origin, isSyncOrPairEndpoint, lan);
                }
            } else if (method == "GET" && path == "/api/pair/peers") {
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    auto peers = pairingMgr.listPeers();
                    std::ostringstream oss;
                    oss << "{\"ok\":true,\"peers\":[";
                    for (size_t i = 0; i < peers.size(); ++i) {
                        if (i > 0) oss << ",";
                        oss << "{\"id\":\"" << peers[i].id << "\",\"ip\":\"" << peers[i].clientIp
                            << "\",\"peerName\":\"" << peers[i].peerName << "\",\"pairedAt\":\"" << peers[i].pairedAt << "\"}";
                    }
                    oss << "]}";
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", oss.str(), origin, isSyncOrPairEndpoint, lan);
                }
            } else if (method == "POST" && path == "/api/pair/revoke") {
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    JsonValue bodyJson;
                    std::string parseErr;
                    if (!parseRequestBody(body, bodyJson, parseErr)) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":\"Invalid JSON: " + parseErr + "\"}", origin, isSyncOrPairEndpoint, lan);
                        CLOSESOCKET(clientSock);
                        continue;
                    }
                    std::string id = getOptionalString(bodyJson, "id");
                    std::string tok = getOptionalString(bodyJson, "token");
                    bool ok = pairingMgr.revokePeer(!id.empty() ? id : tok);
                    sendHttpResponse(clientSock, ok ? 200 : 404, ok ? "OK" : "Not Found", "application/json; charset=utf-8",
                                     "{\"ok\":" + std::string(ok ? "true" : "false") + ",\"message\":\"" +
                                     (ok ? "Peer access revoked" : "Peer not found") + "\"}",
                                     origin, isSyncOrPairEndpoint, lan);
                }
            } else if (method == "GET" && path == "/api/snapshot") {
                std::string json = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", json, origin, isSyncOrPairEndpoint, lan);
            } else if (method == "POST" && (path == "/api/file/save" || path == "/api/file/write")) {
                JsonValue bodyJson;
                std::string parseErr, p;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Invalid JSON: " + parseErr + "\"}", origin, isSyncOrPairEndpoint, lan);
                } else if (!getRequiredString(bodyJson, "path", p, parseErr) || p.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Path is required\"}", origin, isSyncOrPairEndpoint, lan);
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
                        std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"message\":\"saved\",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
                    } catch (const std::exception& e) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(e.what()) + "}", origin, isSyncOrPairEndpoint, lan);
                    }
                }
            } else if (method == "POST" && path == "/api/file/delete") {
                JsonValue bodyJson;
                std::string parseErr, p;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Invalid JSON: " + parseErr + "\"}", origin, isSyncOrPairEndpoint, lan);
                } else if (!getRequiredString(bodyJson, "path", p, parseErr) || p.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Path is required\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    try {
                        std::string rel = repo->resolveRepoPath(p, true);
                        fs::path full = repo->root() / rel;
                        if (fs::exists(full)) {
                            std::error_code ec;
                            fs::remove(full, ec);
                        }
                        std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"message\":\"deleted\",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
                    } catch (const std::exception& e) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(e.what()) + "}", origin, isSyncOrPairEndpoint, lan);
                    }
                }
            } else if (method == "POST" && path == "/api/stage") {
                JsonValue bodyJson;
                std::string parseErr, p;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Invalid JSON: " + parseErr + "\"}", origin, isSyncOrPairEndpoint, lan);
                } else if (!getRequiredString(bodyJson, "path", p, parseErr) || p.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Path is required\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    try {
                        repo->add(p, true);
                        std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"message\":\"staged\",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
                    } catch (const std::exception& e) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(e.what()) + "}", origin, isSyncOrPairEndpoint, lan);
                    }
                }
            } else if (method == "POST" && path == "/api/unstage") {
                JsonValue bodyJson;
                std::string parseErr, p;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Invalid JSON: " + parseErr + "\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    try {
                        p = getOptionalString(bodyJson, "path");
                        repo->unstage(p, true);
                        std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"message\":\"unstaged\",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
                    } catch (const std::exception& e) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(e.what()) + "}", origin, isSyncOrPairEndpoint, lan);
                    }
                }
            } else if (method == "POST" && path == "/api/discard") {
                JsonValue bodyJson;
                std::string parseErr, p;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Invalid JSON: " + parseErr + "\"}", origin, isSyncOrPairEndpoint, lan);
                } else if (!getRequiredString(bodyJson, "path", p, parseErr) || p.empty() || p == "." || p == ".." || fs::path(p).is_absolute()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Invalid or missing path for discard\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    try {
                        repo->discard(p, true);
                        std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                        sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                         "{\"ok\":true,\"message\":\"discarded\",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
                    } catch (const std::exception& e) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":" + jsonString(e.what()) + "}", origin, isSyncOrPairEndpoint, lan);
                    }
                }
            } else if (method == "POST" && path == "/api/commit") {
                JsonValue bodyJson;
                std::string parseErr, msg;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Invalid JSON: " + parseErr + "\"}", origin, isSyncOrPairEndpoint, lan);
                } else if (!getRequiredString(bodyJson, "message", msg, parseErr) || msg.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Commit message cannot be empty\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    Hash h = repo->commit(msg);
                    std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                     "{\"ok\":true,\"hash\":\"" + h.str() + "\",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
                }
            } else if (method == "POST" && path == "/api/branch/create") {
                JsonValue bodyJson;
                std::string parseErr, name;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Invalid JSON: " + parseErr + "\"}", origin, isSyncOrPairEndpoint, lan);
                } else if (!getRequiredString(bodyJson, "name", name, parseErr) || name.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Branch name cannot be empty\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    repo->createBranch(name);
                    std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                     "{\"ok\":true,\"message\":\"created branch\",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
                }
            } else if (method == "POST" && path == "/api/branch/delete") {
                JsonValue bodyJson;
                std::string parseErr, name;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Invalid JSON: " + parseErr + "\"}", origin, isSyncOrPairEndpoint, lan);
                } else if (!getRequiredString(bodyJson, "name", name, parseErr) || name.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Branch name cannot be empty\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    repo->deleteBranch(name);
                    std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                     "{\"ok\":true,\"message\":\"deleted branch\",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
                }
            } else if (method == "POST" && path == "/api/checkout") {
                JsonValue bodyJson;
                std::string parseErr, target;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Invalid JSON: " + parseErr + "\"}", origin, isSyncOrPairEndpoint, lan);
                } else if (!getRequiredString(bodyJson, "target", target, parseErr) || target.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Target cannot be empty\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    repo->checkout(target);
                    std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                     "{\"ok\":true,\"message\":\"checked out\",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
                }
            } else if (method == "POST" && path == "/api/merge") {
                JsonValue bodyJson;
                std::string parseErr, branch;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Invalid JSON: " + parseErr + "\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    bool isAbort = (bodyJson.contains("abort") && bodyJson["abort"].is_bool() && bodyJson["abort"].as_bool());
                    if (!isAbort && bodyJson.contains("branch") && bodyJson["branch"].is_string() && bodyJson["branch"].as_string() == "--abort") {
                        isAbort = true;
                    }
                    if (isAbort) {
                        try {
                            std::string msg = repo->mergeAbort();
                            std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                            sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                             "{\"ok\":true,\"result\":" + jsonString(msg) + ",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
                        } catch (const MiniGitException& e) {
                            sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                             "{\"ok\":false,\"error\":" + jsonString(e.what()) + "}", origin, isSyncOrPairEndpoint, lan);
                        }
                    } else if (!getRequiredString(bodyJson, "branch", branch, parseErr) || branch.empty()) {
                        sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                         "{\"ok\":false,\"error\":\"Branch cannot be empty\"}", origin, isSyncOrPairEndpoint, lan);
                    } else {
                        try {
                            std::string msg = repo->merge(branch);
                            std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                            sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                             "{\"ok\":true,\"result\":" + jsonString(msg) + ",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
                        } catch (const MiniGitException& e) {
                            std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                            sendHttpResponse(clientSock, 409, "Conflict", "application/json; charset=utf-8",
                                             "{\"ok\":false,\"error\":" + jsonString(e.what()) + ",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
                        }
                    }
                }
            } else if (method == "POST" && path == "/api/revert") {
                JsonValue bodyJson;
                std::string parseErr, hash;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Invalid JSON: " + parseErr + "\"}", origin, isSyncOrPairEndpoint, lan);
                } else if (!getRequiredString(bodyJson, "hash", hash, parseErr) || hash.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Hash cannot be empty\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    Hash h = repo->revert(Hash(hash));
                    std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                     "{\"ok\":true,\"hash\":\"" + h.str() + "\",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
                }
            } else if (method == "POST" && path == "/api/undo") {
                std::string resMsg = repo->undo();
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":" + jsonString(resMsg) + ",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
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
                std::string resMsg = repo->stashSave(msg, includeUntracked);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":" + jsonString(resMsg) + ",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
            } else if (method == "POST" && path == "/api/stash/pop") {
                std::string resMsg = repo->stashPop();
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":" + jsonString(resMsg) + ",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
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
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", oss.str(), origin, isSyncOrPairEndpoint, lan);
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
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", json.str(), origin, isSyncOrPairEndpoint, lan);
            } else if (method == "POST" && path == "/api/reflog/restore") {
                JsonValue bodyJson;
                std::string parseErr, hash;
                if (!parseRequestBody(body, bodyJson, parseErr)) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Invalid JSON: " + parseErr + "\"}", origin, isSyncOrPairEndpoint, lan);
                } else if (!getRequiredString(bodyJson, "hash", hash, parseErr) || hash.empty()) {
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"Hash cannot be empty\"}", origin, isSyncOrPairEndpoint, lan);
                } else {
                    repo->checkout(hash);
                    std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                     "{\"ok\":true,\"message\":\"Restored to checkpoint " + hash.substr(0, 7) + "\",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
                }
            } else if (method == "GET" && path == "/api/sync/bundle") {
                std::string bundle = repo->exportBundleJson();
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", bundle, origin, isSyncOrPairEndpoint, lan);
            } else if (method == "POST" && path == "/api/sync/import-bundle") {
                std::string resJson = repo->importBundleJson(body);
                JsonValue parsedRes = JsonValue::parse(resJson);
                bool isOk = parsedRes.is_object() && parsedRes.contains("ok") && parsedRes["ok"].as_bool();
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                if (isOk) {
                    std::string msg = parsedRes["message"].as_string();
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                     "{\"ok\":true,\"message\":" + jsonString(msg) + ",\"result\":" + resJson + ",\"snapshot\":" + snap + "}",
                                     origin, isSyncOrPairEndpoint, lan);
                } else {
                    std::string err = parsedRes.contains("error") ? parsedRes["error"].as_string() : "Import failed";
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":" + jsonString(err) + ",\"result\":" + resJson + "}",
                                     origin, isSyncOrPairEndpoint, lan);
                }
            } else if (method == "GET" && path == "/api/git/status") {
                std::string statusJson = repo->gitStatus();
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", statusJson, origin, isSyncOrPairEndpoint, lan);
            } else if (method == "POST" && path == "/api/git/export") {
                std::string resMsg = repo->exportToGit();
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":" + jsonString(resMsg) + ",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
            } else if (method == "POST" && path == "/api/git/import") {
                std::string resMsg = repo->importFromGit();
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":" + jsonString(resMsg) + ",\"snapshot\":" + snap + "}", origin, isSyncOrPairEndpoint, lan);
            } else {
                sendHttpResponse(clientSock, 404, "Not Found", "application/json", "{\"ok\":false,\"error\":\"endpoint not found\"}", origin, isSyncOrPairEndpoint, lan);
            }
} catch (const MiniGitException& e) {
            std::string err = e.what();
            std::string cleanErr;
            for (char c : err) { if (c == '"') cleanErr += "\\\""; else cleanErr += c; }
            sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                             "{\"ok\":false,\"error\":\"" + cleanErr + "\"}", origin, isSyncOrPairEndpoint, lan);
        } catch (const std::exception& e) {
            std::string err = e.what();
            std::string cleanErr;
            for (char c : err) { if (c == '"') cleanErr += "\\\""; else cleanErr += c; }
            sendHttpResponse(clientSock, 500, "Internal Server Error", "application/json; charset=utf-8",
                             "{\"ok\":false,\"error\":\"" + cleanErr + "\"}", origin, isSyncOrPairEndpoint, lan);
        }

        CLOSESOCKET(clientSock);
    }

    CLOSESOCKET(serverSock);
#ifdef _WIN32
    WSACleanup();
#endif
}
