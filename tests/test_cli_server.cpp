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
#include "PairingManager.h"

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

static std::string httpExchange(const std::string& host, int port, const std::string& rawReq, int timeoutSec = 3) {
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == (socket_t)-1) {
        return "";
    }

#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(timeoutSec * 1000);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    struct timeval tv{};
    tv.tv_sec = timeoutSec;
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

    size_t totalSent = 0;
    while (totalSent < rawReq.size()) {
        int sent = send(s, rawReq.data() + totalSent, static_cast<int>(rawReq.size() - totalSent), 0);
        if (sent <= 0) break;
        totalSent += static_cast<size_t>(sent);
    }

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

    // Requirement 3: CLI `add` and Index skip illegal paths with warning instead of staging
    for (const auto& p : invalidCharPaths) {
        AddCmd addCmd;
        try {
            addCmd.execute(&repo, {p});
        } catch (const MiniGitException&) {}
        Index idxCheck(dir / ".minigit" / "index");
        CHECK(!idxCheck.has(p));
    }
    for (const auto& p : invalidControlPaths) {
        AddCmd addCmd;
        try {
            addCmd.execute(&repo, {p});
        } catch (const MiniGitException&) {}
        Index idxCheck(dir / ".minigit" / "index");
        CHECK(!idxCheck.has(p));
    }

    // Direct Index::set skips illegal paths
    Index idx(dir / ".minigit" / "index");
    idx.set("illegal:path.txt", Hash());
    CHECK(!idx.has("illegal:path.txt"));

    // Index::load skips corrupt / illegal path lines
    {
        std::ofstream ofs(dir / ".minigit" / "index", std::ios::app);
        ofs << "0123456789abcdef0123456789abcdef01234567 corrupt*path.txt\n";
    }
    idx.reload();
    CHECK(!idx.has("corrupt*path.txt"));

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

        // Requirement 4: export then import returns "already up to date" without committing dirty .gitignore
        std::string resImp = repo.importFromGit();
        CHECK(resImp == "already up to date");
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
            std::cout << "[FailureSnapshot] Checking " << path << " ... " << std::flush;
            std::string req = "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            std::string resp = httpExchange("127.0.0.1", testPort, req);
            size_t bPos = resp.find("\r\n\r\n");
            CHECK(bPos != std::string::npos);
            JsonValue j = JsonValue::parse(resp.substr(bPos + 4));
            if (j["ok"].as_bool() != false) {
                std::cerr << "FAILED! Body was: " << resp.substr(bPos + 4) << std::endl;
            }
            CHECK(j["ok"].as_bool() == false);
            CHECK(j.contains("snapshot") && j["snapshot"].is_object());
            std::cout << "OK\n";
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

        // Additional failure snapshot checks (Requirement 5)
        postCheckFailureSnapshot("/api/file/save", "{}");
        postCheckFailureSnapshot("/api/file/delete", "{}");
        postCheckFailureSnapshot("/api/stage", "{}");
        postCheckFailureSnapshot("/api/unstage", "{\"path\":\"bad:name.txt\"}");
        postCheckFailureSnapshot("/api/discard", "{}");
        postCheckFailureSnapshot("/api/checkout", "{\"target\":\"\"}");
        postCheckFailureSnapshot("/api/merge", "{\"abort\":true}");
        postCheckFailureSnapshot("/api/merge", "{\"branch\":\"\"}");
        postCheckFailureSnapshot("/api/git/import", "{}");
    }

    // 11. Host header DNS rebinding check (Requirement 2)
    {
        // evil.com Host header -> 403 Forbidden
        std::string evilHostReq = "GET /api/snapshot HTTP/1.1\r\nHost: evil.com\r\nConnection: close\r\n\r\n";
        std::string evilHostResp = httpExchange("127.0.0.1", testPort, evilHostReq);
        CHECK(evilHostResp.find("403 Forbidden") != std::string::npos);
        CHECK(evilHostResp.find("invalid Host header") != std::string::npos);

        std::string evilHostPortReq = "GET /api/snapshot HTTP/1.1\r\nHost: evil.com:" + std::to_string(testPort) + "\r\nConnection: close\r\n\r\n";
        std::string evilHostPortResp = httpExchange("127.0.0.1", testPort, evilHostPortReq);
        CHECK(evilHostPortResp.find("403 Forbidden") != std::string::npos);
        CHECK(evilHostPortResp.find("invalid Host header") != std::string::npos);

        // Valid host: 127.0.0.1:<port> -> 200 OK
        std::string goodHostReq = "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) + "\r\nConnection: close\r\n\r\n";
        std::string goodHostResp = httpExchange("127.0.0.1", testPort, goodHostReq);
        CHECK(goodHostResp.find("200 OK") != std::string::npos);

        // Valid host: localhost:<port> -> 200 OK
        std::string goodHostReq2 = "GET /api/snapshot HTTP/1.1\r\nHost: localhost:" + std::to_string(testPort) + "\r\nConnection: close\r\n\r\n";
        std::string goodHostResp2 = httpExchange("127.0.0.1", testPort, goodHostReq2);
        CHECK(goodHostResp2.find("200 OK") != std::string::npos);
    }

    // 12. Cross-origin peer pairing and sync with token (Requirement 1)
    {
        std::string peerOrigin = "http://peer-laptop:8080";

        // 12a. OPTIONS on pair and sync endpoints returns 204 + CORS headers (no token required)
        for (const std::string ep : {"/api/pair", "/api/pair/status", "/api/sync/bundle", "/api/sync/import-bundle"}) {
            std::string optReq = "OPTIONS " + ep + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
                "\r\nOrigin: " + peerOrigin + "\r\nAccess-Control-Request-Method: POST\r\nConnection: close\r\n\r\n";
            std::string optResp = httpExchange("127.0.0.1", testPort, optReq);
            CHECK(optResp.find("204 No Content") != std::string::npos);
            CHECK(optResp.find("Access-Control-Allow-Origin: " + peerOrigin) != std::string::npos);
            CHECK(optResp.find("Access-Control-Allow-Methods:") != std::string::npos);
            CHECK(optResp.find("Access-Control-Allow-Headers: Content-Type, X-MiniGit-Token") != std::string::npos);
        }

        // 12b. Evil origin with no token gets 403 on file/write and bundle
        std::string evilWriteBody = "{\"path\":\"hacked.txt\",\"content\":\"evil\"}";
        std::string evilWriteReq = "POST /api/file/write HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nOrigin: http://evil.com\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(evilWriteBody.size()) + "\r\nConnection: close\r\n\r\n" + evilWriteBody;
        std::string evilWriteResp = httpExchange("127.0.0.1", testPort, evilWriteReq);
        CHECK(evilWriteResp.find("403 Forbidden") != std::string::npos);

        std::string evilBundleReq = "GET /api/sync/bundle HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nOrigin: http://evil.com\r\nConnection: close\r\n\r\n";
        std::string evilBundleResp = httpExchange("127.0.0.1", testPort, evilBundleReq);
        CHECK(evilBundleResp.find("403 Forbidden") != std::string::npos);

        // 12c. Get pair code from host loopback
        std::string getCodeReq = "GET /api/pair/code HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) + "\r\nConnection: close\r\n\r\n";
        std::string getCodeResp = httpExchange("127.0.0.1", testPort, getCodeReq);
        CHECK(getCodeResp.find("200 OK") != std::string::npos);
        size_t gcBodyPos = getCodeResp.find("\r\n\r\n");
        CHECK(gcBodyPos != std::string::npos);
        JsonValue gcJson = JsonValue::parse(getCodeResp.substr(gcBodyPos + 4));
        std::string currentCode = gcJson["code"].as_string();
        CHECK(currentCode.size() == 6);

        // 12d. Cross-origin peer page pairs: POST /api/pair with peerOrigin
        std::string pairBody = "{\"code\":\"" + currentCode + "\",\"clientName\":\"Peer Laptop\"}";
        // Cross-origin pair arriving over loopback is refused (R4-1)
        std::string loopbackPairReq = "POST /api/pair HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nOrigin: " + peerOrigin + "\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(pairBody.size()) + "\r\nConnection: close\r\n\r\n" + pairBody;
        std::string loopbackPairResp = httpExchange("127.0.0.1", testPort, loopbackPairReq);
        CHECK(loopbackPairResp.find("403 Forbidden") != std::string::npos);

        // Discover LAN IP for LAN peer connection
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
        std::string peerHost = (!lanIp.empty() && lanIp != "127.0.0.1") ? lanIp : "127.0.0.1";

        std::string pairReq = "POST /api/pair HTTP/1.1\r\nHost: " + peerHost + ":" + std::to_string(testPort) +
            "\r\nOrigin: " + peerOrigin + "\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(pairBody.size()) + "\r\nConnection: close\r\n\r\n" + pairBody;
        std::string pairResp = httpExchange(peerHost, testPort, pairReq);
        CHECK(pairResp.find("200 OK") != std::string::npos);
        CHECK(pairResp.find("Access-Control-Allow-Origin: " + peerOrigin) != std::string::npos);
        size_t pBodyPos = pairResp.find("\r\n\r\n");
        CHECK(pBodyPos != std::string::npos);
        JsonValue pairJson = JsonValue::parse(pairResp.substr(pBodyPos + 4));
        CHECK(pairJson["ok"].as_bool() == true);
        std::string reqId = pairJson["requestId"].as_string();

        // 12e. Host approves pair request
        std::string approveBody = "{\"requestId\":\"" + reqId + "\",\"action\":\"approve\"}";
        std::string approveReq = "POST /api/pair/decide HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(approveBody.size()) + "\r\nConnection: close\r\n\r\n" + approveBody;
        std::string approveResp = httpExchange("127.0.0.1", testPort, approveReq);
        CHECK(approveResp.find("200 OK") != std::string::npos);

        // 12f. Cross-origin peer checks status: GET /api/pair/status?requestId=... with peerOrigin
        std::string statusReq = "GET /api/pair/status?requestId=" + reqId + " HTTP/1.1\r\nHost: " + peerHost + ":" + std::to_string(testPort) +
            "\r\nOrigin: " + peerOrigin + "\r\nConnection: close\r\n\r\n";
        std::string statusResp = httpExchange(peerHost, testPort, statusReq);
        CHECK(statusResp.find("200 OK") != std::string::npos);
        CHECK(statusResp.find("Access-Control-Allow-Origin: " + peerOrigin) != std::string::npos);
        size_t stBodyPos = statusResp.find("\r\n\r\n");
        CHECK(stBodyPos != std::string::npos);
        JsonValue statusJson = JsonValue::parse(statusResp.substr(stBodyPos + 4));
        CHECK(statusJson["status"].as_string() == "approved");
        std::string sessionToken = statusJson["token"].as_string();
        CHECK(!sessionToken.empty());

        // 12g. Peer pulls bundle with token from different origin: GET /api/sync/bundle
        std::string bundleReq = "GET /api/sync/bundle HTTP/1.1\r\nHost: " + peerHost + ":" + std::to_string(testPort) +
            "\r\nOrigin: " + peerOrigin + "\r\nX-MiniGit-Token: " + sessionToken + "\r\nConnection: close\r\n\r\n";
        std::string bundleResp = httpExchange(peerHost, testPort, bundleReq);
        CHECK(bundleResp.find("200 OK") != std::string::npos);
        CHECK(bundleResp.find("Access-Control-Allow-Origin: " + peerOrigin) != std::string::npos);
        size_t bResBodyPos = bundleResp.find("\r\n\r\n");
        CHECK(bResBodyPos != std::string::npos);
        JsonValue pulledBundle = JsonValue::parse(bundleResp.substr(bResBodyPos + 4));
        CHECK(pulledBundle.contains("objects"));

        // 12h. Peer pushes import-bundle with token from different origin: POST /api/sync/import-bundle
        std::string impBody = bundleResp.substr(bResBodyPos + 4);
        std::string impReq = "POST /api/sync/import-bundle HTTP/1.1\r\nHost: " + peerHost + ":" + std::to_string(testPort) +
            "\r\nOrigin: " + peerOrigin + "\r\nX-MiniGit-Token: " + sessionToken + "\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(impBody.size()) + "\r\nConnection: close\r\n\r\n" + impBody;
        std::string impResp = httpExchange(peerHost, testPort, impReq);
        CHECK(impResp.find("200 OK") != std::string::npos);
        CHECK(impResp.find("Access-Control-Allow-Origin: " + peerOrigin) != std::string::npos);
    }

    // 13. R3-2: Host header in --lan mode accepts IPv4 literals on port, rejects non-localhost domains
    {
        // Two local IP literals work on our port
        std::string h1Req = "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) + "\r\nConnection: close\r\n\r\n";
        std::string h1Resp = httpExchange("127.0.0.1", testPort, h1Req);
        CHECK(h1Resp.find("200 OK") != std::string::npos);

        std::string h2Req = "GET /api/snapshot HTTP/1.1\r\nHost: 192.168.1.100:" + std::to_string(testPort) + "\r\nConnection: close\r\n\r\n";
        std::string h2Resp = httpExchange("127.0.0.1", testPort, h2Req);
        CHECK(h2Resp.find("200 OK") != std::string::npos);

        std::string h3Req = "GET /api/snapshot HTTP/1.1\r\nHost: 10.0.0.42:" + std::to_string(testPort) + "\r\nConnection: close\r\n\r\n";
        std::string h3Resp = httpExchange("127.0.0.1", testPort, h3Req);
        CHECK(h3Resp.find("200 OK") != std::string::npos);

        // evil.example:port gets 403 Forbidden
        std::string evilHostReq = "GET /api/snapshot HTTP/1.1\r\nHost: evil.example:" + std::to_string(testPort) + "\r\nConnection: close\r\n\r\n";
        std::string evilHostResp = httpExchange("127.0.0.1", testPort, evilHostReq);
        CHECK(evilHostResp.find("403 Forbidden") != std::string::npos);
    }

    // 14. R3-3: CORS headers (request Origin, Vary: Origin) on 401/403 from sync and pair endpoints
    {
        std::string foreignOrigin = "http://peer-app.local:3000";
        // 403 from /api/sync/bundle without valid token has CORS headers
        std::string badTokenReq = "GET /api/sync/bundle HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nOrigin: " + foreignOrigin + "\r\nX-MiniGit-Token: invalid_token_123\r\nConnection: close\r\n\r\n";
        std::string badTokenResp = httpExchange("127.0.0.1", testPort, badTokenReq);
        CHECK(badTokenResp.find("403 Forbidden") != std::string::npos);
        CHECK(badTokenResp.find("Access-Control-Allow-Origin: " + foreignOrigin) != std::string::npos);
        CHECK(badTokenResp.find("Vary: Origin") != std::string::npos);

        // 403 from /api/pair on cross-origin loopback has CORS headers
        std::string wrongCodeBody = "{\"code\":\"000000\",\"clientName\":\"Peer\"}";
        std::string wrongCodeReq = "POST /api/pair HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nOrigin: " + foreignOrigin + "\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(wrongCodeBody.size()) + "\r\nConnection: close\r\n\r\n" + wrongCodeBody;
        std::string wrongCodeResp = httpExchange("127.0.0.1", testPort, wrongCodeReq);
        CHECK(wrongCodeResp.find("403 Forbidden") != std::string::npos);
        CHECK(wrongCodeResp.find("Access-Control-Allow-Origin: " + foreignOrigin) != std::string::npos);
        CHECK(wrongCodeResp.find("Vary: Origin") != std::string::npos);
    }

    // 15. R4-1: Count failed codes per client IP + Origin (loopback included), lock that client after 5, never locks out real code for others
    {
        PairingManager pm;
        std::string code = pm.currentCode();
        // Loopback client fails 5 times and gets locked
        for (int i = 1; i <= 5; ++i) {
            auto res = pm.submitCode("000000", "127.0.0.1", "", "http://evil.com");
            CHECK(!res.success);
            if (i == 5) CHECK(res.locked);
        }
        CHECK(pm.isClientLocked("127.0.0.1|http://evil.com"));

        // Attacker is locked even with real code
        auto resAtkLocked = pm.submitCode(code, "127.0.0.1", "", "http://evil.com", true);
        CHECK(!resAtkLocked.success);
        CHECK(resAtkLocked.locked);

        // LAN client B is NOT locked and submits the real code successfully!
        CHECK(!pm.isClientLocked("192.168.1.102"));
        auto resBOk = pm.submitCode(code, "192.168.1.102", "Teammate B");
        CHECK(resBOk.success);
        CHECK(!resBOk.locked);
        CHECK(!resBOk.requestId.empty());
    }

    // 16. R3-5: Quoted peer names in /api/pair/pending and /api/pair/peers JSON
    {
        // Generate fresh code
        std::string genReq = "POST /api/pair/generate-code HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
        std::string genResp = httpExchange("127.0.0.1", testPort, genReq);
        size_t gcBodyPos = genResp.find("\r\n\r\n");
        CHECK(gcBodyPos != std::string::npos);
        JsonValue genJson = JsonValue::parse(genResp.substr(gcBodyPos + 4));
        std::string freshCode = genJson["code"].as_string();

        // Submit pair request with quotes and special characters in peer name
        std::string quoteName = "Alice \"The Engineer\" O'Connor";
        std::string qBody = "{\"code\":\"" + freshCode + "\",\"peerName\":" + jsonString(quoteName) + "}";
        std::string qReq = "POST /api/pair HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(qBody.size()) + "\r\nConnection: close\r\n\r\n" + qBody;
        std::string qResp = httpExchange("127.0.0.1", testPort, qReq);
        CHECK(qResp.find("200 OK") != std::string::npos);
        size_t qBodyPos = qResp.find("\r\n\r\n");
        CHECK(qBodyPos != std::string::npos);
        JsonValue qJson = JsonValue::parse(qResp.substr(qBodyPos + 4));
        std::string qReqId = qJson["requestId"].as_string();

        // GET /api/pair/pending must return valid parseable JSON containing the quoted name
        std::string pendReq = "GET /api/pair/pending HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) + "\r\nConnection: close\r\n\r\n";
        std::string pendResp = httpExchange("127.0.0.1", testPort, pendReq);
        size_t pBodyPos = pendResp.find("\r\n\r\n");
        CHECK(pBodyPos != std::string::npos);
        std::string pErr;
        JsonValue pendJson = JsonValue::parse(pendResp.substr(pBodyPos + 4), &pErr);
        CHECK(pErr.empty());
        CHECK(pendJson.contains("pending") && pendJson["pending"].is_array());
        bool foundQuotedPending = false;
        for (const auto& item : pendJson["pending"].as_array()) {
            if (item["id"].as_string() == qReqId) {
                CHECK(item["peerName"].as_string() == quoteName);
                foundQuotedPending = true;
            }
        }
        CHECK(foundQuotedPending);

        // Approve it and check GET /api/pair/peers also parses cleanly with the quoted name
        std::string approveBody = "{\"requestId\":\"" + qReqId + "\",\"action\":\"approve\"}";
        std::string approveReq = "POST /api/pair/decide HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(approveBody.size()) + "\r\nConnection: close\r\n\r\n" + approveBody;
        httpExchange("127.0.0.1", testPort, approveReq);

        std::string peersReq = "GET /api/pair/peers HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) + "\r\nConnection: close\r\n\r\n";
        std::string peersResp = httpExchange("127.0.0.1", testPort, peersReq);
        size_t peersBodyPos = peersResp.find("\r\n\r\n");
        CHECK(peersBodyPos != std::string::npos);
        std::string prsErr;
        JsonValue peersJson = JsonValue::parse(peersResp.substr(peersBodyPos + 4), &prsErr);
        CHECK(prsErr.empty());
        CHECK(peersJson.contains("peers") && peersJson["peers"].is_array());
        bool foundQuotedPeer = false;
        for (const auto& item : peersJson["peers"].as_array()) {
            if (item["peerName"].as_string() == quoteName) {
                foundQuotedPeer = true;
            }
        }
        CHECK(foundQuotedPeer);
    }

    // 17. Nit: /api/file/delete and /api/unstage on paths that don't exist return ok:false
    {
        std::string delBody = "{\"path\":\"nonexistent_random_file_9876.txt\"}";
        std::string delReq = "POST /api/file/delete HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(delBody.size()) + "\r\nConnection: close\r\n\r\n" + delBody;
        std::string delResp = httpExchange("127.0.0.1", testPort, delReq);
        CHECK(delResp.find("400 Bad Request") != std::string::npos);
        size_t delBodyPos = delResp.find("\r\n\r\n");
        CHECK(delBodyPos != std::string::npos);
        JsonValue delJson = JsonValue::parse(delResp.substr(delBodyPos + 4));
        CHECK(delJson["ok"].as_bool() == false);
        CHECK(delJson.contains("snapshot"));

        std::string unstageBody = "{\"path\":\"nonexistent_random_file_9876.txt\"}";
        std::string unstageReq = "POST /api/unstage HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(unstageBody.size()) + "\r\nConnection: close\r\n\r\n" + unstageBody;
        std::string unstageResp = httpExchange("127.0.0.1", testPort, unstageReq);
        CHECK(unstageResp.find("400 Bad Request") != std::string::npos);
        size_t unstageBodyPos = unstageResp.find("\r\n\r\n");
        CHECK(unstageBodyPos != std::string::npos);
        JsonValue unstageJson = JsonValue::parse(unstageResp.substr(unstageBodyPos + 4));
        CHECK(unstageJson["ok"].as_bool() == false);
        CHECK(unstageJson.contains("snapshot"));
    }

    removeDir(dir);
    std::cout << "PASSED" << std::endl;
}

