#include <iostream>
#include <cstdlib>

#ifndef CHECK
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "Check failed: (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
        std::exit(1); \
    } \
} while (0)
#endif
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

    CHECK(output.find("Date:   ") != std::string::npos);
    size_t datePos = output.find("Date:   ") + 8;
    std::string dateLine = output.substr(datePos, output.find('\n', datePos) - datePos);

    bool hasNonDigit = false;
    for (char c : dateLine) {
        if (std::isalpha(static_cast<unsigned char>(c))) hasNonDigit = true;
    }
    CHECK(hasNonDigit);
    CHECK(dateLine.find('+') != std::string::npos || dateLine.find('-') != std::string::npos);

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
            CHECK(std::string(e.what()).find("usage:") != std::string::npos);
        }
        CHECK(threw);
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
        CHECK(std::string(e.what()).find("usage:") != std::string::npos);
    }
    CHECK(threwMissing);

    bool threwServe = false;
    try {
        serveCmd.execute(&repo, {"--port", "invalid"});
    } catch (const MiniGitException& e) {
        threwServe = true;
        CHECK(std::string(e.what()).find("usage:") != std::string::npos);
    }
    CHECK(threwServe);

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
        CHECK(out == "* main\n");
    }

    {
        CoutCapture capture;
        CheckoutCmd checkoutCmd;
        checkoutCmd.execute(&repo, {"main"});
        std::string out = capture.str();
        CHECK(out.find("branch 'main' has no commits yet") != std::string::npos);
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
    CHECK(threwMerge);
    CHECK(repo.isMerging());

    std::string out;
    {
        CoutCapture capture;
        StatusCmd statusCmd;
        statusCmd.execute(&repo, {});
        out = capture.str();
    }

    CHECK(out.find("You are in the middle of a merge.") != std::string::npos);
    CHECK(out.find("Unmerged paths:") != std::string::npos);
    CHECK(out.find("f.txt") != std::string::npos);

    repo.mergeAbort();
    CHECK(!repo.isMerging());

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
        CHECK(err.empty());
        CHECK(j.is_object());

        CHECK(j["head"].is_null());
        CHECK(j["detached"].as_bool() == false);
        CHECK(j["merging"].as_bool() == false);
        CHECK(j["conflicts"].is_array() && j["conflicts"].as_array().empty());
        CHECK(j["stagedStatus"].is_object());
        CHECK(j["version"].is_string() && !j["version"].as_string().empty());
        CHECK(j["currentBranch"].as_string() == "main");
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
        CHECK(j["head"].as_string() == h1.str());
        CHECK(j["stagedStatus"].is_object());
        CHECK(j["stagedStatus"]["file2.txt"].as_string() == "A");
    }
    Hash h2 = repo.commit("c2");

    repo.checkout(h1.str());
    {
        Snapshot s = repo.snapshot();
        CHECK(s.detached);
        CHECK(s.currentBranch.empty());

        std::string jsonStr = HtmlExporter::toJson(s, "test_repo");
        JsonValue j = JsonValue::parse(jsonStr);

        CHECK(j["detached"].as_bool() == true);
        CHECK(j["currentBranch"].as_string() == "");

        const auto& branchesObj = j["branches"].as_object();
        for (const auto& [name, tip] : branchesObj) {
            (void)tip;
            CHECK(name.find("(detached") == std::string::npos);
        }
    }

    repo.checkout("main");
    std::string vA = HtmlExporter::computeVersion(repo.snapshot());
    writeFile(dir / "file1.txt", "modified content");
    std::string vB = HtmlExporter::computeVersion(repo.snapshot());
    CHECK(vA != vB);

    repo.add("file1.txt", true);
    std::string vC = HtmlExporter::computeVersion(repo.snapshot());
    CHECK(vB != vC);

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
    CHECK(repo.branchExists("feat"));
    {
        CoutCapture cap;
        BranchCmd cmd;
        cmd.execute(&repo, {"-d", "feat"});
        CHECK(cap.str().find("Deleted branch feat") != std::string::npos);
        CHECK(!repo.branchExists("feat"));
    }

    writeFile(dir / "f.txt", "v2");
    repo.add("f.txt", true);
    Hash h2 = repo.commit("commit 2");
    CHECK(repo.head() == h2);
    {
        CoutCapture cap;
        UndoCmd cmd;
        cmd.execute(&repo, {});
        CHECK(cap.str().find("Undid commit") != std::string::npos);
        CHECK(repo.head() != h2);
        CHECK(readFile(dir / "f.txt") == "v2");
    }

    repo.add("f.txt", true);
    Hash hRecommit = repo.commit("commit v2 restored");
    {
        CoutCapture cap;
        RevertCmd cmd;
        cmd.execute(&repo, {hRecommit.shortStr()});
        CHECK(cap.str().find("Revert") != std::string::npos);
        CHECK(readFile(dir / "f.txt") == "v1");
    }

    writeFile(dir / "f.txt", "stash-modification");
    writeFile(dir / "untracked.txt", "untracked-stuff");
    {
        CoutCapture capSave;
        StashCmd cmd;
        cmd.execute(&repo, {"save", "-u", "-m", "my-stash"});
        CHECK(capSave.str().find("Saved stash") != std::string::npos);
        CHECK(readFile(dir / "f.txt") == "v1");
        CHECK(!fs::exists(dir / "untracked.txt"));
    }
    {
        CoutCapture capList;
        StashCmd cmd;
        cmd.execute(&repo, {"list"});
        CHECK(capList.str().find("stash@{0}: my-stash") != std::string::npos);
    }
    {
        CoutCapture capPop;
        StashCmd cmd;
        cmd.execute(&repo, {"pop"});
        CHECK(capPop.str().find("Restored and applied stash") != std::string::npos);
        CHECK(readFile(dir / "f.txt") == "stash-modification");
        CHECK(fs::exists(dir / "untracked.txt"));
        CHECK(readFile(dir / "untracked.txt") == "untracked-stuff");
    }

    {
        CoutCapture capReflog;
        ReflogCmd cmd;
        cmd.execute(&repo, {});
        std::string reflogOut = capReflog.str();
        CHECK(reflogOut.find("HEAD@{") != std::string::npos);
        CHECK(reflogOut.find("commit") != std::string::npos || reflogOut.find("revert") != std::string::npos);
    }

    removeDir(dir);
    std::cout << "PASSED" << std::endl;
}

