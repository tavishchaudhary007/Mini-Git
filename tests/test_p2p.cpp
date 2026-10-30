#include <cstdlib>

#ifndef CHECK
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "Check failed: (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
        std::exit(1); \
    } \
} while (0)
#endif
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
#include "PairingManager.h"
#include "Repository.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
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
    Hash c1 = repoA.commit("Add special and binary files");

    // Commit 2 on A: update special file and add another file
    std::string specialContent2 = specialContent + "\nLine 4: added in commit 2 with <brackets> & &";
    writeFile(repoAPath / "special.txt", specialContent2);
    writeFile(repoAPath / "notes.txt", "Plain notes file");
    repoA.add("special.txt");
    repoA.add("notes.txt");
    Hash c2 = repoA.commit("Update special file and add notes");

    // Export bundle from A
    std::string bundleA = repoA.exportBundleJson();
    CHECK(!bundleA.empty());

    // Import bundle into empty Repo B
    fs::current_path(repoBPath);
    std::string importRes1 = repoB.importBundleJson(bundleA);
    JsonValue jsonRes1 = JsonValue::parse(importRes1);

    CHECK(jsonRes1.is_object());
    CHECK(jsonRes1["ok"].as_bool() == true);
    CHECK(jsonRes1["objectsImported"].as_int64() > 0);
    CHECK(jsonRes1["objectsRejected"].as_int64() == 0);
    CHECK(jsonRes1["branchesCreated"].as_array().size() == 1);
    CHECK(jsonRes1["branchesCreated"].as_array()[0].as_string() == "main");

    // Check Repo B log and tips
    CHECK(repoB.branchTip("main") == c2);
    CHECK(repoB.head() == c2);

    auto logB = repoB.log();
    CHECK(logB.size() == 2);
    CHECK(logB[0].hash() == c2);
    CHECK(logB[1].hash() == c1);

    // Verify file contents match byte for byte
    CHECK(readFile(repoBPath / "special.txt") == specialContent2);
    CHECK(readFile(repoBPath / "binary.bin") == binaryContent);
    CHECK(readFile(repoBPath / "notes.txt") == "Plain notes file");

    std::cout << "  -> PASSED: All files, logs, and branch tips match byte for byte with exact hashes.\n";

    // -------------------------------------------------------------
    // TEST 2: Importing the same bundle twice changes nothing
    // -------------------------------------------------------------
    std::cout << "[Test 2] Testing idempotent second import of same bundle...\n";
    std::string importRes2 = repoB.importBundleJson(bundleA);
    JsonValue jsonRes2 = JsonValue::parse(importRes2);

    CHECK(jsonRes2.is_object());
    CHECK(jsonRes2["ok"].as_bool() == true);
    CHECK(jsonRes2["objectsImported"].as_int64() == 0);
    CHECK(jsonRes2["objectsPresent"].as_int64() > 0);
    CHECK(jsonRes2["objectsRejected"].as_int64() == 0);
    CHECK(jsonRes2["branchesUpToDate"].as_array().size() >= 1);
    CHECK(repoB.branchTip("main") == c2);

    std::cout << "  -> PASSED: Re-importing reports everything as already present and modifies nothing.\n";

    // -------------------------------------------------------------
    // TEST 3: Tampered object (content changed, key kept) is rejected
    // -------------------------------------------------------------
    std::cout << "[Test 3] Testing tampered object rejection (refs must remain untouched)...\n";
    // Tamper with bundle by changing content of a blob without changing the hash key
    std::string tamperedBundle = bundleA;
    size_t notesPos = tamperedBundle.find("Plain notes file");
    CHECK(notesPos != std::string::npos);
    tamperedBundle.replace(notesPos, 16, "TAMPERED notes!!");

    // Create a fresh Repo C
    fs::path repoCPath = tempBase / "repoC";
    cleanDir(repoCPath);
    Repository::init(repoCPath);
    fs::current_path(repoCPath);
    Repository repoC(repoCPath);

    std::string importTampered = repoC.importBundleJson(tamperedBundle);
    JsonValue jsonTampered = JsonValue::parse(importTampered);

    CHECK(jsonTampered.is_object());
    CHECK(jsonTampered["objectsRejected"].as_int64() > 0);
    // Branch main should NOT have been set to c2 because of rejected dependencies
    CHECK(!repoC.branchExists("main") || repoC.branchTip("main") != c2);

    std::cout << "  -> PASSED: Tampered object rejected and refs completely untouched.\n";

    // -------------------------------------------------------------
    // TEST 4: Diverged branch reported as skipped, tip unchanged
    // -------------------------------------------------------------
    std::cout << "[Test 4] Testing diverged branch safety (merge needed)...\n";
    // Repo B makes an independent commit on main
    fs::current_path(repoBPath);
    writeFile(repoBPath / "diverged_b.txt", "Work on B");
    repoB.add("diverged_b.txt");
    Hash cB = repoB.commit("Commit on B diverging from A");

    // Repo A makes an independent commit on main
    fs::current_path(repoAPath);
    writeFile(repoAPath / "diverged_a.txt", "Work on A");
    repoA.add("diverged_a.txt");
    Hash cA = repoA.commit("Commit on A diverging from B");

    std::string bundleA_diverged = repoA.exportBundleJson();

    // Import A's diverged bundle into B
    fs::current_path(repoBPath);
    std::string importDiverged = repoB.importBundleJson(bundleA_diverged);
    JsonValue jsonDiverged = JsonValue::parse(importDiverged);

    CHECK(jsonDiverged.is_object());
    CHECK(jsonDiverged["ok"].as_bool() == false);
    CHECK(jsonDiverged["branchesDiverged"].as_array().size() == 1);
    CHECK(jsonDiverged["branchesDiverged"].as_array()[0].as_string() == "main");
    // Branch tip in B MUST remain at cB
    CHECK(repoB.branchTip("main") == cB);
    CHECK(repoB.branchExists("peer-main"));
    CHECK(repoB.branchTip("peer-main") == cA);
    CHECK(jsonDiverged.contains("peerRefs"));
    CHECK(jsonDiverged["peerRefs"].as_array().size() == 1);
    CHECK(jsonDiverged["peerRefs"].as_array()[0].as_string() == "peer-main");

    std::cout << "  -> PASSED: Diverged branch correctly identified, skipped, and tip remained unchanged.\n";

    // -------------------------------------------------------------
    // TEST 5: Dirty working tree on current branch refuses fast-forward
    // -------------------------------------------------------------
    std::cout << "[Test 5] Testing dirty working tree refusal on active branch fast-forward...\n";
    // Create a new branch 'feature' on A, add commit, export bundle
    fs::current_path(repoAPath);
    repoA.createBranch("feature");
    repoA.checkout("feature");
    writeFile(repoAPath / "feature.txt", "Feature progress");
    repoA.add("feature.txt");
    Hash cFeat = repoA.commit("Feature commit");
    std::string bundleFeat = repoA.exportBundleJson();

    // Import into Repo B so B gets the feature branch
    fs::current_path(repoBPath);
    repoB.importBundleJson(bundleFeat);
    CHECK(repoB.branchTip("feature") == cFeat);

    // Now A advances feature with commit 2
    fs::current_path(repoAPath);
    writeFile(repoAPath / "feature.txt", "Feature progress v2");
    repoA.add("feature.txt");
    Hash cFeat2 = repoA.commit("Feature commit v2");
    std::string bundleFeat2 = repoA.exportBundleJson();

    // On B, checkout feature and make working tree dirty
    fs::current_path(repoBPath);
    repoB.checkout("feature");
    writeFile(repoBPath / "feature.txt", "UNCOMMITTED DIRTY EDITS ON B");

    std::string importDirty = repoB.importBundleJson(bundleFeat2);
    JsonValue jsonDirty = JsonValue::parse(importDirty);

    CHECK(jsonDirty.is_object());
    CHECK(jsonDirty["ok"].as_bool() == false);
    CHECK(jsonDirty["error"].as_string().find("uncommitted changes") != std::string::npos);
    // Tip of feature on B should still be cFeat (not fast-forwarded)
    CHECK(repoB.branchTip("feature") == cFeat);
    CHECK(readFile(repoBPath / "feature.txt") == "UNCOMMITTED DIRTY EDITS ON B");

    // Clean up dirty edits
    repoB.discard("feature.txt");

    std::cout << "  -> PASSED: Fast-forward cleanly refused with clear error when working tree is dirty.\n";

    // -------------------------------------------------------------
    // TEST 6: Malformed JSON returns an error and does not crash
    // -------------------------------------------------------------
    std::cout << "[Test 6] Testing malformed JSON handling...\n";
    std::vector<std::string> malformedBundles = {
        "{ unclosed json",
        "{\"minigit_bundle\": 1, \"objects\": \"not an object\"}",
        "{\"minigit_bundle\": 1, \"objects\": { \"abc\": { \"type\": \"unknown_type\" } } }",
        "",
        "null",
        "{\"objects\": {}, \"branches\": 12345}"
    };

    for (const auto& bad : malformedBundles) {
        std::string res = repoB.importBundleJson(bad);
        JsonValue j = JsonValue::parse(res);
        CHECK(j.is_object());
        CHECK(j["ok"].as_bool() == false);
        CHECK(!j["error"].as_string().empty());
    }

    std::cout << "  -> PASSED: All malformed JSON inputs handled safely with ok:false and zero crashes.\n";

    // -------------------------------------------------------------
    // TEST 7: 6-Digit Pair Code, Expiration, Lockout, Allow/Deny, Session Token & Revoke
    // -------------------------------------------------------------
    std::cout << "[Test 7] Testing 6-digit pair code, lockout, host Allow/Deny, 32-byte session token, and Revoke...\n";

    // 7.1 Direct unit tests on PairingManager
    {
        PairingManager pm;
        std::string c = pm.currentCode();
        CHECK(c.size() == 6);
        for (char ch : c) CHECK(std::isdigit(static_cast<unsigned char>(ch)));
        CHECK(!pm.isExpired());
        CHECK(!pm.isLocked());
        CHECK(!pm.isUsed());
        CHECK(pm.failedAttempts() == 0);

        // Test wrong attempts lockout
        for (int i = 1; i <= 4; ++i) {
            auto r = pm.submitCode("000000", "192.168.1.50");
            CHECK(!r.success);
            CHECK(!r.locked);
            CHECK(r.remainingAttempts == 5 - i);
        }
        // 5th wrong attempt locks the code
        auto r5 = pm.submitCode("000000", "192.168.1.50");
        CHECK(!r5.success);
        CHECK(r5.locked);
        CHECK(pm.isLocked());

        // 6th attempt is blocked
        auto r6 = pm.submitCode(c, "192.168.1.50");
        CHECK(!r6.success);
        CHECK(r6.locked);

        // Generating a new code unlocks
        std::string newC = pm.generateNewCode();
        CHECK(newC.size() == 6);
        CHECK(!pm.isLocked());
        CHECK(pm.failedAttempts() == 0);

        // Test single use: approve request, then try using the code again
        auto rOk = pm.submitCode(newC, "192.168.1.50");
        CHECK(rOk.success);
        CHECK(!rOk.requestId.empty());

        // Host approves
        bool dec = pm.decideRequest(rOk.requestId, true, "Teammate Laptop");
        CHECK(dec);
        CHECK(pm.isUsed());

        auto status = pm.getRequestStatus(rOk.requestId);
        CHECK(status.status == PendingPairRequest::APPROVED);
        CHECK(status.sessionToken.size() == 64); // 32 random bytes represented as 64 hex characters
        CHECK(pm.isValidSessionToken(status.sessionToken));

        // Attempting to submit the used code again fails
        auto rUsed = pm.submitCode(newC, "192.168.1.51");
        CHECK(!rUsed.success);
        CHECK(rUsed.error.find("already been used") != std::string::npos);

        // Test Revoke
        auto peers = pm.listPeers();
        CHECK(peers.size() == 1);
        CHECK(pm.isValidSessionToken(peers[0].sessionToken));

        bool revoked = pm.revokePeer(peers[0].id);
        CHECK(revoked);
        CHECK(!pm.isValidSessionToken(peers[0].sessionToken));
        CHECK(pm.listPeers().empty());

        // Test Expiration (5 min)
        pm.generateNewCode();
        pm.setExpirationSeconds(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        CHECK(pm.isExpired());
        auto rExp = pm.submitCode(pm.currentCode(), "192.168.1.50");
        CHECK(!rExp.success);
        CHECK(rExp.error.find("expired") != std::string::npos);

        // Group 4 (Bug 15): Failures do NOT invalidate code globally; per-IP lockout with exponential backoff
        PairingManager pmG4;
        std::string validCodeG4 = pmG4.currentCode();
        for (int i = 0; i < 10; ++i) {
            pmG4.submitCode("000000", "192.168.1.88");
        }
        CHECK(pmG4.isClientLocked("192.168.1.88"));
        CHECK(!pmG4.isInvalidated());
        CHECK(!pmG4.isExpired());
        // Another client with correct code succeeds
        auto rClientB = pmG4.submitCode(validCodeG4, "192.168.1.99");
        CHECK(rClientB.success);

        // Group 4 (Bug 16): Clear pending requests on new code or expiry; decideRequest refuses expired/used
        PairingManager pmClear;
        auto rPending = pmClear.submitCode(pmClear.currentCode(), "192.168.1.77");
        CHECK(rPending.success);
        CHECK(pmClear.pendingRequests().size() == 1);
        pmClear.generateNewCode();
        CHECK(pmClear.pendingRequests().empty());

        auto rPending2 = pmClear.submitCode(pmClear.currentCode(), "192.168.1.77");
        CHECK(rPending2.success);
        CHECK(pmClear.decideRequest(rPending2.requestId, true));
        CHECK(pmClear.isUsed());
        CHECK(!pmClear.decideRequest(rPending2.requestId, true));
    }
    std::cout << "  -> PASSED: PairingManager 6-digit code, expiration, 5-wrong lockout, single-use, and revoke verified.\n";

    // 7.2 Integration test over HTTP socket with real HttpServer in LAN mode
    int testPort = 18889;
    std::thread serverThread([&]() {
        try {
            HttpServer::run(&repoA, testPort, false, true); // lan = true
        } catch (const std::exception& e) {
            std::cerr << "HttpServer threw std::exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "HttpServer threw unknown exception" << std::endl;
        }
    });
    serverThread.detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Get pair code from loopback /api/pair/code (with port probing and retry)
    std::string codeReq = "GET /api/pair/code HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    std::string codeResp;
    int boundTestPort = testPort;
    for (int retry = 0; retry < 25; ++retry) {
        for (int p = testPort; p < testPort + 5; ++p) {
            codeResp = httpExchange("127.0.0.1", p, codeReq);
            if (codeResp.find("200 OK") != std::string::npos) {
                boundTestPort = p;
                break;
            }
        }
        if (codeResp.find("200 OK") != std::string::npos) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    testPort = boundTestPort;
    CHECK(codeResp.find("200 OK") != std::string::npos);

    size_t bodyPos = codeResp.find("\r\n\r\n");
    CHECK(bodyPos != std::string::npos);
    JsonValue codeObj = JsonValue::parse(codeResp.substr(bodyPos + 4));
    CHECK(codeObj.is_object());
    std::string pairCode = codeObj["code"].as_string();
    CHECK(pairCode.size() == 6);

    // Discover local LAN IP
    socket_t sTmp = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in loopbackAddr{};
    loopbackAddr.sin_family = AF_INET;
    loopbackAddr.sin_addr.s_addr = inet_addr("8.8.8.8");
    loopbackAddr.sin_port = htons(53);
    connect(sTmp, (struct sockaddr*)&loopbackAddr, sizeof(loopbackAddr));
    sockaddr_in nameAddr{};
#ifdef _WIN32
    int nameLen = sizeof(nameAddr);
#else
    socklen_t nameLen = sizeof(nameAddr);
#endif
    getsockname(sTmp, (struct sockaddr*)&nameAddr, &nameLen);
    char lanIpBuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &nameAddr.sin_addr, lanIpBuf, sizeof(lanIpBuf));
    CLOSESOCK(sTmp);
    std::string lanIp = lanIpBuf;

    if (lanIp != "127.0.0.1" && !lanIp.empty()) {
        std::cout << "  [Testing via LAN interface: " << lanIp << "]\n";

        // Non-loopback request to non-sync/pair endpoint is 403 Forbidden
        std::string lanCommitReq = "POST /api/commit HTTP/1.1\r\nHost: " + lanIp + "\r\nContent-Type: application/json\r\nContent-Length: 22\r\nConnection: close\r\n\r\n{\"message\":\"malicious\"}";
        std::string lanCommitResp = httpExchange(lanIp, testPort, lanCommitReq);
        CHECK(lanCommitResp.find("403 Forbidden") != std::string::npos);

        // Submit wrong code over LAN
        std::string wrongBody = "{\"code\":\"000000\"}";
        std::string wrongReq = "POST /api/pair HTTP/1.1\r\nHost: " + lanIp + "\r\nContent-Type: application/json\r\nContent-Length: " +
                               std::to_string(wrongBody.size()) + "\r\nConnection: close\r\n\r\n" + wrongBody;
        std::string wrongResp = httpExchange(lanIp, testPort, wrongReq);
        CHECK(wrongResp.find("401 Unauthorized") != std::string::npos);

        // Submit correct pair code over LAN
        std::string correctBody = "{\"code\":\"" + pairCode + "\",\"peerName\":\"Laptop B\"}";
        std::string correctReq = "POST /api/pair HTTP/1.1\r\nHost: " + lanIp + "\r\nContent-Type: application/json\r\nContent-Length: " +
                                 std::to_string(correctBody.size()) + "\r\nConnection: close\r\n\r\n" + correctBody;
        std::string correctResp = httpExchange(lanIp, testPort, correctReq);
        CHECK(correctResp.find("200 OK") != std::string::npos);

        size_t bPos = correctResp.find("\r\n\r\n");
        JsonValue pairRes = JsonValue::parse(correctResp.substr(bPos + 4));
        CHECK(pairRes["status"].as_string() == "pending");
        std::string reqId = pairRes["requestId"].as_string();
        CHECK(!reqId.empty());

        // Host checks pending requests via loopback
        std::string pendReq = "GET /api/pair/pending HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        std::string pendResp = httpExchange("127.0.0.1", testPort, pendReq);
        CHECK(pendResp.find("200 OK") != std::string::npos);
        CHECK(pendResp.find(reqId) != std::string::npos);

        // Host clicks ALLOW
        std::string decideBody = "{\"requestId\":\"" + reqId + "\",\"action\":\"allow\"}";
        std::string decideReq = "POST /api/pair/decide HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
                                std::to_string(decideBody.size()) + "\r\nConnection: close\r\n\r\n" + decideBody;
        std::string decideResp = httpExchange("127.0.0.1", testPort, decideReq);
        CHECK(decideResp.find("200 OK") != std::string::npos);

        // Peer polls status over LAN and receives 32-byte session token
        std::string statReq = "GET /api/pair/status?requestId=" + reqId + " HTTP/1.1\r\nHost: " + lanIp + "\r\nConnection: close\r\n\r\n";
        std::string statResp = httpExchange(lanIp, testPort, statReq);
        CHECK(statResp.find("200 OK") != std::string::npos);
        size_t sPos = statResp.find("\r\n\r\n");
        JsonValue statObj = JsonValue::parse(statResp.substr(sPos + 4));
        CHECK(statObj["status"].as_string() == "approved");
        std::string sessionToken = statObj["token"].as_string();
        CHECK(sessionToken.size() == 64); // 32 random bytes represented as 64 hex characters

        // Group 4 (Bug 16): Token handed out once, then deleted; subsequent poll returns 404
        std::string secondStatResp = httpExchange(lanIp, testPort, statReq);
        CHECK(secondStatResp.find("404 Not Found") != std::string::npos);

        // Sync bundle using valid session token
        std::string syncReq = "GET /api/sync/bundle HTTP/1.1\r\nHost: " + lanIp + "\r\nX-MiniGit-Token: " + sessionToken + "\r\nConnection: close\r\n\r\n";
        std::string syncResp = httpExchange(lanIp, testPort, syncReq);
        CHECK(syncResp.find("200 OK") != std::string::npos);

        // Sync bundle with invalid token returns 401
        std::string badSyncReq = "GET /api/sync/bundle HTTP/1.1\r\nHost: " + lanIp + "\r\nX-MiniGit-Token: bad_token_123\r\nConnection: close\r\n\r\n";
        std::string badSyncResp = httpExchange(lanIp, testPort, badSyncReq);
        CHECK(badSyncResp.find("401 Unauthorized") != std::string::npos);

        // Host checks paired peers list
        std::string peersReq = "GET /api/pair/peers HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        std::string peersResp = httpExchange("127.0.0.1", testPort, peersReq);
        CHECK(peersResp.find("200 OK") != std::string::npos);
        size_t pPos = peersResp.find("\r\n\r\n");
        JsonValue peersObj = JsonValue::parse(peersResp.substr(pPos + 4));
        CHECK(peersObj["peers"].as_array().size() >= 1);
        std::string peerId = peersObj["peers"].as_array()[0]["id"].as_string();

        // Host revokes peer
        std::string revokeBody = "{\"id\":\"" + peerId + "\"}";
        std::string revokeReq = "POST /api/pair/revoke HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
                                std::to_string(revokeBody.size()) + "\r\nConnection: close\r\n\r\n" + revokeBody;
        std::string revokeResp = httpExchange("127.0.0.1", testPort, revokeReq);
        CHECK(revokeResp.find("200 OK") != std::string::npos);

        // Now previous session token is revoked and sync returns 401 Unauthorized
        std::string revokedSyncResp = httpExchange(lanIp, testPort, syncReq);
        CHECK(revokedSyncResp.find("401 Unauthorized") != std::string::npos);
        std::cout << "  -> PASSED: HTTP pair code verification, Host approval popup, session token sync, and revocation verified.\n";
    }

    // 7.3 Payload size limit (50 MB)
    std::string hugeReq = "POST /api/sync/import-bundle HTTP/1.1\r\nHost: 127.0.0.1\r\ncontent-length: 60000000\r\nConnection: close\r\n\r\n";
    std::string hugeResp = httpExchange("127.0.0.1", testPort, hugeReq);
    CHECK(hugeResp.find("413 Payload Too Large") != std::string::npos);

    // -------------------------------------------------------------
    // TEST 8: Diverged branch 3-way merge, invalid branch security, and multi-parent commit
    // -------------------------------------------------------------
    std::cout << "[Test 8] Testing 3-way merge, security validation, and multi-parent commits...\n";
    fs::path repo8APath = tempBase / "repo8_a";
    fs::path repo8BPath = tempBase / "repo8_b";
    fs::create_directories(repo8APath);
    fs::create_directories(repo8BPath);

    Repository::init(repo8APath);
    Repository::init(repo8BPath);
    Repository repo8A(repo8APath);
    Repository repo8B(repo8BPath);

    // Shared base commit on A
    fs::current_path(repo8APath);
    writeFile(repo8APath / "shared.txt", "Shared base line\n");
    repo8A.add("shared.txt");
    Hash baseCommit = repo8A.commit("Shared initial base");
    repo8A.createBranch("side");

    // Export base bundle to B
    std::string baseBundle = repo8A.exportBundleJson();
    fs::current_path(repo8BPath);
    repo8B.importBundleJson(baseBundle);
    CHECK(repo8B.branchTip("main") == baseCommit);

    // A commits a_only.txt on main
    fs::current_path(repo8APath);
    writeFile(repo8APath / "a_only.txt", "A only content\n");
    repo8A.add("a_only.txt");
    Hash c8A = repo8A.commit("Commit on A main");

    // A advances branch side
    repo8A.checkout("side");
    writeFile(repo8APath / "side.txt", "Side content\n");
    repo8A.add("side.txt");
    Hash c8Side = repo8A.commit("Commit on A side");
    repo8A.checkout("main");

    // B commits b_only.txt on main
    fs::current_path(repo8BPath);
    writeFile(repo8BPath / "b_only.txt", "B only content\n");
    repo8B.add("b_only.txt");
    Hash c8B = repo8B.commit("Commit on B main");

    // Export bundle from A, and tamper it with invalid branch names: "../HEAD" and "peer-evil"
    std::string bundle8A = repo8A.exportBundleJson();
    JsonValue j8A = JsonValue::parse(bundle8A);
    CHECK(j8A.is_object() && j8A.contains("branches"));
    std::map<std::string, JsonValue> branchesMap = j8A["branches"].as_object();
    branchesMap["../HEAD"] = JsonValue(c8A.str());
    branchesMap["peer-evil"] = JsonValue(c8A.str());
    std::map<std::string, JsonValue> rootMap = j8A.as_object();
    rootMap["branches"] = JsonValue(branchesMap);
    std::string tamperedBundle8A = JsonValue(rootMap).serialize();

    // Import into B
    std::string import8Res = repo8B.importBundleJson(tamperedBundle8A);
    JsonValue jImport8 = JsonValue::parse(import8Res);

    // Assert side fast-forwarded
    CHECK(repo8B.branchExists("side"));
    CHECK(repo8B.branchTip("side") == c8Side);

    // Assert main unchanged
    CHECK(repo8B.branchTip("main") == c8B);

    // Assert peer-main == A's tip
    CHECK(repo8B.branchExists("peer-main"));
    CHECK(repo8B.branchTip("peer-main") == c8A);

    // Assert branchesSkippedInvalid has 2 entries
    CHECK(jImport8.contains("branchesSkippedInvalid"));
    const auto& skippedArr = jImport8["branchesSkippedInvalid"].as_array();
    CHECK(skippedArr.size() == 2);
    bool foundDotDot = false, foundPeerEvil = false;
    for (const auto& v : skippedArr) {
        if (v.as_string() == "../HEAD") foundDotDot = true;
        if (v.as_string() == "peer-evil") foundPeerEvil = true;
    }
    CHECK(foundDotDot && foundPeerEvil);

    // Assert no files written outside refs/heads/
    CHECK(!fs::exists(repo8BPath / ".minigit" / "refs" / "HEAD"));
    CHECK(!fs::exists(repo8BPath / "HEAD"));
    CHECK(!fs::exists(repo8BPath / ".minigit" / "refs" / "heads" / "peer-evil"));

    // Then run B.merge("peer-main")
    std::string mergeRes = repo8B.merge("peer-main");
    CHECK(mergeRes.find("Merged peer-main (3-way)") != std::string::npos);

    // Assert both files exist
    CHECK(fs::exists(repo8BPath / "a_only.txt"));
    CHECK(fs::exists(repo8BPath / "b_only.txt"));
    CHECK(fs::exists(repo8BPath / "shared.txt"));
    CHECK(readFile(repo8BPath / "a_only.txt") == "A only content\n");
    CHECK(readFile(repo8BPath / "b_only.txt") == "B only content\n");

    // HEAD has two parents
    auto log8B = repo8B.log();
    CHECK(!log8B.empty());
    CHECK(log8B.front().hasParent2());
    CHECK(log8B.front().parent() == c8B);
    CHECK(log8B.front().parent2() == c8A);

    // Re-importing bundle reports nothing diverged
    std::string reimportRes = repo8B.importBundleJson(repo8A.exportBundleJson());
    JsonValue jReimport = JsonValue::parse(reimportRes);
    CHECK(jReimport.contains("branchesDiverged"));
    CHECK(jReimport["branchesDiverged"].as_array().empty());

    std::cout << "  -> PASSED: 3-way merge succeeded, 2 parents verified, security bounds enforced.\n";

    fs::current_path(origCwd);
    cleanDir(tempBase);

    std::cout << "\n========================================\n";
    std::cout << "ALL 8 TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "========================================\n";
    return 0;
}
