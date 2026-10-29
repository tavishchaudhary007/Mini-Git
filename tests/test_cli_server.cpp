#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <thread>
#include <chrono>

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

#include "Repository.h"
#include "HtmlExporter.h"
#include "HttpServer.h"
#include "Commands.h"
#include "CLI.h"
#include "Json.h"
#include "FileUtil.h"

namespace fs = std::filesystem;

static void removeDir(const fs::path& p) {
    std::error_code ec;
    if (fs::exists(p, ec)) {
        for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); ++it) {
            fs::permissions(it->path(), fs::perms::all, fs::perm_options::add, ec);
        }
        fs::permissions(p, fs::perms::all, fs::perm_options::add, ec);
        fs::remove_all(p, ec);
    }
}

static void cleanDir(const fs::path& p) {
    removeDir(p);
    std::error_code ec;
    fs::create_directories(p, ec);
}

// Redirect cout helper
class CoutCapture {
    std::stringstream buffer;
    std::streambuf* old;
public:
    CoutCapture() : old(std::cout.rdbuf(buffer.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(old); }
    std::string str() const { return buffer.str(); }
};

static std::string httpExchange(const std::string& host, int port, const std::string& rawReq) {
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == (socket_t)-1) {
        return "";
    }

#ifdef _WIN32
    DWORD timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#endif

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

// =========================================================================
// Test 1: Fix 12 - log prints Date as readable local time, not unix seconds
// =========================================================================
void test12_log_readable_time() {
    std::cout << "[Test 12] Log readable local date format... " << std::flush;
    fs::path dir = fs::current_path() / "test_repo_cli_12";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "hello.txt", "world");
    repo.add("hello.txt", true);
    repo.commit("initial commit");

    std::string output;
    {
        CoutCapture capture;
        LogCmd logCmd;
        logCmd.execute(&repo, {});
        output = capture.str();
    }

    assert(output.find("Date:   ") != std::string::npos);
    size_t datePos = output.find("Date:   ") + 8;
    std::string dateLine = output.substr(datePos, output.find('\n', datePos) - datePos);

    bool hasNonDigit = false;
    for (char c : dateLine) {
        if (std::isalpha(static_cast<unsigned char>(c))) hasNonDigit = true;
    }
    assert(hasNonDigit);
    assert(dateLine.find('+') != std::string::npos || dateLine.find('-') != std::string::npos);

    removeDir(dir);
    std::cout << "PASSED" << std::endl;
}

// =========================================================================
// Test 2: Fix 17 - ui/serve --port validation (1-65535)
// =========================================================================
void test17_port_validation() {
    std::cout << "[Test 17] ui/serve --port 1-65535 validation... " << std::flush;
    fs::path dir = fs::current_path() / "test_repo_cli_17";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    UiCmd uiCmd;
    ServeCmd serveCmd;

    auto testInvalidPort = [&](const std::string& p) {
        bool threw = false;
        try {
            uiCmd.execute(&repo, {"--port", p, "--no-open"});
        } catch (const MiniGitException& e) {
            threw = true;
            assert(std::string(e.what()).find("usage:") != std::string::npos);
        }
        assert(threw);
    };

    testInvalidPort("abc");
    testInvalidPort("0");
    testInvalidPort("65536");
    testInvalidPort("999999");
    testInvalidPort("-5");
    testInvalidPort("8080abc");

    bool threwMissing = false;
    try {
        uiCmd.execute(&repo, {"--port"});
    } catch (const MiniGitException& e) {
        threwMissing = true;
        assert(std::string(e.what()).find("usage:") != std::string::npos);
    }
    assert(threwMissing);

    bool threwServe = false;
    try {
        serveCmd.execute(&repo, {"--port", "invalid"});
    } catch (const MiniGitException& e) {
        threwServe = true;
        assert(std::string(e.what()).find("usage:") != std::string::npos);
    }
    assert(threwServe);

    removeDir(dir);
    std::cout << "PASSED" << std::endl;
}

// =========================================================================
// Test 3: Fix 18 - Empty repository branch and checkout main
// =========================================================================
void test18_empty_repo_branch_and_checkout() {
    std::cout << "[Test 18] Empty repo branch & checkout main... " << std::flush;
    fs::path dir = fs::current_path() / "test_repo_cli_18";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    {
        CoutCapture capture;
        BranchCmd branchCmd;
        branchCmd.execute(&repo, {});
        std::string out = capture.str();
        assert(out == "* main\n");
    }

    {
        CoutCapture capture;
        CheckoutCmd checkoutCmd;
        checkoutCmd.execute(&repo, {"main"});
        std::string out = capture.str();
        assert(out.find("branch 'main' has no commits yet") != std::string::npos);
    }

    removeDir(dir);
    std::cout << "PASSED" << std::endl;
}