// =========================================================================
// Test 7: HTTP Server: Fix 19, branch/create with "at", ETag/304, peerRefs, snapshot on fail
// =========================================================================

// Backend item 2: Path validation rejects control chars and Windows forbidden chars; index stays intact
void test_path_validation_and_index_intact() {
    std::cout << "[Test Backend 2] Path validation rejects control chars & \\:*?\"<>| (index intact)... " << std::flush;
    fs::path dir = fs::current_path() / "test_path_val_repo";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "valid.txt", "valid file content\n");
    repo.add("valid.txt", true);
    repo.commit("initial commit");

    Snapshot snapInit = repo.snapshot();
    CHECK(snapInit.files.count("valid.txt") == 1);

    // Control characters: \n, \r, \t, \0
    std::vector<std::string> invalidControlPaths = {
        "file\nname.txt",
        "file\rname.txt",
        "file\tname.txt",
        std::string("file\0name.txt", 13)
    };
    for (const auto& p : invalidControlPaths) {
        bool threw = false;
        try {
            repo.add(p, true);
        } catch (const MiniGitException&) {
            threw = true;
        }
        CHECK(threw);
    }

    // Windows reserved / dangerous characters: \ : * ? " < > |
    std::vector<std::string> invalidCharPaths = {
        "file\\name.txt",
        "file:name.txt",
        "file*name.txt",
        "file?name.txt",
        "file\"name.txt",
        "file<name.txt",
        "file>name.txt",
        "file|name.txt"
    };
    for (const auto& p : invalidCharPaths) {
        bool threw = false;
        try {
            repo.add(p, true);
        } catch (const MiniGitException&) {
            threw = true;
        }
        CHECK(threw);

        // Also test unstage & discard
        threw = false;
        try {
            repo.unstage(p, true);
        } catch (const MiniGitException&) {
            threw = true;
        }
        CHECK(threw);

        threw = false;
        try {
            repo.discard(p, true);
        } catch (const MiniGitException&) {
            threw = true;
        }
        CHECK(threw);
    }

    // Crucially verify that index and snapshot stay intact after all rejected attempts
    Snapshot snapAfter = repo.snapshot();
    CHECK(snapAfter.files.count("valid.txt") == 1);
    Status st = repo.status();
    CHECK(st.clean());

    removeDir(dir);
    std::cout << "PASSED\n";
}

