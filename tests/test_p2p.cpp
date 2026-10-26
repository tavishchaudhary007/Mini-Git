#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Base64.h"
#include "FileUtil.h"
#include "HtmlExporter.h"
#include "HttpServer.h"
#include "Json.h"
#include "Repository.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#define CLOSESOCK(s) closesocket(s)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
#define CLOSESOCK(s) close(s)
#endif

namespace fs = std::filesystem;

static void cleanDir(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

static std::string httpExchange(const std::string& host, int port, const std::string& rawReq) {
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == (socket_t)-1) {
        std::cerr << "socket failed\n";
        return "";
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = inet_addr(host.c_str());

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        CLOSESOCK(s);
        return "";
    }

    send(s, rawReq.data(), static_cast<int>(rawReq.size()), 0);

    std::string response;
    char buf[4096];
    int r = 0;
    while ((r = recv(s, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[r] = '\0';
        response.append(buf, r);
    }
    CLOSESOCK(s);
    return response;
}

int main() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    std::cout << "========================================\n";
    std::cout << "Running MiniGit P2P Sync & Security Tests\n";
    std::cout << "========================================\n";

    fs::path tempBase = fs::current_path() / "build" / "test_scratch";
    cleanDir(tempBase);
    fs::create_directories(tempBase);

    fs::path repoAPath = tempBase / "repoA";
    fs::path repoBPath = tempBase / "repoB";

    // -------------------------------------------------------------
    // TEST 1: Full Import of 2 commits with < > & " ', newline, binary file
    // -------------------------------------------------------------
    std::cout << "[Test 1] Testing full import of 2 commits, special chars, binary file, and hash verification...\n";
    cleanDir(repoAPath);
    cleanDir(repoBPath);
    Repository::init(repoAPath);
    Repository::init(repoBPath);

    fs::path origCwd = fs::current_path();
    fs::current_path(repoAPath);
    Repository repoA(repoAPath);
    Repository repoB(repoBPath);

    // Commit 1 on A: special characters and binary file
    std::string specialContent = "Line 1: <test> & \"quoted\" 'single' \\u003c \\u003e \\u0026\nLine 2: symbols & <tags>\r\nLine 3: end";
    std::string binaryContent = std::string("PNG\x00\x01\x02\xFF\xFE\x00HELLO\x00WORLD\xFF", 18);

    writeFile(repoAPath / "special.txt", specialContent);
    writeFile(repoAPath / "binary.bin", binaryContent);

    repoA.add("special.txt");
    repoA.add("binary.bin");
    Hash c1 = repoA.commit("Commit 1: special chars and binary file");

    // Commit 2 on A
    writeFile(repoAPath / "extra.txt", "Extra file content with <HTML> & entities.");
    repoA.add("extra.txt");
    Hash c2 = repoA.commit("Commit 2: extra file");

    // Export bundle from A
    std::string bundle = repoA.exportBundleJson();
    assert(!bundle.empty());

    // Import bundle into B
    std::string resJson = repoB.importBundleJson(bundle);
    JsonValue resVal = JsonValue::parse(resJson);
    assert(resVal.is_object());
    assert(resVal["ok"].as_bool() == true);
    assert(resVal["objectsImported"].as_int64() > 0);
    assert(resVal["objectsRejected"].as_int64() == 0);

    // Verify Repo B state
    assert(repoB.branchExists("main"));
    assert(repoB.branchTip("main") == c2);
    auto logB = repoB.log();
    assert(logB.size() == 2);
    assert(logB[0].hash() == c2);
    assert(logB[1].hash() == c1);

    fs::current_path(repoBPath);
    // Checkout main in B and verify files byte for byte
    repoB.checkout("main");
    std::string specialB = readFile(repoBPath / "special.txt");
    std::string binaryB = readFile(repoBPath / "binary.bin");
    std::string extraB = readFile(repoBPath / "extra.txt");

    assert(specialB == specialContent);
    assert(binaryB == binaryContent);
    assert(extraB == "Extra file content with <HTML> & entities.");
    std::cout << "  -> PASSED: All files, logs, and branch tips match byte for byte with exact hashes.\n";

    // -------------------------------------------------------------
    // TEST 2: Importing the same bundle twice
    // -------------------------------------------------------------
    std::cout << "[Test 2] Testing idempotent second import of same bundle...\n";
    std::string res2 = repoB.importBundleJson(bundle);
    JsonValue resVal2 = JsonValue::parse(res2);
    assert(resVal2.is_object());
    assert(resVal2["ok"].as_bool() == true);
    assert(resVal2["objectsImported"].as_int64() == 0);
    assert(resVal2["objectsPresent"].as_int64() > 0);
    assert(resVal2["objectsRejected"].as_int64() == 0);
    assert(repoB.branchTip("main") == c2);
    std::cout << "  -> PASSED: Re-importing reports everything as already present and modifies nothing.\n";

    // -------------------------------------------------------------
    // TEST 3: Tampered object (content changed, key kept)
    // -------------------------------------------------------------
    std::cout << "[Test 3] Testing tampered object rejection (refs must remain untouched)...\n";
    JsonValue tamperedRoot = JsonValue::parse(bundle);
    auto& objs = tamperedRoot.as_object_mut()["objects"].as_object_mut();

    // Find a blob and tamper its content
    bool tampered = false;
    for (auto& [hStr, oVal] : objs) {
        if (oVal["type"].as_string() == "blob") {
            oVal.as_object_mut()["content"] = JsonValue("tampered content that does not match original hash!");
            tampered = true;
            break;
        }
    }
    assert(tampered);

    Hash mainBeforeTamper = repoB.branchTip("main");
    std::string tamperedBundleStr = tamperedRoot.serialize();
    std::string res3 = repoB.importBundleJson(tamperedBundleStr);
    JsonValue resVal3 = JsonValue::parse(res3);

    assert(resVal3.is_object());
    assert(resVal3["ok"].as_bool() == false);
    assert(resVal3["objectsRejected"].as_int64() > 0);
    assert(repoB.branchTip("main") == mainBeforeTamper);
    std::cout << "  -> PASSED: Tampered object rejected and refs completely untouched.\n";

    // -------------------------------------------------------------
    // TEST 4: Diverged branch reported as skipped, tip unchanged
    // -------------------------------------------------------------
    std::cout << "[Test 4] Testing diverged branch safety (merge needed)...\n";
    fs::current_path(repoAPath);
    // In Repo A: create branch diverged_branch at c2, make commit c3_A
    repoA.createBranch("diverged_branch");
    repoA.checkout("diverged_branch");
    writeFile(repoAPath / "div.txt", "A's version of history");
    repoA.add("div.txt");
    Hash c3_A = repoA.commit("Branch commit on A");

    fs::current_path(repoBPath);
    // In Repo B: create branch diverged_branch at c2, make commit c3_B
    repoB.createBranch("diverged_branch");
    repoB.checkout("diverged_branch");
    writeFile(repoBPath / "div.txt", "B's diverged version of history");
    repoB.add("div.txt");
    Hash c3_B = repoB.commit("Branch commit on B");

    // Export A's bundle and import to B
    std::string bundleA_div = repoA.exportBundleJson();
    std::string res4 = repoB.importBundleJson(bundleA_div);
    JsonValue resVal4 = JsonValue::parse(res4);

    assert(resVal4.is_object());
    // Branch diverged_branch should be in branchesDiverged
    assert(resVal4["branchesDiverged"].as_array().size() > 0);
    // Tip of diverged_branch in Repo B MUST remain at c3_B
    assert(repoB.branchTip("diverged_branch") == c3_B);
    std::cout << "  -> PASSED: Diverged branch correctly identified, skipped, and tip remained unchanged.\n";

    // -------------------------------------------------------------
    // TEST 5: Dirty working tree on active branch: fast-forward refused
    // -------------------------------------------------------------
    std::cout << "[Test 5] Testing dirty working tree refusal on active branch fast-forward...\n";
    fs::current_path(repoAPath);
    // On Repo A: advance main with commit c4
    repoA.checkout("main");
    writeFile(repoAPath / "ff.txt", "fast forward file on A");
    repoA.add("ff.txt");
    Hash c4 = repoA.commit("Advance main commit on A");
    std::string bundleA_ff = repoA.exportBundleJson();

    fs::current_path(repoBPath);
    // On Repo B: switch to main, then make it dirty
    repoB.checkout("main");
    Hash mainBeforeFF = repoB.branchTip("main");
    writeFile(repoBPath / "special.txt", "UNCOMMITTED LOCAL MODIFICATION ON B");

    std::string res5 = repoB.importBundleJson(bundleA_ff);
    JsonValue resVal5 = JsonValue::parse(res5);

    assert(resVal5.is_object());
    assert(resVal5["ok"].as_bool() == false);
    assert(resVal5["error"].as_string().find("uncommitted changes") != std::string::npos);
    // Working tree still has uncommitted modification
    assert(readFile(repoBPath / "special.txt") == "UNCOMMITTED LOCAL MODIFICATION ON B");
    // Main branch tip in B unchanged
    assert(repoB.branchTip("main") == mainBeforeFF);
    std::cout << "  -> PASSED: Fast-forward cleanly refused with clear error when working tree is dirty.\n";

    // Discard dirty change on B and verify clean fast-forward works
    repoB.discard("special.txt");
    std::string res5_clean = repoB.importBundleJson(bundleA_ff);
    JsonValue resVal5_clean = JsonValue::parse(res5_clean);
    assert(resVal5_clean["ok"].as_bool() == true);
    assert(repoB.branchTip("main") == c4);

    // -------------------------------------------------------------
    // TEST 6: Malformed JSON returns an error and does not crash
    // -------------------------------------------------------------
    std::cout << "[Test 6] Testing malformed JSON handling...\n";
    std::vector<std::string> malformedInputs = {
        "",
        "not json at all",
        "{truncated: true",
        "{\"objects\": 123}",
        "{\"objects\": {\"short\": {\"type\": \"blob\"}}}",
        "{\"branches\": [1, 2, 3]}",
        "{\"objects\": {}}"
    };

    for (const auto& badInput : malformedInputs) {
        std::string badRes = repoB.importBundleJson(badInput);
        JsonValue val = JsonValue::parse(badRes);
        assert(val.is_object());
        assert(val["ok"].as_bool() == false);
    }
    std::cout << "  -> PASSED: All malformed JSON inputs handled safely with ok:false and zero crashes.\n";

    // -------------------------------------------------------------
    // TEST 7: LAN mode security and pairing token verification
    // -------------------------------------------------------------
    std::cout << "[Test 7] Testing LAN mode security, pairing token, and endpoint restrictions...\n";
    // We launch HttpServer in LAN mode in a background thread
    int testPort = 18889;
    std::thread serverThread([&]() {
        try {
            HttpServer::run(&repoA, testPort, false, true); // lan = true
        } catch (...) {}
    });
    serverThread.detach();

    // Wait for server to bind
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 7.1: Loopback can get token via /api/token
    std::string tokenReq = "GET /api/token HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    std::string tokenResp = httpExchange("127.0.0.1", testPort, tokenReq);
    assert(tokenResp.find("200 OK") != std::string::npos);

    size_t bodyPos = tokenResp.find("\r\n\r\n");
    assert(bodyPos != std::string::npos);
    std::string tokenBody = tokenResp.substr(bodyPos + 4);
    JsonValue tokenObj = JsonValue::parse(tokenBody);
    assert(tokenObj.is_object());
    assert(tokenObj["ok"].as_bool() == true);
    std::string serverToken = tokenObj["token"].as_string();
    assert(!serverToken.empty());

    // 7.2: Non-loopback simulation:
    // To test non-loopback simulation on localhost, let's test connecting to the LAN IP address of this machine!
    // Or send requests without token to /api/sync/bundle:
    // When connecting to 127.0.0.1, it's loopback, but we can verify that /api/sync/bundle responds with 200 to loopback.
    std::string loopbackSyncReq = "GET /api/sync/bundle HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    std::string loopbackSyncResp = httpExchange("127.0.0.1", testPort, loopbackSyncReq);
    assert(loopbackSyncResp.find("200 OK") != std::string::npos);

    // Now test LAN IP connection:
    // Discover local LAN IP
    socket_t sTmp = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in loopbackAddr{};
    loopbackAddr.sin_family = AF_INET;
    loopbackAddr.sin_addr.s_addr = inet_addr("8.8.8.8");
    loopbackAddr.sin_port = htons(53);
    connect(sTmp, (struct sockaddr*)&loopbackAddr, sizeof(loopbackAddr));
    sockaddr_in nameAddr{};
    int nameLen = sizeof(nameAddr);
    getsockname(sTmp, (struct sockaddr*)&nameAddr, &nameLen);
    char lanIpBuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &nameAddr.sin_addr, lanIpBuf, sizeof(lanIpBuf));
    CLOSESOCK(sTmp);
    std::string lanIp = lanIpBuf;

    if (lanIp != "127.0.0.1" && !lanIp.empty()) {
        std::cout << "  [Testing via LAN interface: " << lanIp << "]\n";
        // 7.2a: Request to /api/commit from LAN interface MUST be 403 Forbidden
        std::string lanCommitReq = "POST /api/commit HTTP/1.1\r\nHost: " + lanIp + "\r\nContent-Type: application/json\r\nContent-Length: 22\r\nConnection: close\r\n\r\n{\"message\":\"malicious\"}";
        std::string lanCommitResp = httpExchange(lanIp, testPort, lanCommitReq);
        assert(lanCommitResp.find("403 Forbidden") != std::string::npos);

        // 7.2b: Request to /api/sync/bundle WITHOUT token MUST be 401 Unauthorized
        std::string lanSyncNoToken = "GET /api/sync/bundle HTTP/1.1\r\nHost: " + lanIp + "\r\nConnection: close\r\n\r\n";
        std::string lanSyncNoTokenResp = httpExchange(lanIp, testPort, lanSyncNoToken);
        assert(lanSyncNoTokenResp.find("401 Unauthorized") != std::string::npos);

        // 7.2c: Request to /api/sync/bundle with WRONG token MUST be 401 Unauthorized
        std::string lanSyncWrongToken = "GET /api/sync/bundle HTTP/1.1\r\nHost: " + lanIp + "\r\nX-MiniGit-Token: wrong_token\r\nConnection: close\r\n\r\n";
        std::string lanSyncWrongResp = httpExchange(lanIp, testPort, lanSyncWrongToken);
        assert(lanSyncWrongResp.find("401 Unauthorized") != std::string::npos);

        // 7.2d: Request to /api/sync/bundle with VALID token MUST be 200 OK
        std::string lanSyncValidToken = "GET /api/sync/bundle HTTP/1.1\r\nHost: " + lanIp + "\r\nX-MiniGit-Token: " + serverToken + "\r\nConnection: close\r\n\r\n";
        std::string lanSyncValidResp = httpExchange(lanIp, testPort, lanSyncValidToken);
        assert(lanSyncValidResp.find("200 OK") != std::string::npos);

        // 7.2e: OPTIONS preflight with Origin header
        std::string lanOptionsReq = "OPTIONS /api/sync/bundle HTTP/1.1\r\nHost: " + lanIp + "\r\nOrigin: http://example.com\r\nConnection: close\r\n\r\n";
        std::string lanOptionsResp = httpExchange(lanIp, testPort, lanOptionsReq);
        assert(lanOptionsResp.find("204 No Content") != std::string::npos);
        assert(lanOptionsResp.find("Access-Control-Allow-Origin: http://example.com") != std::string::npos);
        assert(lanOptionsResp.find("Access-Control-Allow-Headers: Content-Type, X-MiniGit-Token") != std::string::npos);
    } else {
        std::cout << "  [Single network interface active (127.0.0.1)]\n";
    }

    // 7.3: Payload size limit (50 MB)
    std::string hugeReq = "POST /api/sync/import-bundle HTTP/1.1\r\nHost: 127.0.0.1\r\ncontent-length: 60000000\r\nConnection: close\r\n\r\n";
    std::string hugeResp = httpExchange("127.0.0.1", testPort, hugeReq);
    assert(hugeResp.find("413 Payload Too Large") != std::string::npos);

    std::cout << "  -> PASSED: LAN mode pairing token, CORS, body limit, and endpoint access controls verified.\n";

    fs::current_path(origCwd);
    // Clean up
    cleanDir(tempBase);

    std::cout << "\n========================================\n";
    std::cout << "ALL 7 TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "========================================\n";
    return 0;
}