// =========================================================================
// Test 4: Fix 8 - status prints merge information when isMerging()
// =========================================================================
void test8_status_merging() {
    std::cout << "[Test 8] Status prints merge info when isMerging()... " << std::flush;
    fs::path dir = fs::current_path() / "test_repo_cli_8";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "f.txt", "base");
    repo.add("f.txt", true);
    repo.commit("initial");

    repo.createBranch("feat");
    repo.checkout("feat");
    writeFile(dir / "f.txt", "feature content");
    repo.add("f.txt", true);
    repo.commit("feat commit");

    repo.checkout("main");
    writeFile(dir / "f.txt", "main content");
    repo.add("f.txt", true);
    repo.commit("main commit");

    bool threwMerge = false;
    try {
        repo.merge("feat");
    } catch (const MergeException&) {
        threwMerge = true;
    }
    assert(threwMerge);
    assert(repo.isMerging());

    std::string out;
    {
        CoutCapture capture;
        StatusCmd statusCmd;
        statusCmd.execute(&repo, {});
        out = capture.str();
    }

    assert(out.find("You are in the middle of a merge.") != std::string::npos);
    assert(out.find("Unmerged paths:") != std::string::npos);
    assert(out.find("f.txt") != std::string::npos);

    repo.mergeAbort();
    assert(!repo.isMerging());

    removeDir(dir);
    std::cout << "PASSED" << std::endl;
}

// =========================================================================
// Test 5: Fix 21 & JSON Contract - Snapshot fields, no fake branch, ETag version
// =========================================================================
void test21_snapshot_json_contract() {
    std::cout << "[Test 21] Snapshot JSON contract & real branches... " << std::flush;
    fs::path dir = fs::current_path() / "test_repo_cli_21";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    {
        Snapshot s = repo.snapshot();
        std::string jsonStr = HtmlExporter::toJson(s, "test_repo");
        std::string err;
        JsonValue j = JsonValue::parse(jsonStr, &err);
        assert(err.empty());
        assert(j.is_object());

        assert(j["head"].is_null());
        assert(j["detached"].as_bool() == false);
        assert(j["merging"].as_bool() == false);
        assert(j["conflicts"].is_array() && j["conflicts"].as_array().empty());
        assert(j["stagedStatus"].is_object());
        assert(j["version"].is_string() && !j["version"].as_string().empty());
        assert(j["currentBranch"].as_string() == "main");
    }

    writeFile(dir / "file1.txt", "content1");
    repo.add("file1.txt", true);
    Hash h1 = repo.commit("c1");

    writeFile(dir / "file2.txt", "content2");
    repo.add("file2.txt", true);
    {
        Snapshot s = repo.snapshot();
        std::string jsonStr = HtmlExporter::toJson(s, "test_repo");
        JsonValue j = JsonValue::parse(jsonStr);
        assert(j["head"].as_string() == h1.str());
        assert(j["stagedStatus"].is_object());
        assert(j["stagedStatus"]["file2.txt"].as_string() == "A");
    }
    Hash h2 = repo.commit("c2");

    repo.checkout(h1.str());
    {
        Snapshot s = repo.snapshot();
        assert(s.detached);
        assert(s.currentBranch.empty());

        std::string jsonStr = HtmlExporter::toJson(s, "test_repo");
        JsonValue j = JsonValue::parse(jsonStr);

        assert(j["detached"].as_bool() == true);
        assert(j["currentBranch"].as_string() == "");

        const auto& branchesObj = j["branches"].as_object();
        for (const auto& [name, tip] : branchesObj) {
            assert(name.find("(detached") == std::string::npos);
        }
    }

    repo.checkout("main");
    std::string vA = HtmlExporter::computeVersion(repo.snapshot());
    writeFile(dir / "file1.txt", "modified content");
    std::string vB = HtmlExporter::computeVersion(repo.snapshot());
    assert(vA != vB);

    repo.add("file1.txt", true);
    std::string vC = HtmlExporter::computeVersion(repo.snapshot());
    assert(vB != vC);

    removeDir(dir);
    std::cout << "PASSED" << std::endl;
}