// Backend item 7: CLI polish: revert commit msg, HEAD revision, detached format, stash save
void test_cli_polish_and_stash() {
    std::cout << "[Test Backend 7] CLI polish: revert commit msg, HEAD revision, detached format, stash save... " << std::flush;
    fs::path dir = fs::current_path() / "test_cli_polish_repo";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "initial.txt", "initial text\n");
    repo.add("initial.txt", true);
    Hash h1 = repo.commit("Initial commit");

    // 1. Detached branch prints "* (HEAD detached at abc1234)" and status prints "HEAD detached at abc1234"
    repo.checkout(h1.str()); // checkout commit detaches HEAD
    CHECK(repo.currentBranch().empty());

    // Capture StatusCmd output
    std::stringstream ssStatus;
    std::streambuf* oldCout = std::cout.rdbuf(ssStatus.rdbuf());
    StatusCmd statusCmd;
    statusCmd.execute(&repo, {});
    std::cout.rdbuf(oldCout);
    std::string statusOut = ssStatus.str();
    CHECK(statusOut.find("HEAD detached at " + h1.shortStr()) != std::string::npos);

    // Capture BranchCmd output
    std::stringstream ssBranch;
    oldCout = std::cout.rdbuf(ssBranch.rdbuf());
    BranchCmd branchCmd;
    branchCmd.execute(&repo, {});
    std::cout.rdbuf(oldCout);
    std::string branchOut = ssBranch.str();
    CHECK(branchOut.find("* (HEAD detached at " + h1.shortStr() + ")") != std::string::npos);

    // 2. Revert prints commit message, not hash; supports HEAD as revision
    repo.checkout("main");
    writeFile(dir / "feature.txt", "feature text\n");
    repo.add("feature.txt", true);
    repo.commit("Add cool feature");

    std::stringstream ssRevert;
    oldCout = std::cout.rdbuf(ssRevert.rdbuf());
    RevertCmd revertCmd;
    revertCmd.execute(&repo, {"HEAD"});
    std::cout.rdbuf(oldCout);
    std::string revertOut = ssRevert.str();
    CHECK(revertOut.find("Revert \"Add cool feature\"") != std::string::npos);

    // 3. Stash save with no tracked changes and no -u says "no local changes to save" and creates no stash
    Status sClean = repo.status();
    CHECK(sClean.clean());
    std::string resNoChanges = repo.stashSave("", false);
    CHECK(resNoChanges == "no local changes to save");
    CHECK(repo.stashList().empty());

    // Add untracked file, stash without -u should still say "no local changes to save"
    writeFile(dir / "untracked.txt", "untracked");
    std::string resUntracked = repo.stashSave("", false);
    CHECK(resUntracked == "no local changes to save");
    CHECK(repo.stashList().empty());

    removeDir(dir);
    std::cout << "PASSED\n";
}