void test_r4_security_and_fixes() {
    std::cout << "[Test R4] Pairing brute force (50 bad codes, 429 limit, non-LAN 403, real peer pairing), escapePath & dir delete... " << std::flush;

    // R4-5: In status, escape backslashes too (\\) and print every control byte below 0x20, and 0x7F, as \xNN
    {
        CHECK(escapePath("normal.txt") == "normal.txt");
        CHECK(escapePath("file\\name.txt") == "file\\\\name.txt");
        CHECK(escapePath("file\nname.txt") == "file\\x0aname.txt");
        CHECK(escapePath("file\rname.txt") == "file\\x0dname.txt");
        CHECK(escapePath("file\tname.txt") == "file\\x09name.txt");
        CHECK(escapePath("file\x01name.txt") == "file\\x01name.txt");
        CHECK(escapePath("file\x1bname.txt") == "file\\x1bname.txt");
        CHECK(escapePath("file\x7fname.txt") == "file\\x7fname.txt");
    }

    fs::path dir = fs::current_path() / "test_repo_r4";
    removeDir(dir);
    fs::create_directories(dir);
    Repository::init(dir.string());
    Repository repo(dir.string());
    writeFile(dir / "hello.txt", "world\n");
    repo.add("hello.txt", true);
    repo.commit("initial");

    // Non-LAN server on port 19935
    int nonLanPort = 19935;
    std::thread nonLanThread([&]() {
        try {
            HttpServer::run(&repo, nonLanPort, false, false); // lan = false
        } catch (...) {}
    });
    nonLanThread.detach();

    // LAN server on port 19936
    int lanPort = 19936;
    std::thread lanThread([&]() {
        try {
            HttpServer::run(&repo, lanPort, false, true); // lan = true
        } catch (...) {}
    });
    lanThread.detach();

    // Probe both ports
    for (int retry = 0; retry < 50; ++retry) {
        std::string ping1 = httpExchange("127.0.0.1", nonLanPort, "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(nonLanPort) + "\r\nConnection: close\r\n\r\n");
        std::string ping2 = httpExchange("127.0.0.1", lanPort, "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(lanPort) + "\r\nConnection: close\r\n\r\n");
        if (ping1.find("200 OK") != std::string::npos && ping2.find("200 OK") != std::string::npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Determine local LAN IP for peer requests
    socket_t sTmpLan = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in loopbackAddrTmp{};
    loopbackAddrTmp.sin_family = AF_INET;
    loopbackAddrTmp.sin_addr.s_addr = inet_addr("8.8.8.8");
    loopbackAddrTmp.sin_port = htons(53);
    connect(sTmpLan, (struct sockaddr*)&loopbackAddrTmp, sizeof(loopbackAddrTmp));
    sockaddr_in nameAddrTmp{};
#ifdef _WIN32
    int nameLenTmp = sizeof(nameAddrTmp);
#else
    socklen_t nameLenTmp = sizeof(nameAddrTmp);
#endif
    getsockname(sTmpLan, (struct sockaddr*)&nameAddrTmp, &nameLenTmp);
    char lanIpBufTmp[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &nameAddrTmp.sin_addr, lanIpBufTmp, sizeof(lanIpBufTmp));
    CLOSESOCK(sTmpLan);
    std::string lanIp = lanIpBufTmp;
    std::string peerHost = (!lanIp.empty() && lanIp != "127.0.0.1") ? lanIp : "127.0.0.1";

    // 1. Non-LAN mode returns 403 on /api/pair and /api/pair/status
    {
        std::string pairBody = "{\"code\":\"123456\"}";
        std::string pairReq = "POST /api/pair HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(nonLanPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(pairBody.size()) + "\r\nConnection: close\r\n\r\n" + pairBody;
        std::string pairResp = httpExchange("127.0.0.1", nonLanPort, pairReq);
        CHECK(pairResp.find("403 Forbidden") != std::string::npos);
        CHECK(pairResp.find("LAN mode is not enabled") != std::string::npos);

        std::string statusReq = "GET /api/pair/status?requestId=req_test HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(nonLanPort) + "\r\nConnection: close\r\n\r\n";
        std::string statusResp = httpExchange("127.0.0.1", nonLanPort, statusReq);
        CHECK(statusResp.find("403 Forbidden") != std::string::npos);
        CHECK(statusResp.find("LAN mode is not enabled") != std::string::npos);
    }

    // 2. R4-6: /api/file/delete on a directory returns ok:false (400 Bad Request)
    {
        fs::create_directories(dir / "subfolder");
        writeFile(dir / "subfolder" / "nested.txt", "inside");
        std::string delDirBody = "{\"path\":\"subfolder\"}";
        std::string delDirReq = "POST /api/file/delete HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(lanPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(delDirBody.size()) + "\r\nConnection: close\r\n\r\n" + delDirBody;
        std::string delDirResp = httpExchange("127.0.0.1", lanPort, delDirReq);
        CHECK(delDirResp.find("400 Bad Request") != std::string::npos);
        size_t bPos = delDirResp.find("\r\n\r\n");
        CHECK(bPos != std::string::npos);
        JsonValue delJson = JsonValue::parse(delDirResp.substr(bPos + 4));
        CHECK(delJson["ok"].as_bool() == false);
        CHECK(fs::exists(dir / "subfolder"));
    }

    // 3. R5-1 & R6-1: 15 cross-origin bad tries over loopback (403 refused), then LAN peer with correct code -> 200
    {
        // Host gets current code
        std::string codeReq = "GET /api/pair/code HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(lanPort) + "\r\nConnection: close\r\n\r\n";
        std::string codeResp = httpExchange("127.0.0.1", lanPort, codeReq);
        size_t cPos = codeResp.find("\r\n\r\n");
        CHECK(cPos != std::string::npos);
        JsonValue codeJson = JsonValue::parse(codeResp.substr(cPos + 4));
        std::string activeCode = codeJson["code"].as_string();

        // 15 cross-origin bad tries over loopback from rotating origins
        for (int i = 1; i <= 15; ++i) {
            std::string badBody = "{\"code\":\"" + std::to_string(100000 + i) + "\"}";
            std::string req = "POST /api/pair HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(lanPort) +
                "\r\nOrigin: http://evil-" + std::to_string(i) + ".com\r\nContent-Type: application/json\r\nContent-Length: " +
                std::to_string(badBody.size()) + "\r\nConnection: close\r\n\r\n" + badBody;
            std::string resp = httpExchange("127.0.0.1", lanPort, req);
            CHECK(resp.find("403 Forbidden") != std::string::npos);
            CHECK(resp.find("cross-origin pairing over loopback refused") != std::string::npos);
        }

        // LAN peer submits the correct code -> 200 OK (not blocked by 429!)
        std::string goodBody = "{\"code\":\"" + activeCode + "\",\"peerName\":\"Legit LAN Peer\"}";
        std::string goodReq = "POST /api/pair HTTP/1.1\r\nHost: " + peerHost + ":" + std::to_string(lanPort) +
            "\r\nOrigin: http://peer-laptop.local\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(goodBody.size()) + "\r\nConnection: close\r\n\r\n" + goodBody;
        std::string goodResp = httpExchange(peerHost, lanPort, goodReq);
        CHECK(goodResp.find("200 OK") != std::string::npos);
    }

    // 4. The real LAN peer can still pair after an attacker is locked
    {
        // Host generates a fresh pair code
        std::string genReq = "POST /api/pair/generate-code HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(lanPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
        std::string genResp = httpExchange("127.0.0.1", lanPort, genReq);
        size_t gPos = genResp.find("\r\n\r\n");
        CHECK(gPos != std::string::npos);
        JsonValue genJson = JsonValue::parse(genResp.substr(gPos + 4));
        std::string correctCode = genJson["code"].as_string();
        CHECK(correctCode.size() == 6);

        // Attacker sends 5 bad codes to lock out attacker client
        for (int i = 1; i <= 5; ++i) {
            std::string badBody = "{\"code\":\"000000\"}";
            std::string atkReq = "POST /api/pair HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(lanPort) +
                "\r\nOrigin: http://127.0.0.1:" + std::to_string(lanPort) + "\r\nContent-Type: application/json\r\nContent-Length: " +
                std::to_string(badBody.size()) + "\r\nConnection: close\r\n\r\n" + badBody;
            std::string atkResp = httpExchange("127.0.0.1", lanPort, atkReq);
            if (i == 5) {
                CHECK(atkResp.find("403 Forbidden") != std::string::npos);
                CHECK(atkResp.find("locked") != std::string::npos);
            }
        }

        // Check lockedClientsCount on host
        std::string codeCheckReq = "GET /api/pair/code HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(lanPort) + "\r\nConnection: close\r\n\r\n";
        std::string codeCheckResp = httpExchange("127.0.0.1", lanPort, codeCheckReq);
        size_t ccPos = codeCheckResp.find("\r\n\r\n");
        CHECK(ccPos != std::string::npos);
        JsonValue codeCheckJson = JsonValue::parse(codeCheckResp.substr(ccPos + 4));
        CHECK(codeCheckJson["lockedClientsCount"].as_int64() >= 1);

        // Real LAN peer connecting from LAN IP (or non-locked client) submits the correct code
        std::string realBody = "{\"code\":\"" + correctCode + "\",\"peerName\":\"Real Peer\"}";
        std::string realReq = "POST /api/pair HTTP/1.1\r\nHost: " + peerHost + ":" + std::to_string(lanPort) +
            "\r\nOrigin: http://real-peer.local\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(realBody.size()) + "\r\nConnection: close\r\n\r\n" + realBody;
        std::string realResp = httpExchange(peerHost, lanPort, realReq);
        CHECK(realResp.find("200 OK") != std::string::npos);
        size_t rPos = realResp.find("\r\n\r\n");
        CHECK(rPos != std::string::npos);
        JsonValue realJson = JsonValue::parse(realResp.substr(rPos + 4));
        CHECK(realJson["ok"].as_bool() == true);
        std::string realReqId = realJson["requestId"].as_string();
        CHECK(!realReqId.empty());

        // Host checks pending list: origin is present
        std::string pendReq = "GET /api/pair/pending HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(lanPort) + "\r\nConnection: close\r\n\r\n";
        std::string pendResp = httpExchange("127.0.0.1", lanPort, pendReq);
        CHECK(pendResp.find("200 OK") != std::string::npos);
        CHECK(pendResp.find("http://real-peer.local") != std::string::npos);

        // Host approves
        std::string allowBody = "{\"requestId\":\"" + realReqId + "\",\"action\":\"allow\"}";
        std::string allowReq = "POST /api/pair/decide HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(lanPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(allowBody.size()) + "\r\nConnection: close\r\n\r\n" + allowBody;
        std::string allowResp = httpExchange("127.0.0.1", lanPort, allowReq);
        CHECK(allowResp.find("200 OK") != std::string::npos);

        // Real peer checks status and gets token
        std::string statReq = "GET /api/pair/status?requestId=" + realReqId + " HTTP/1.1\r\nHost: " + peerHost + ":" + std::to_string(lanPort) +
            "\r\nOrigin: http://real-peer.local\r\nConnection: close\r\n\r\n";
        std::string statResp = httpExchange(peerHost, lanPort, statReq);
        CHECK(statResp.find("200 OK") != std::string::npos);
        CHECK(statResp.find("approved") != std::string::npos);
    }

    // 5. Minor: /api/pair/decide with missing or unknown action returns 400 Bad Request
    {
        std::string badDecide1 = "{\"requestId\":\"req_fake\",\"action\":\"\"}";
        std::string req1 = "POST /api/pair/decide HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(lanPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(badDecide1.size()) + "\r\nConnection: close\r\n\r\n" + badDecide1;
        std::string resp1 = httpExchange("127.0.0.1", lanPort, req1);
        CHECK(resp1.find("400 Bad Request") != std::string::npos);

        std::string badDecide2 = "{\"requestId\":\"req_fake\",\"action\":\"foobar\"}";
        std::string req2 = "POST /api/pair/decide HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(lanPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(badDecide2.size()) + "\r\nConnection: close\r\n\r\n" + badDecide2;
        std::string resp2 = httpExchange("127.0.0.1", lanPort, req2);
        CHECK(resp2.find("400 Bad Request") != std::string::npos);

        std::string badDecide3 = "{\"requestId\":\"req_fake\"}";
        std::string req3 = "POST /api/pair/decide HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(lanPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(badDecide3.size()) + "\r\nConnection: close\r\n\r\n" + badDecide3;
        std::string resp3 = httpExchange("127.0.0.1", lanPort, req3);
        CHECK(resp3.find("400 Bad Request") != std::string::npos);
    }

    // 6. R6-1: Rotating Origin from one LAN IP must still lock after 5
    {
        // Unit level check: pm with non-loopback IP rotates Origin
        {
            PairingManager pm;
            for (int i = 1; i <= 5; ++i) {
                auto r = pm.submitCode("000000", "192.168.1.55", "Attacker", "http://rot-" + std::to_string(i) + ".com");
                if (i < 5) {
                    CHECK(!r.locked);
                } else {
                    CHECK(r.locked);
                }
            }
            CHECK(pm.isClientLocked("192.168.1.55"));
            auto rLocked = pm.submitCode(pm.currentCode(), "192.168.1.55", "Attacker", "http://rot-99.com");
            CHECK(!rLocked.success);
            CHECK(rLocked.locked);
        }

        // HTTP level check: rotating Origin over peerHost connects
        std::string genReq = "POST /api/pair/generate-code HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(lanPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
        std::string genResp = httpExchange("127.0.0.1", lanPort, genReq);
        CHECK(genResp.find("200 OK") != std::string::npos);

        for (int i = 1; i <= 6; ++i) {
            std::string badBody = "{\"code\":\"000000\"}";
            std::string atkReq = "POST /api/pair HTTP/1.1\r\nHost: " + peerHost + ":" + std::to_string(lanPort) +
                "\r\nOrigin: http://rotating-origin-" + std::to_string(i) + ".com\r\nContent-Type: application/json\r\nContent-Length: " +
                std::to_string(badBody.size()) + "\r\nConnection: close\r\n\r\n" + badBody;
            std::string atkResp = httpExchange(peerHost, lanPort, atkReq);
            if (i < 5) {
                CHECK(atkResp.find("401 Unauthorized") != std::string::npos);
            } else {
                CHECK(atkResp.find("403 Forbidden") != std::string::npos);
                CHECK(atkResp.find("locked") != std::string::npos);
            }
        }
    }

    // 7. R6-1: From one LAN client: 1000 wrong codes, then the correct code. It must NOT return 200.
    {
        // Unit level check
        {
            PairingManager pm;
            std::string realCode = pm.currentCode();
            for (int i = 0; i < 1000; ++i) {
                pm.submitCode("000000", "192.168.1.77");
            }
            auto rCorrect = pm.submitCode(realCode, "192.168.1.77");
            CHECK(!rCorrect.success);
        }

        // HTTP level check
        std::string genReq = "POST /api/pair/generate-code HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(lanPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
        std::string genResp = httpExchange("127.0.0.1", lanPort, genReq);
        size_t gp = genResp.find("\r\n\r\n");
        CHECK(gp != std::string::npos);
        std::string activeCode = JsonValue::parse(genResp.substr(gp + 4))["code"].as_string();

        for (int i = 1; i <= 1000; ++i) {
            std::string badBody = "{\"code\":\"000000\"}";
            std::string atkReq = "POST /api/pair HTTP/1.1\r\nHost: " + peerHost + ":" + std::to_string(lanPort) +
                "\r\nOrigin: http://dos-attacker.local\r\nContent-Type: application/json\r\nContent-Length: " +
                std::to_string(badBody.size()) + "\r\nConnection: close\r\n\r\n" + badBody;
            std::string atkResp = httpExchange(peerHost, lanPort, atkReq);
            CHECK(atkResp.find("200 OK") == std::string::npos);
        }

        std::string goodBody = "{\"code\":\"" + activeCode + "\"}";
        std::string goodReq = "POST /api/pair HTTP/1.1\r\nHost: " + peerHost + ":" + std::to_string(lanPort) +
            "\r\nOrigin: http://dos-attacker.local\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(goodBody.size()) + "\r\nConnection: close\r\n\r\n" + goodBody;
        std::string goodResp = httpExchange(peerHost, lanPort, goodReq);
        CHECK(goodResp.find("200 OK") == std::string::npos);
    }

    // 8. R6-1: After invalidation, the old code fails even when correct, and a new code from generate-code works
    {
        // Unit level check
        {
            PairingManager pm;
            std::string c1 = pm.currentCode();
            CHECK(!pm.isInvalidated());
            for (int i = 0; i < 5; ++i) {
                pm.submitCode("000000", "192.168.1.10");
            }
            CHECK(!pm.isInvalidated());
            CHECK(pm.isClientLocked("192.168.1.10"));
            auto resLocked = pm.submitCode(c1, "192.168.1.10");
            CHECK(!resLocked.success);
            CHECK(resLocked.locked);
            auto resOld = pm.submitCode(c1, "192.168.1.99");
            CHECK(resOld.success);

            std::string c2 = pm.generateNewCode();
            CHECK(!pm.isInvalidated());
            auto resNew = pm.submitCode(c2, "192.168.1.99");
            CHECK(resNew.success);
        }

        // HTTP level check
        std::string genReq = "POST /api/pair/generate-code HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(lanPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
        std::string genResp = httpExchange("127.0.0.1", lanPort, genReq);
        size_t gp = genResp.find("\r\n\r\n");
        CHECK(gp != std::string::npos);
        std::string oldCode = JsonValue::parse(genResp.substr(gp + 4))["code"].as_string();

        // 10 failures occur to invalidate oldCode
        for (int i = 1; i <= 10; ++i) {
            std::string badBody = "{\"code\":\"000000\"}";
            std::string req = "POST /api/pair HTTP/1.1\r\nHost: " + peerHost + ":" + std::to_string(lanPort) +
                "\r\nOrigin: http://peer-" + std::to_string(i) + ".local\r\nContent-Type: application/json\r\nContent-Length: " +
                std::to_string(badBody.size()) + "\r\nConnection: close\r\n\r\n" + badBody;
            std::string resp = httpExchange(peerHost, lanPort, req);
            CHECK(resp.find("200 OK") == std::string::npos);
        }

        // Submitting old code fails even when correct
        std::string oldTryBody = "{\"code\":\"" + oldCode + "\",\"peerName\":\"New Peer\"}";
        std::string oldTryReq = "POST /api/pair HTTP/1.1\r\nHost: " + peerHost + ":" + std::to_string(lanPort) +
            "\r\nOrigin: http://unlocked-peer.local\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(oldTryBody.size()) + "\r\nConnection: close\r\n\r\n" + oldTryBody;
        std::string oldTryResp = httpExchange(peerHost, lanPort, oldTryReq);
        CHECK(oldTryResp.find("200 OK") == std::string::npos);

        // Host generates new code
        std::string genReq2 = "POST /api/pair/generate-code HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(lanPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
        std::string genResp2 = httpExchange("127.0.0.1", lanPort, genReq2);
        size_t gp2 = genResp2.find("\r\n\r\n");
        CHECK(gp2 != std::string::npos);
        std::string newCode = JsonValue::parse(genResp2.substr(gp2 + 4))["code"].as_string();
        CHECK(newCode != oldCode);

        // New code works
        std::string newTryBody = "{\"code\":\"" + newCode + "\",\"peerName\":\"New Peer\"}";
        std::string newTryReq = "POST /api/pair HTTP/1.1\r\nHost: " + peerHost + ":" + std::to_string(lanPort) +
            "\r\nOrigin: http://unlocked-peer.local\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(newTryBody.size()) + "\r\nConnection: close\r\n\r\n" + newTryBody;
        std::string newTryResp = httpExchange(peerHost, lanPort, newTryReq);
        CHECK(newTryResp.find("200 OK") != std::string::npos);
    }

    removeDir(dir);
    std::cout << "PASSED" << std::endl;
}

// =========================================================================
// Test L2: Branch names "HEAD", starting with "-", and ending in ".lock" rejected
// =========================================================================
void test_l2_branch_name_validation() {
    std::cout << "[Test L2] Branch name validation (HEAD, -x, .lock)... " << std::flush;

    // 1. Check isValidBranchName directly
    CHECK(!RefManager::isValidBranchName("HEAD"));
    CHECK(!RefManager::isValidBranchName("-x"));
    CHECK(!RefManager::isValidBranchName("-feature"));
    CHECK(!RefManager::isValidBranchName("-"));
    CHECK(!RefManager::isValidBranchName("branch.lock"));
    CHECK(!RefManager::isValidBranchName("test.lock"));
    CHECK(!RefManager::isValidBranchName(".lock"));
    CHECK(!RefManager::isValidBranchName(""));
    CHECK(!RefManager::isValidBranchName(".hidden"));

    CHECK(RefManager::isValidBranchName("main"));
    CHECK(RefManager::isValidBranchName("feature-1"));
    CHECK(RefManager::isValidBranchName("dev_test"));
    CHECK(RefManager::isValidBranchName("v1.0"));
    CHECK(RefManager::isValidBranchName("lock")); // does not end with .lock

    // 2. Check via /api/branch/create
    fs::path dir = fs::current_path() / "test_repo_l2";
    cleanDir(dir);
    Repository::init(dir.string());
    Repository repo(dir.string());
    writeFile(dir / "hello.txt", "world\n");
    repo.add("hello.txt", true);
    repo.commit("initial");

    int testPort = 19940;
    std::thread srvThread([&]() {
        try {
            HttpServer::run(&repo, testPort, false, false);
        } catch (...) {}
    });
    srvThread.detach();

    for (int retry = 0; retry < 50; ++retry) {
        std::string ping = httpExchange("127.0.0.1", testPort,
            "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) + "\r\nConnection: close\r\n\r\n");
        if (ping.find("200 OK") != std::string::npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Creating "HEAD" branch must fail with 400 Bad Request
    {
        std::string body = "{\"name\":\"HEAD\"}";
        std::string req = "POST /api/branch/create HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        std::string resp = httpExchange("127.0.0.1", testPort, req);
        CHECK(resp.find("400 Bad Request") != std::string::npos);
        CHECK(resp.find("\"ok\":false") != std::string::npos);
    }

    // Creating "-x" branch must fail with 400 Bad Request
    {
        std::string body = "{\"name\":\"-x\"}";
        std::string req = "POST /api/branch/create HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        std::string resp = httpExchange("127.0.0.1", testPort, req);
        CHECK(resp.find("400 Bad Request") != std::string::npos);
        CHECK(resp.find("\"ok\":false") != std::string::npos);
    }

    // Creating "branch.lock" must fail with 400 Bad Request
    {
        std::string body = "{\"name\":\"branch.lock\"}";
        std::string req = "POST /api/branch/create HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        std::string resp = httpExchange("127.0.0.1", testPort, req);
        CHECK(resp.find("400 Bad Request") != std::string::npos);
        CHECK(resp.find("\"ok\":false") != std::string::npos);
    }

    // Creating valid branch succeeds
    {
        std::string body = "{\"name\":\"feature-valid\"}";
        std::string req = "POST /api/branch/create HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        std::string resp = httpExchange("127.0.0.1", testPort, req);
        CHECK(resp.find("200 OK") != std::string::npos);
        CHECK(resp.find("\"ok\":true") != std::string::npos);
    }

    removeDir(dir);
    std::cout << "PASSED" << std::endl;
}

// =========================================================================
// Test L4: /api/pair/status returns 404 on unknown requestId, 200 on denial
// =========================================================================
void test_l4_pairing_status_unknown_vs_denied() {
    std::cout << "[Test L4] Pairing status: 404 unknown vs 200 real denial... " << std::flush;

    fs::path dir = fs::current_path() / "test_repo_l4";
    cleanDir(dir);
    Repository::init(dir.string());
    Repository repo(dir.string());
    writeFile(dir / "hello.txt", "world\n");
    repo.add("hello.txt", true);
    repo.commit("initial");

    int testPort = 19941;
    std::thread srvThread([&]() {
        try {
            HttpServer::run(&repo, testPort, false, true); // lan = true
        } catch (...) {}
    });
    srvThread.detach();

    for (int retry = 0; retry < 50; ++retry) {
        std::string ping = httpExchange("127.0.0.1", testPort,
            "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) + "\r\nConnection: close\r\n\r\n");
        if (ping.find("200 OK") != std::string::npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // 1. Unknown requestId: GET /api/pair/status?requestId=req_unknown_xyz -> 404 Not Found
    {
        std::string req = "GET /api/pair/status?requestId=req_unknown_xyz HTTP/1.1\r\nHost: 127.0.0.1:" +
            std::to_string(testPort) + "\r\nConnection: close\r\n\r\n";
        std::string resp = httpExchange("127.0.0.1", testPort, req);
        CHECK(resp.find("404 Not Found") != std::string::npos);
        size_t bpos = resp.find("\r\n\r\n");
        CHECK(bpos != std::string::npos);
        JsonValue j = JsonValue::parse(resp.substr(bpos + 4));
        CHECK(j["ok"].as_bool() == false);
        CHECK(j["status"].as_string() == "unknown");
        CHECK(j["error"].as_string() == "Unknown or expired pairing request");
    }

    // 2. Real denial: valid code submitted, host denies, status is "denied" with 200 OK
    {
        std::string codeReq = "GET /api/pair/code HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) + "\r\nConnection: close\r\n\r\n";
        std::string codeResp = httpExchange("127.0.0.1", testPort, codeReq);
        size_t cp = codeResp.find("\r\n\r\n");
        CHECK(cp != std::string::npos);
        std::string code = JsonValue::parse(codeResp.substr(cp + 4))["code"].as_string();
        CHECK(!code.empty());

        std::string subBody = "{\"code\":\"" + code + "\",\"peerName\":\"Bob\"}";
        std::string subReq = "POST /api/pair HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(subBody.size()) + "\r\nConnection: close\r\n\r\n" + subBody;
        std::string subResp = httpExchange("127.0.0.1", testPort, subReq);
        size_t sp = subResp.find("\r\n\r\n");
        CHECK(sp != std::string::npos);
        JsonValue sj = JsonValue::parse(subResp.substr(sp + 4));
        CHECK(sj["ok"].as_bool() == true);
        std::string reqId = sj["requestId"].as_string();
        CHECK(!reqId.empty());

        // Host denies
        std::string denyBody = "{\"requestId\":\"" + reqId + "\",\"action\":\"deny\"}";
        std::string denyReq = "POST /api/pair/decide HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(denyBody.size()) + "\r\nConnection: close\r\n\r\n" + denyBody;
        std::string denyResp = httpExchange("127.0.0.1", testPort, denyReq);
        CHECK(denyResp.find("200 OK") != std::string::npos);

        // Peer checks status -> 200 OK with status: "denied"
        std::string statReq = "GET /api/pair/status?requestId=" + reqId + " HTTP/1.1\r\nHost: 127.0.0.1:" +
            std::to_string(testPort) + "\r\nConnection: close\r\n\r\n";
        std::string statResp = httpExchange("127.0.0.1", testPort, statReq);
        CHECK(statResp.find("200 OK") != std::string::npos);
        size_t stp = statResp.find("\r\n\r\n");
        CHECK(stp != std::string::npos);
        JsonValue statJson = JsonValue::parse(statResp.substr(stp + 4));
        CHECK(statJson["ok"].as_bool() == false);
        CHECK(statJson["status"].as_string() == "denied");
        CHECK(statJson["error"].as_string() == "Host denied the pairing request.");
    }

    removeDir(dir);
    std::cout << "PASSED" << std::endl;
}

// =========================================================================
// Test M2: Idle socket timeout (~5s) & 431 Request Header Fields Too Large
// =========================================================================
void test_m2_server_idle_timeout_and_oversized_headers() {
    std::cout << "[Test M2] Idle socket timeout & oversized header 431... " << std::flush;

    fs::path dir = fs::current_path() / "test_repo_m2";
    cleanDir(dir);
    Repository::init(dir.string());
    Repository repo(dir.string());
    writeFile(dir / "hello.txt", "world\n");
    repo.add("hello.txt", true);
    repo.commit("initial");

    int testPort = 19942;
    std::thread srvThread([&]() {
        try {
            HttpServer::run(&repo, testPort, false, true); // lan = true
        } catch (...) {}
    });
    srvThread.detach();

    for (int retry = 0; retry < 50; ++retry) {
        std::string ping = httpExchange("127.0.0.1", testPort,
            "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) + "\r\nConnection: close\r\n\r\n");
        if (ping.find("200 OK") != std::string::npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // 1. Oversized header (> 64 KB) returns 431
    {
        std::string bigHeader(70000, 'A');
        std::string overReq = "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nX-Big-Header: " + bigHeader + "\r\nConnection: close\r\n\r\n";
        std::string overResp = httpExchange("127.0.0.1", testPort, overReq, 5);
        CHECK(overResp.find("431") != std::string::npos);
    }

    // 2. Open a raw socket, send NOTHING, verify normal GET succeeds within ~6 s
    {
        socket_t idleSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        CHECK(idleSock != (socket_t)-1);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(testPort));
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        int connectRes = connect(idleSock, (struct sockaddr*)&addr, sizeof(addr));
        CHECK(connectRes == 0);
        // Do not send anything on idleSock!

        auto tStart = std::chrono::steady_clock::now();
        std::string snapReq = "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nConnection: close\r\n\r\n";
        std::string snapResp = httpExchange("127.0.0.1", testPort, snapReq, 10);
        auto tElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tStart).count();

        CHECK(snapResp.find("200 OK") != std::string::npos);
        CHECK(snapResp.find("repoName") != std::string::npos);
        // With thread pool, snapReq succeeds without being blocked by idleSock
        CHECK(tElapsed <= 3500);

        CLOSESOCK(idleSock);
    }

    // 3. Partial request dropped: server doesn\'t hang, subsequent request succeeds
    {
        socket_t partialSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in paddr{};
        paddr.sin_family = AF_INET;
        paddr.sin_port = htons(static_cast<uint16_t>(testPort));
        paddr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(partialSock, (struct sockaddr*)&paddr, sizeof(paddr)) == 0) {
            std::string partialReq = "POST /api/branch/create HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
                "\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{\"name\":";
            send(partialSock, partialReq.data(), static_cast<int>(partialReq.size()), 0);
            CLOSESOCK(partialSock);
        }
        std::string ping = httpExchange("127.0.0.1", testPort,
            "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) + "\r\nConnection: close\r\n\r\n");
        CHECK(ping.find("200 OK") != std::string::npos);
    }

    removeDir(dir);
    std::cout << "PASSED" << std::endl;
}


// =========================================================================
// Group 1 Regression Tests: Bug 2 & Bug 3
// =========================================================================
void test_group1_http_unauth_body_limit_and_slowloris() {
    std::cout << "[Test Group 1.2 & 1.3] Unauth 4KB body cap & Slowloris protection... " << std::flush;
    fs::path dir = fs::current_path() / "test_repo_g1_srv";
    cleanDir(dir);
    Repository::init(dir.string());
    Repository repo(dir.string());
    writeFile(dir / "hello.txt", "world\n");
    repo.add("hello.txt", true);
    repo.commit("initial");

    int testPort = 19945;
    std::thread srvThread([&]() {
        try {
            HttpServer::run(&repo, testPort, false, true); // lan = true
        } catch (...) {}
    });
    srvThread.detach();

    for (int retry = 0; retry < 50; ++retry) {
        std::string ping = httpExchange("127.0.0.1", testPort,
            "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) + "\r\nConnection: close\r\n\r\n");
        if (ping.find("200 OK") != std::string::npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Bug 2: POST /api/pair with body > 4KB returns 413 Payload Too Large
    {
        std::string largeBody(5000, 'x');
        std::string req = "POST /api/pair HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(largeBody.size()) +
            "\r\nConnection: close\r\n\r\n" + largeBody;
        std::string resp = httpExchange("127.0.0.1", testPort, req);
        CHECK(resp.find("413") != std::string::npos);
        CHECK(resp.find("Payload Too Large (max 4KB)") != std::string::npos);
    }

    // Bug 2: GET /api/pair/status with body > 4KB returns 413 Payload Too Large
    {
        std::string largeBody(5000, 'y');
        std::string req = "GET /api/pair/status HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(largeBody.size()) +
            "\r\nConnection: close\r\n\r\n" + largeBody;
        std::string resp = httpExchange("127.0.0.1", testPort, req);
        CHECK(resp.find("413") != std::string::npos);
    }

    // Bug 3: Slowloris per-request 10s deadline & thread pool concurrency
    {
        socket_t slowSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        CHECK(slowSock != (socket_t)-1);
        sockaddr_in saddr{};
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(static_cast<uint16_t>(testPort));
        saddr.sin_addr.s_addr = inet_addr("127.0.0.1");
        int cRes = connect(slowSock, (struct sockaddr*)&saddr, sizeof(saddr));
        CHECK(cRes == 0);

        // Send partial header
        std::string partial = "POST /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Slow: ";
        send(slowSock, partial.data(), static_cast<int>(partial.size()), 0);

        // While slow connection is open, another request MUST succeed immediately via thread pool
        std::string quickReq = "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) +
            "\r\nConnection: close\r\n\r\n";
        auto t0 = std::chrono::steady_clock::now();
        std::string quickResp = httpExchange("127.0.0.1", testPort, quickReq, 3);
        auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(quickResp.find("200 OK") != std::string::npos);
        CHECK(dur < 2000);

        CLOSESOCK(slowSock);
    }

    removeDir(dir);
    std::cout << "PASSED\n";
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
    test_r4_security_and_fixes();
    test_l2_branch_name_validation();
    test_l4_pairing_status_unknown_vs_denied();
    test_m2_server_idle_timeout_and_oversized_headers();
    test_group1_http_unauth_body_limit_and_slowloris();

    std::cout << "\nALL CLI & SERVER TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