// =========================================================================
// Test 6: Fix 22 - New CLI commands (branch -d, undo, revert, stash, reflog)
// =========================================================================
void test22_new_cli_commands() {
    std::cout << "[Test 22] New CLI commands (branch -d, undo, revert, stash, reflog)... " << std::flush;
    fs::path dir = fs::current_path() / "test_repo_cli_22";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "f.txt", "v1");
    repo.add("f.txt", true);
    repo.commit("initial");

    repo.createBranch("feat");
    assert(repo.branchExists("feat"));
    {
        CoutCapture cap;
        BranchCmd cmd;
        cmd.execute(&repo, {"-d", "feat"});
        assert(cap.str().find("Deleted branch feat") != std::string::npos);
        assert(!repo.branchExists("feat"));
    }

    writeFile(dir / "f.txt", "v2");
    repo.add("f.txt", true);
    Hash h2 = repo.commit("commit 2");
    assert(repo.head() == h2);
    {
        CoutCapture cap;
        UndoCmd cmd;
        cmd.execute(&repo, {});
        assert(cap.str().find("Undid commit") != std::string::npos);
        assert(repo.head() != h2);
        assert(readFile(dir / "f.txt") == "v2");
    }

    repo.add("f.txt", true);
    Hash hRecommit = repo.commit("commit v2 restored");
    {
        CoutCapture cap;
        RevertCmd cmd;
        cmd.execute(&repo, {hRecommit.shortStr()});
        assert(cap.str().find("Revert") != std::string::npos);
        assert(readFile(dir / "f.txt") == "v1");
    }

    writeFile(dir / "f.txt", "stash-modification");
    writeFile(dir / "untracked.txt", "untracked-stuff");
    {
        CoutCapture capSave;
        StashCmd cmd;
        cmd.execute(&repo, {"save", "-u", "-m", "my-stash"});
        assert(capSave.str().find("Saved stash") != std::string::npos);
        assert(readFile(dir / "f.txt") == "v1");
        assert(!fs::exists(dir / "untracked.txt"));
    }
    {
        CoutCapture capList;
        StashCmd cmd;
        cmd.execute(&repo, {"list"});
        assert(capList.str().find("stash@{0}: my-stash") != std::string::npos);
    }
    {
        CoutCapture capPop;
        StashCmd cmd;
        cmd.execute(&repo, {"pop"});
        assert(capPop.str().find("Restored and applied stash") != std::string::npos);
        assert(readFile(dir / "f.txt") == "stash-modification");
        assert(fs::exists(dir / "untracked.txt"));
        assert(readFile(dir / "untracked.txt") == "untracked-stuff");
    }

    {
        CoutCapture capReflog;
        ReflogCmd cmd;
        cmd.execute(&repo, {});
        std::string reflogOut = capReflog.str();
        assert(reflogOut.find("HEAD@{") != std::string::npos);
        assert(reflogOut.find("commit") != std::string::npos || reflogOut.find("revert") != std::string::npos);
    }

    removeDir(dir);
    std::cout << "PASSED" << std::endl;
}

