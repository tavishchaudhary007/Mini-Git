#include "HttpServer.h"
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include "HtmlExporter.h"
#include "Exceptions.h"

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
                      const std::string& contentType, const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        << "Access-Control-Allow-Headers: Content-Type\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    sendAll(sock, oss.str());
}

} // namespace

void HttpServer::run(Repository* repo, int preferredPort, bool openBrowser) {
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
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
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

    std::cout << "\n============================================================\n"
              << "  Mini-Git Web Studio & Interactive Local Server\n"
              << "============================================================\n"
              << "  * Repository: " << repoName << "\n"
              << "  * Studio URL: " << serverUrl << "\n"
              << "  * P2P / LAN:  Listening on 0.0.0.0:" << boundPort << " (Wi-Fi sync ready)\n"
              << "  * Mode:       Point-and-click (Zero CLI commands required)\n"
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

        std::string request;
        char buf[4096];
        int bytesRead = 0;
        while ((bytesRead = recv(clientSock, buf, sizeof(buf) - 1, 0)) > 0) {
            buf[bytesRead] = '\0';
            request.append(buf, bytesRead);
            if (request.find("\r\n\r\n") != std::string::npos) {
                // Header complete. Check for Content-Length for POST body
                size_t clPos = request.find("Content-Length:");
                if (clPos == std::string::npos) clPos = request.find("content-length:");
                if (clPos != std::string::npos) {
                    size_t numStart = clPos + 15;
                    size_t lineEnd = request.find("\r\n", numStart);
                    int cl = std::atoi(request.substr(numStart, lineEnd - numStart).c_str());
                    size_t bodyStart = request.find("\r\n\r\n") + 4;
                    if (request.size() - bodyStart < static_cast<size_t>(cl)) {
                        continue; // read more
                    }
                }
                break;
            }
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

        if (method == "OPTIONS") {
            sendHttpResponse(clientSock, 204, "No Content", "text/plain", "");
            CLOSESOCKET(clientSock);
            continue;
        }

        try {
            if (method == "GET" && (uri == "/" || uri == "/index.html")) {
                std::string html = HtmlExporter::render(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "text/html; charset=utf-8", html);
            } else if (method == "GET" && uri == "/api/snapshot") {
                std::string json = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", json);
            } else if (method == "POST" && uri == "/api/stage") {
                std::string path = getJsonString(body, "path");
                repo->add(path);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"staged\",\"snapshot\":" + snap + "}");
            } else if (method == "POST" && uri == "/api/unstage") {
                std::string path = getJsonString(body, "path");
                repo->unstage(path);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"unstaged\",\"snapshot\":" + snap + "}");
            } else if (method == "POST" && uri == "/api/discard") {
                std::string path = getJsonString(body, "path");
                repo->discard(path);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"discarded\",\"snapshot\":" + snap + "}");
            } else if (method == "POST" && uri == "/api/commit") {
                std::string msg = getJsonString(body, "message");
                Hash h = repo->commit(msg);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"hash\":\"" + h.str() + "\",\"snapshot\":" + snap + "}");
            } else if (method == "POST" && uri == "/api/branch/create") {
                std::string name = getJsonString(body, "name");
                repo->createBranch(name);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"created branch\",\"snapshot\":" + snap + "}");
            } else if (method == "POST" && uri == "/api/branch/delete") {
                std::string name = getJsonString(body, "name");
                repo->deleteBranch(name);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"deleted branch\",\"snapshot\":" + snap + "}");
            } else if (method == "POST" && uri == "/api/checkout") {
                std::string target = getJsonString(body, "target");
                repo->checkout(target);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"checked out\",\"snapshot\":" + snap + "}");
            } else if (method == "POST" && uri == "/api/merge") {
                std::string branch = getJsonString(body, "branch");
                std::string msg = repo->merge(branch);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"result\":\"" + msg + "\",\"snapshot\":" + snap + "}");
            } else if (method == "POST" && uri == "/api/revert") {
                std::string hash = getJsonString(body, "hash");
                Hash h = repo->revert(Hash(hash));
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"hash\":\"" + h.str() + "\",\"snapshot\":" + snap + "}");
            } else if (method == "POST" && uri == "/api/undo") {
                std::string resMsg = repo->undo();
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"" + resMsg + "\",\"snapshot\":" + snap + "}");
            } else if (method == "POST" && uri == "/api/stash/save") {
                std::string msg = getJsonString(body, "message");
                std::string resMsg = repo->stashSave(msg);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"" + resMsg + "\",\"snapshot\":" + snap + "}");
            } else if (method == "POST" && uri == "/api/stash/pop") {
                std::string resMsg = repo->stashPop();
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"" + resMsg + "\",\"snapshot\":" + snap + "}");
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
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", json.str());
            } else if (method == "POST" && uri == "/api/reflog/restore") {
                std::string hash = getJsonString(body, "hash");
                repo->checkout(hash);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"Restored to checkpoint " + hash.substr(0, 7) + "\",\"snapshot\":" + snap + "}");
            } else if (method == "GET" && uri == "/api/sync/bundle") {
                std::string bundle = repo->exportBundleJson();
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", bundle);
            } else if (method == "POST" && uri == "/api/sync/import-bundle") {
                std::string resMsg = repo->importBundleJson(body);
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"" + resMsg + "\",\"snapshot\":" + snap + "}");
            } else if (method == "GET" && uri == "/api/git/status") {
                std::string statusJson = repo->gitStatus();
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8", statusJson);
            } else if (method == "POST" && uri == "/api/git/export") {
                std::string resMsg = repo->exportToGit();
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"" + resMsg + "\",\"snapshot\":" + snap + "}");
            } else if (method == "POST" && uri == "/api/git/import") {
                std::string resMsg = repo->importFromGit();
                std::string snap = HtmlExporter::toJson(repo->snapshot(), repoName);
                sendHttpResponse(clientSock, 200, "OK", "application/json; charset=utf-8",
                                 "{\"ok\":true,\"message\":\"" + resMsg + "\",\"snapshot\":" + snap + "}");
            } else {
                sendHttpResponse(clientSock, 404, "Not Found", "application/json", "{\"ok\":false,\"error\":\"endpoint not found\"}");
            }
        } catch (const MiniGitException& e) {
            std::string err = e.what();
            // escape quotes in error
            std::string cleanErr;
            for (char c : err) { if (c == '"') cleanErr += "\\\""; else cleanErr += c; }
            sendHttpResponse(clientSock, 400, "Bad Request", "application/json; charset=utf-8",
                             "{\"ok\":false,\"error\":\"" + cleanErr + "\"}");
        } catch (const std::exception& e) {
            std::string err = e.what();
            std::string cleanErr;
            for (char c : err) { if (c == '"') cleanErr += "\\\""; else cleanErr += c; }
            sendHttpResponse(clientSock, 500, "Internal Server Error", "application/json; charset=utf-8",
                             "{\"ok\":false,\"error\":\"" + cleanErr + "\"}");
        }

        CLOSESOCKET(clientSock);
    }

    CLOSESOCKET(serverSock);
#ifdef _WIN32
    WSACleanup();
#endif
}
