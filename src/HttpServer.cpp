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

std::string generatePairingToken() {
    static const char chars[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::string token = "mg_";
    for (int i = 0; i < 24; ++i) token += chars[dis(gen)];
    return token;
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

std::string getJsonString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    size_t start = json.find('"', pos);
    if (start == std::string::npos) return "";
    ++start;
    std::string val;
    for (size_t i = start; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            val += json[++i];
        } else if (json[i] == '"') {
            break;
        } else {
            val += json[i];
        }
    }
    return val;
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
                      const std::string& origin = "", bool isSyncEndpoint = false,
                      bool lan = false) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n";
    if (lan && isSyncEndpoint && !origin.empty()) {
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
    std::string token = lan ? generatePairingToken() : "";

    std::cout << "\n============================================================\n"
              << "  Mini-Git Web Studio & Interactive Local Server\n"
              << "============================================================\n"
              << "  * Repository: " << repoName << "\n"
              << "  * Studio URL: " << serverUrl << "\n";
    if (lan) {
        std::cout << "  * P2P / LAN:  Enabled (0.0.0.0:" << boundPort << ")\n"
                  << "  * LAN URL:    " << lanUrl << "\n"
                  << "  * Pair Token: " << token << "\n";
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

        // Extract body
        std::string body;
        size_t bodyPos = request.find("\r\n\r\n");
        if (bodyPos != std::string::npos) {
            body = request.substr(bodyPos + 4);
        }

        std::string origin = getHeaderValue(request, "Origin");
        bool isSyncEndpoint = (uri == "/api/sync/bundle" || uri == "/api/sync/import-bundle");

        if (method == "OPTIONS") {
            sendHttpResponse(clientSock, 204, "No Content", "text/plain", "", origin, isSyncEndpoint, lan);
            CLOSESOCKET(clientSock);
            continue;
        }

        if (!isLoopback) {
            if (!isSyncEndpoint) {
                sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                 "{\"ok\":false,\"error\":\"Forbidden: non-loopback access restricted to sync endpoints\"}",
                                 origin, isSyncEndpoint, lan);
                CLOSESOCKET(clientSock);
                continue;
            }
            std::string clientToken = getHeaderValue(request, "X-MiniGit-Token");
            if (token.empty() || clientToken != token) {
                sendHttpResponse(clientSock, 401, "Unauthorized", "application/json",
                                 "{\"ok\":false,\"error\":\"Unauthorized: invalid or missing X-MiniGit-Token\"}",
                                 origin, isSyncEndpoint, lan);
                CLOSESOCKET(clientSock);
                continue;
            }
        }

        try {
            if (method == "GET" && (uri == "/" || uri == "/index.html")) {
                std::string html = HtmlExporter::render(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "text/html; charset=utf-8", html, origin, isSyncEndpoint, lan);
            } else if (method == "GET" && uri == "/api/token") {
                if (!isLoopback) {
                    sendHttpResponse(clientSock, 403, "Forbidden", "application/json",
                                     "{\"ok\":false,\"error\":\"Forbidden\"}", origin, isSyncEndpoint, lan);
                } else {
                    std::string tokenJson = "{\"ok\":true,\"token\":\"" + token +
                                            "\",\"lan\":" + (lan ? "true" : "false") +
                                            ",\"lanUrl\":\"" + lanUrl + "\"}";
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", tokenJson, origin, isSyncEndpoint, lan);
                }
            } else if (method == "GET" && uri == "/api/snapshot") {
                std::string json = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", json, origin, isSyncEndpoint, lan);
            } else if (method == "POST" && uri == "/api/stage") {
                std::string path = getJsonString(body, "path");
                repo->add(path);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"staged\",\"snapshot\":" + snap + "}", origin, isSyncEndpoint, lan);
            } else if (method == "POST" && uri == "/api/unstage") {
                std::string path = getJsonString(body, "path");
                repo->unstage(path);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"unstaged\",\"snapshot\":" + snap + "}", origin, isSyncEndpoint, lan);
            } else if (method == "POST" && uri == "/api/discard") {
                std::string path = getJsonString(body, "path");
                repo->discard(path);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"discarded\",\"snapshot\":" + snap + "}", origin, isSyncEndpoint, lan);
            } else if (method == "POST" && uri == "/api/commit") {
                std::string msg = getJsonString(body, "message");
                Hash h = repo->commit(msg);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"hash\":\"" + h.str() + "\",\"snapshot\":" + snap + "}", origin, isSyncEndpoint, lan);
            } else if (method == "POST" && uri == "/api/branch/create") {
                std::string name = getJsonString(body, "name");
                repo->createBranch(name);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"created branch\",\"snapshot\":" + snap + "}", origin, isSyncEndpoint, lan);
            } else if (method == "POST" && uri == "/api/branch/delete") {
                std::string name = getJsonString(body, "name");
                repo->deleteBranch(name);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"deleted branch\",\"snapshot\":" + snap + "}", origin, isSyncEndpoint, lan);
            } else if (method == "POST" && uri == "/api/checkout") {
                std::string target = getJsonString(body, "target");
                repo->checkout(target);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"checked out\",\"snapshot\":" + snap + "}", origin, isSyncEndpoint, lan);
            } else if (method == "POST" && uri == "/api/merge") {
                std::string branch = getJsonString(body, "branch");
                std::string msg = repo->merge(branch);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"result\":\"" + msg + "\",\"snapshot\":" + snap + "}", origin, isSyncEndpoint, lan);
            } else if (method == "POST" && uri == "/api/revert") {
                std::string hash = getJsonString(body, "hash");
                Hash h = repo->revert(Hash(hash));
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"hash\":\"" + h.str() + "\",\"snapshot\":" + snap + "}", origin, isSyncEndpoint, lan);
            } else if (method == "POST" && uri == "/api/undo") {
                std::string resMsg = repo->undo();
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"" + resMsg + "\",\"snapshot\":" + snap + "}", origin, isSyncEndpoint, lan);
            } else if (method == "POST" && uri == "/api/stash/save") {
                std::string msg = getJsonString(body, "message");
                std::string resMsg = repo->stashSave(msg);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"" + resMsg + "\",\"snapshot\":" + snap + "}", origin, isSyncEndpoint, lan);
            } else if (method == "POST" && uri == "/api/stash/pop") {
                std::string resMsg = repo->stashPop();
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"" + resMsg + "\",\"snapshot\":" + snap + "}", origin, isSyncEndpoint, lan);
            } else if (method == "GET" && uri == "/api/reflog") {
                auto entries = repo->reflog();
                std::ostringstream json;
                json << "{\"ok\":true,\"reflog\":[";
                bool first = true;
                for (const auto& r : entries) {
                    if (!first) json << ',';
                    first = false;
                    json << "{\"timestamp\":\"" << r.timestamp << "\","
                         << "\"action\":\"" << r.action << "\","
                         << "\"oldHash\":\"" << r.oldHash << "\","
                         << "\"newHash\":\"" << r.newHash << "\","
                         << "\"description\":\"" << r.description << "\"}";
                }
                json << "]}";
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", json.str(), origin, isSyncEndpoint, lan);
            } else if (method == "POST" && uri == "/api/reflog/restore") {
                std::string hash = getJsonString(body, "hash");
                repo->checkout(hash);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"Restored to checkpoint " + hash.substr(0, 7) + "\",\"snapshot\":" + snap + "}", origin, isSyncEndpoint, lan);
            } else if (method == "GET" && uri == "/api/sync/bundle") {
                std::string bundle = repo->exportBundleJson();
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", bundle, origin, isSyncEndpoint, lan);
            } else if (method == "POST" && uri == "/api/sync/import-bundle") {
                std::string resJson = repo->importBundleJson(body);
                JsonValue parsedRes = JsonValue::parse(resJson);
                bool isOk = parsedRes.is_object() && parsedRes.contains("ok") && parsedRes["ok"].as_bool();
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                if (isOk) {
                    std::string msg = parsedRes["message"].as_string();
                    sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                     "{\"ok\":true,\"message\":\"" + msg + "\",\"result\":" + resJson + ",\"snapshot\":" + snap + "}",
                                     origin, isSyncEndpoint, lan);
                } else {
                    std::string err = parsedRes.contains("error") ? parsedRes["error"].as_string() : "Import failed";
                    sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                                     "{\"ok\":false,\"error\":\"" + err + "\",\"result\":" + resJson + "}",
                                     origin, isSyncEndpoint, lan);
                }
            } else if (method == "GET" && uri == "/api/git/status") {
                std::string statusJson = repo->gitStatus();
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", statusJson, origin, isSyncEndpoint, lan);
            } else if (method == "POST" && uri == "/api/git/export") {
                std::string resMsg = repo->exportToGit();
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"" + resMsg + "\",\"snapshot\":" + snap + "}", origin, isSyncEndpoint, lan);
            } else if (method == "POST" && uri == "/api/git/import") {
                std::string resMsg = repo->importFromGit();
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"" + resMsg + "\",\"snapshot\":" + snap + "}", origin, isSyncEndpoint, lan);
            } else {
                sendHttpResponse(clientSock, 404, "Not Found", "application/json", "{\"ok\":false,\"error\":\"endpoint not found\"}", origin, isSyncEndpoint, lan);
            }
        } catch (const MiniGitException& e) {
            std::string err = e.what();
            std::string cleanErr;
            for (char c : err) { if (c == '"') cleanErr += "\\\""; else cleanErr += c; }
            sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                             "{\"ok\":false,\"error\":\"" + cleanErr + "\"}", origin, isSyncEndpoint, lan);
        } catch (const std::exception& e) {
            std::string err = e.what();
            std::string cleanErr;
            for (char c : err) { if (c == '"') cleanErr += "\\\""; else cleanErr += c; }
            sendHttpResponse(clientSock, 500, "Internal Server Error", "application/json; charset=utf-8",
                             "{\"ok\":false,\"error\":\"" + cleanErr + "\"}", origin, isSyncEndpoint, lan);
        }

        CLOSESOCKET(clientSock);
    }

    CLOSESOCKET(serverSock);
#ifdef _WIN32
    WSACleanup();
#endif
}