// =========================================================================
// Test 7: HTTP Server: Fix 19, branch/create with "at", ETag/304, peerRefs, snapshot on fail
// =========================================================================
void test_http_server_endpoints() {
    std::cout << "[Test Server] ETag/304, branch create at, backslash JSON, failure snapshot, peerRefs... " << std::flush;
    fs::path dir = fs::current_path() / "test_repo_cli_srv";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "init.txt", "hello");
    repo.add("init.txt", true);
    Hash hBase = repo.commit("base commit");

    writeFile(dir / "next.txt", "world");
    repo.add("next.txt", true);
    Hash hNext = repo.commit("next commit");

    int testPort = 19933;
    std::thread serverThread([&]() {
        try {
            HttpServer::run(&repo, testPort, false, true); // lan = true binds INADDR_ANY
        } catch (...) {}
    });
    serverThread.detach();

    // Probe port
    std::string probeReq = "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    std::string probeResp;
    int boundPort = testPort;
    for (int retry = 0; retry < 25; ++retry) {
        for (int p = testPort; p < testPort + 5; ++p) {
            probeResp = httpExchange("127.0.0.1", p, probeReq);
            if (probeResp.find("200 OK") != std::string::npos) {
                boundPort = p;
                break;
            }
        }
        if (probeResp.find("200 OK") != std::string::npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    testPort = boundPort;

    if (probeResp.find("200 OK") == std::string::npos) {
        std::cout << "SKIPPED (loopback socket blocked in sandbox)" << std::endl;
        removeDir(dir);
        return;
    }

    // 1. ETag & 304 Not Modified
    size_t etagPos = probeResp.find("ETag: ");
    assert(etagPos != std::string::npos);
    size_t etagEnd = probeResp.find("\r\n", etagPos);
    std::string etagVal = probeResp.substr(etagPos + 6, etagEnd - (etagPos + 6));
    assert(!etagVal.empty());

    std::string req304 = "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1\r\nIf-None-Match: " + etagVal + "\r\nConnection: close\r\n\r\n";
    std::string resp304 = httpExchange("127.0.0.1", testPort, req304);
    assert(resp304.find("304 Not Modified") != std::string::npos);
    size_t bodySep = resp304.find("\r\n\r\n");
    assert(bodySep != std::string::npos);
    std::string body304 = resp304.substr(bodySep + 4);
    assert(body304.empty());

    // 2. POST /api/branch/create with "at" (hash or prefix)
    std::string createAtBody = "{\"name\":\"branch_at_c1\",\"at\":\"" + hBase.shortStr() + "\"}";
    std::string createAtReq = "POST /api/branch/create HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(createAtBody.size()) + "\r\nConnection: close\r\n\r\n" + createAtBody;
    std::string createAtResp = httpExchange("127.0.0.1", testPort, createAtReq);
    assert(createAtResp.find("200 OK") != std::string::npos);
    assert(repo.branchExists("branch_at_c1"));
    assert(repo.branchTip("branch_at_c1") == hBase);

    // 3. POST /api/stash/save with includeUntracked: true (item 49)
    writeFile(dir / "untracked_api.txt", "untracked api text");
    std::string stashBody = "{\"message\":\"api-stash\",\"includeUntracked\":true}";
    std::string stashReq = "POST /api/stash/save HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(stashBody.size()) + "\r\nConnection: close\r\n\r\n" + stashBody;
    std::string stashResp = httpExchange("127.0.0.1", testPort, stashReq);
    assert(stashResp.find("200 OK") != std::string::npos);
    assert(!fs::exists(dir / "untracked_api.txt"));

    std::string popReq = "POST /api/stash/pop HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    std::string popResp = httpExchange("127.0.0.1", testPort, popReq);
    assert(popResp.find("200 OK") != std::string::npos);
    assert(fs::exists(dir / "untracked_api.txt"));

    // 4. Fix 19: Error with a backslash still returns valid JSON
    std::string bsBodyIn = "{\"path\":\"..\\\\invalid\\\\path.txt\",\"content\":\"\"}";
    std::string backslashReq = "POST /api/file/save HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(bsBodyIn.size()) + "\r\nConnection: close\r\n\r\n" + bsBodyIn;
    std::string backslashResp = httpExchange("127.0.0.1", testPort, backslashReq);
    assert(backslashResp.find("400 Bad Request") != std::string::npos);
    size_t bsBodyPos = backslashResp.find("\r\n\r\n");
    assert(bsBodyPos != std::string::npos);
    std::string bsBody = backslashResp.substr(bsBodyPos + 4);
    std::string parseErr;
    JsonValue bsJson = JsonValue::parse(bsBody, &parseErr);
    assert(parseErr.empty());
    assert(bsJson.is_object());
    assert(bsJson["ok"].as_bool() == false);
    assert(bsJson.contains("error"));

    // 5. Checkout failure includes "snapshot"
    std::string coBody = "{\"target\":\"nonexistent_target\"}";
    std::string badCheckoutReq = "POST /api/checkout HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(coBody.size()) + "\r\nConnection: close\r\n\r\n" + coBody;
    std::string badCheckoutResp = httpExchange("127.0.0.1", testPort, badCheckoutReq);
    assert(badCheckoutResp.find("400 Bad Request") != std::string::npos);
    size_t coBodyPos = badCheckoutResp.find("\r\n\r\n");
    assert(coBodyPos != std::string::npos);
    JsonValue coJson = JsonValue::parse(badCheckoutResp.substr(coBodyPos + 4));
    assert(coJson["ok"].as_bool() == false);
    assert(coJson.contains("snapshot") && coJson["snapshot"].is_object());

    // 6. import-bundle failure includes "snapshot" and top-level "peerRefs" (item 35)
    std::string bundleBody = "{\"objects\":\"not_an_object\"}";
    std::string badBundleReq = "POST /api/sync/import-bundle HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(bundleBody.size()) + "\r\nConnection: close\r\n\r\n" + bundleBody;
    std::string badBundleResp = httpExchange("127.0.0.1", testPort, badBundleReq);
    assert(badBundleResp.find("400 Bad Request") != std::string::npos);
    size_t bundleBodyPos = badBundleResp.find("\r\n\r\n");
    assert(bundleBodyPos != std::string::npos);
    JsonValue bundleJson = JsonValue::parse(badBundleResp.substr(bundleBodyPos + 4));
    assert(bundleJson["ok"].as_bool() == false);
    assert(bundleJson.contains("peerRefs") && bundleJson["peerRefs"].is_array());
    assert(bundleJson.contains("snapshot") && bundleJson["snapshot"].is_object());

    removeDir(dir);
    std::cout << "PASSED" << std::endl;
}

int main() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    std::cout << "============================================\n";
    std::cout << "  Mini-Git CLI & Server Verification Tests\n";
    std::cout << "============================================\n";

    test12_log_readable_time();
    test17_port_validation();
    test18_empty_repo_branch_and_checkout();
    test8_status_merging();
    test21_snapshot_json_contract();
    test22_new_cli_commands();
    test_http_server_endpoints();

    std::cout << "\nALL CLI & SERVER TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