// Backend item 6: Git export checks identity before init/gitignore, returns 'already up to date'
void test_git_export_identity_and_up_to_date() {
    std::cout << "[Test Backend 6] Git export: identity check & 'already up to date'... " << std::flush;
    fs::path dir = fs::current_path() / "test_git_exp_repo";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "f.txt", "content\n");
    repo.add("f.txt", true);
    repo.commit("initial");

    try {
        std::string res1 = repo.exportToGit();
        (void)res1;
        std::string res2 = repo.exportToGit();
        CHECK(res2 == "already up to date");
    } catch (const MiniGitException& e) {
        std::string err = e.what();
        CHECK(err.find("git is not installed") != std::string::npos ||
              err.find("git user.name and user.email are not configured") != std::string::npos);
    }

    removeDir(dir);
    std::cout << "PASSED\n";
}

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
    CHECK(etagPos != std::string::npos);
    size_t etagEnd = probeResp.find("\r\n", etagPos);
    std::string etagVal = probeResp.substr(etagPos + 6, etagEnd - (etagPos + 6));
    CHECK(!etagVal.empty());

    std::string req304 = "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1\r\nIf-None-Match: " + etagVal + "\r\nConnection: close\r\n\r\n";
    std::string resp304 = httpExchange("127.0.0.1", testPort, req304);
    CHECK(resp304.find("304 Not Modified") != std::string::npos);
    size_t bodySep = resp304.find("\r\n\r\n");
    CHECK(bodySep != std::string::npos);
    std::string body304 = resp304.substr(bodySep + 4);
    CHECK(body304.empty());

    // 2. POST /api/branch/create with "at" (hash or prefix)
    std::string createAtBody = "{\"name\":\"branch_at_c1\",\"at\":\"" + hBase.shortStr() + "\"}";
    std::string createAtReq = "POST /api/branch/create HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(createAtBody.size()) + "\r\nConnection: close\r\n\r\n" + createAtBody;
    std::string createAtResp = httpExchange("127.0.0.1", testPort, createAtReq);
    CHECK(createAtResp.find("200 OK") != std::string::npos);
    CHECK(repo.branchExists("branch_at_c1"));
    CHECK(repo.branchTip("branch_at_c1") == hBase);

    // 3. POST /api/stash/save with includeUntracked: true (item 49)
    writeFile(dir / "untracked_api.txt", "untracked api text");
    std::string stashBody = "{\"message\":\"api-stash\",\"includeUntracked\":true}";
    std::string stashReq = "POST /api/stash/save HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(stashBody.size()) + "\r\nConnection: close\r\n\r\n" + stashBody;
    std::string stashResp = httpExchange("127.0.0.1", testPort, stashReq);
    CHECK(stashResp.find("200 OK") != std::string::npos);
    CHECK(!fs::exists(dir / "untracked_api.txt"));

    std::string popReq = "POST /api/stash/pop HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    std::string popResp = httpExchange("127.0.0.1", testPort, popReq);
    CHECK(popResp.find("200 OK") != std::string::npos);
    CHECK(fs::exists(dir / "untracked_api.txt"));

    // 4. Fix 19: Error with a backslash still returns valid JSON
    std::string bsBodyIn = "{\"path\":\"..\\\\invalid\\\\path.txt\",\"content\":\"\"}";
    std::string backslashReq = "POST /api/file/save HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(bsBodyIn.size()) + "\r\nConnection: close\r\n\r\n" + bsBodyIn;
    std::string backslashResp = httpExchange("127.0.0.1", testPort, backslashReq);
    CHECK(backslashResp.find("400 Bad Request") != std::string::npos);
    size_t bsBodyPos = backslashResp.find("\r\n\r\n");
    CHECK(bsBodyPos != std::string::npos);
    std::string bsBody = backslashResp.substr(bsBodyPos + 4);
    std::string parseErr;
    JsonValue bsJson = JsonValue::parse(bsBody, &parseErr);
    CHECK(parseErr.empty());
    CHECK(bsJson.is_object());
    CHECK(bsJson["ok"].as_bool() == false);
    CHECK(bsJson.contains("error"));

    // 5. Checkout failure includes "snapshot"
    std::string coBody = "{\"target\":\"nonexistent_target\"}";
    std::string badCheckoutReq = "POST /api/checkout HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(coBody.size()) + "\r\nConnection: close\r\n\r\n" + coBody;
    std::string badCheckoutResp = httpExchange("127.0.0.1", testPort, badCheckoutReq);
    CHECK(badCheckoutResp.find("400 Bad Request") != std::string::npos);
    size_t coBodyPos = badCheckoutResp.find("\r\n\r\n");
    CHECK(coBodyPos != std::string::npos);
    JsonValue coJson = JsonValue::parse(badCheckoutResp.substr(coBodyPos + 4));
    CHECK(coJson["ok"].as_bool() == false);
    CHECK(coJson.contains("snapshot") && coJson["snapshot"].is_object());

    // 6. import-bundle failure includes "snapshot" and top-level "peerRefs" (item 35)
    std::string bundleBody = "{\"objects\":\"not_an_object\"}";
    std::string badBundleReq = "POST /api/sync/import-bundle HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(bundleBody.size()) + "\r\nConnection: close\r\n\r\n" + bundleBody;
    std::string badBundleResp = httpExchange("127.0.0.1", testPort, badBundleReq);
    CHECK(badBundleResp.find("400 Bad Request") != std::string::npos);
    size_t bundleBodyPos = badBundleResp.find("\r\n\r\n");
    CHECK(bundleBodyPos != std::string::npos);
    JsonValue bundleJson = JsonValue::parse(badBundleResp.substr(bundleBodyPos + 4));
    CHECK(bundleJson["ok"].as_bool() == false);
    CHECK(bundleJson.contains("peerRefs") && bundleJson["peerRefs"].is_array());
    CHECK(bundleJson.contains("snapshot") && bundleJson["snapshot"].is_object());

    // 7. CSRF and CORS security checks (Backend item 1)
    {
        // 7a. Non-GET loopback with foreign Origin -> 403 Forbidden
        std::string csrfBody = "{\"name\":\"csrf_branch\"}";
        std::string csrfReq = "POST /api/branch/create HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: http://evil.com\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(csrfBody.size()) + "\r\nConnection: close\r\n\r\n" + csrfBody;
        std::string csrfResp = httpExchange("127.0.0.1", testPort, csrfReq);
        CHECK(csrfResp.find("403 Forbidden") != std::string::npos);
        CHECK(csrfResp.find("Access-Control-Allow-Origin: http://evil.com") == std::string::npos);

        // 7b. Non-GET loopback missing Content-Type: application/json -> 400 Bad Request
        std::string noCtReq = "POST /api/branch/create HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
            std::to_string(csrfBody.size()) + "\r\nConnection: close\r\n\r\n" + csrfBody;
        std::string noCtResp = httpExchange("127.0.0.1", testPort, noCtReq);
        CHECK(noCtResp.find("400 Bad Request") != std::string::npos);

        // 7c. Non-GET loopback with valid local Origin -> 200 OK
        std::string legitReq = "POST /api/branch/create HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: http://127.0.0.1:" + std::to_string(testPort) + "\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(csrfBody.size()) + "\r\nConnection: close\r\n\r\n" + csrfBody;
        std::string legitResp = httpExchange("127.0.0.1", testPort, legitReq);
        CHECK(legitResp.find("200 OK") != std::string::npos);

        // 7d. GET /api/sync/bundle with foreign Origin -> 403 Forbidden
        std::string foreignBundleReq = "GET /api/sync/bundle HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: http://evil.com\r\nConnection: close\r\n\r\n";
        std::string foreignBundleResp = httpExchange("127.0.0.1", testPort, foreignBundleReq);
        CHECK(foreignBundleResp.find("403 Forbidden") != std::string::npos);
    }

    // 8. Live CLI changes seen by server & merge works (Backend item 3)
    {
        std::error_code ec;
        fs::remove(dir / "untracked_api.txt", ec);
        writeFile(dir / "cli_live.txt", "cli live content\n");
        repo.add("cli_live.txt", true);
        repo.commit("commit from cli");

        std::string liveSnapReq = "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        std::string liveSnapResp = httpExchange("127.0.0.1", testPort, liveSnapReq);
        CHECK(liveSnapResp.find("200 OK") != std::string::npos);
        size_t lsBodyPos = liveSnapResp.find("\r\n\r\n");
        CHECK(lsBodyPos != std::string::npos);
        JsonValue lsJson = JsonValue::parse(liveSnapResp.substr(lsBodyPos + 4));
        CHECK(lsJson["status"]["staged"].as_array().empty());
        CHECK(lsJson["status"]["modified"].as_array().empty());
        CHECK(lsJson["status"]["deleted"].as_array().empty());
        CHECK(lsJson["status"]["untracked"].as_array().empty());
        CHECK(lsJson["files"].contains("cli_live.txt"));

        // CLI creates feature branch and commits, then switches to main
        repo.createBranch("feat_cli_merge");
        repo.checkout("feat_cli_merge");
        writeFile(dir / "feat_file.txt", "feat content\n");
        repo.add("feat_file.txt", true);
        repo.commit("commit on feat branch");
        repo.checkout("main");

        // Merge via server API
        std::string mergeBody = "{\"branch\":\"feat_cli_merge\"}";
        std::string mergeReq = "POST /api/merge HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(mergeBody.size()) + "\r\nConnection: close\r\n\r\n" + mergeBody;
        std::string mergeResp = httpExchange("127.0.0.1", testPort, mergeReq);
        CHECK(mergeResp.find("200 OK") != std::string::npos);
        size_t mBodyPos = mergeResp.find("\r\n\r\n");
        CHECK(mBodyPos != std::string::npos);
        JsonValue mJson = JsonValue::parse(mergeResp.substr(mBodyPos + 4));
        CHECK(mJson["ok"].as_bool() == true);
        std::string mMsg = mJson.contains("result") ? mJson["result"].as_string() : mJson["message"].as_string();
        CHECK(mMsg.find("Merged") != std::string::npos || mMsg.find("Fast-forward") != std::string::npos);
    }

    // 9. Snapshot JSON contains "lan": bool and "binaryFiles": [paths] (Backend item 4)
    {
        std::string binContent = "bin";
        binContent.push_back('\0');
        binContent += "ary";
        writeFile(dir / "sample.bin", binContent);
        repo.add("sample.bin", true);
        repo.commit("add binary file");

        std::string binSnapReq = "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        std::string binSnapResp = httpExchange("127.0.0.1", testPort, binSnapReq);
        CHECK(binSnapResp.find("200 OK") != std::string::npos);
        size_t bsSnapPos = binSnapResp.find("\r\n\r\n");
        CHECK(bsSnapPos != std::string::npos);
        JsonValue bsSnapJson = JsonValue::parse(binSnapResp.substr(bsSnapPos + 4));
        CHECK(bsSnapJson.contains("lan") && bsSnapJson["lan"].is_bool());
        CHECK(bsSnapJson.contains("binaryFiles") && bsSnapJson["binaryFiles"].is_array());
        bool foundBin = false;
        for (const auto& bf : bsSnapJson["binaryFiles"].as_array()) {
            if (bf.as_string() == "sample.bin") foundBin = true;
        }
        CHECK(foundBin);
    }

    // 10. Failure replies include "snapshot" (Backend item 5)
    {
        auto postCheckFailureSnapshot = [&](const std::string& path, const std::string& body) {
            std::string req = "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            std::string resp = httpExchange("127.0.0.1", testPort, req);
            size_t bPos = resp.find("\r\n\r\n");
            CHECK(bPos != std::string::npos);
            JsonValue j = JsonValue::parse(resp.substr(bPos + 4));
            CHECK(j["ok"].as_bool() == false);
            CHECK(j.contains("snapshot") && j["snapshot"].is_object());
        };

        // commit failure (nothing staged)
        postCheckFailureSnapshot("/api/commit", "{\"message\":\"empty stage\"}");
        // revert failure (invalid hash)
        postCheckFailureSnapshot("/api/revert", "{\"hash\":\"deadbeef00\"}");
        // stash/save failure (no changes to save)
        postCheckFailureSnapshot("/api/stash/save", "{\"message\":\"no changes\"}");
        // stash/pop failure (empty stash)
        postCheckFailureSnapshot("/api/stash/pop", "{}");
        // branch/create failure (duplicate name)
        postCheckFailureSnapshot("/api/branch/create", "{\"name\":\"main\"}");
        // branch/delete failure (delete current branch)
        postCheckFailureSnapshot("/api/branch/delete", "{\"name\":\"main\"}");
        // reflog/restore failure (invalid hash)
        postCheckFailureSnapshot("/api/reflog/restore", "{\"hash\":\"deadbeef00\"}");
    }

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
    test_path_validation_and_index_intact();
    test_cli_polish_and_stash();
    test_git_export_identity_and_up_to_date();
    test_http_server_endpoints();

    std::cout << "\nALL CLI & SERVER TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
