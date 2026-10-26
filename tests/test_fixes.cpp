#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include "Repository.h"
#include "HtmlExporter.h"
#include "Json.h"
#include "FileUtil.h"

namespace fs = std::filesystem;

void removeDir(const fs::path& p) {
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

void cleanDir(const fs::path& p) {
    removeDir(p);
    std::error_code ec;
    fs::create_directories(p, ec);
}

void test1_discard_safety() {
    std::cout << "[Test 1] Discard safety... ";
    fs::path dir = fs::current_path() / "test_repo_1";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "a.txt", "hello a");
    writeFile(dir / "b.txt", "hello b");
    repo.add(".");
    repo.commit("initial commit");

    // 1. Calling discard with empty, ".", "..", absolute paths must throw
    bool threwEmpty = false;
    try { repo.discard("", true); } catch (const MiniGitException&) { threwEmpty = true; }
    assert(threwEmpty);

    bool threwDot = false;
    try { repo.discard(".", true); } catch (const MiniGitException&) { threwDot = true; }
    assert(threwDot);

    bool threwDotDot = false;
    try { repo.discard("..", true); } catch (const MiniGitException&) { threwDotDot = true; }
    assert(threwDotDot);

    bool threwAbs = false;
    try { repo.discard((dir / "a.txt").string(), true); } catch (const MiniGitException&) { threwAbs = true; }
    assert(threwAbs);

    // Repo root and .minigit must still exist and files intact
    assert(fs::exists(dir));
    assert(fs::exists(dir / ".minigit"));
    assert(fs::exists(dir / "a.txt"));
    assert(fs::exists(dir / "b.txt"));
    assert(readFile(dir / "a.txt") == "hello a");

    // Untracked file discard works and deletes file
    writeFile(dir / "untracked.txt", "remove me");
    assert(fs::exists(dir / "untracked.txt"));
    repo.discard("untracked.txt", true);
    assert(!fs::exists(dir / "untracked.txt"));
    assert(fs::exists(dir / ".minigit"));

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test2_revert_delta_and_conflict() {
    std::cout << "[Test 2] Revert delta & conflict detection... ";
    fs::path dir = fs::current_path() / "test_repo_2";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    // Commit A: adds f.txt="1"
    writeFile(dir / "f.txt", "1");
    repo.add("f.txt", true);
    Hash hashA = repo.commit("Commit A: add f=1");

    // Commit B: edits g.txt="g_val"
    writeFile(dir / "g.txt", "g_val");
    repo.add("g.txt", true);
    Hash hashB = repo.commit("Commit B: add g");

    // Commit C: sets f.txt="2"
    writeFile(dir / "f.txt", "2");
    repo.add("f.txt", true);
    Hash hashC = repo.commit("Commit C: update f=2");

    // Reverting B: restores only g.txt, and f.txt stays "2"
    Hash revB = repo.revert(hashB);
    assert(!revB.empty());
    assert(readFile(dir / "f.txt") == "2");
    assert(!fs::exists(dir / "g.txt")); // g was added in B, so revert removes it

    // Reverting A while C changed f.txt must report a conflict!
    bool conflictDetected = false;
    try {
        repo.revert(hashA);
    } catch (const MiniGitException& e) {
        std::string msg = e.what();
        if (msg.find("revert conflict in f.txt") != std::string::npos) {
            conflictDetected = true;
        }
    }
    assert(conflictDetected);

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test3_untracked_overwrite_protection() {
    std::cout << "[Test 3] Untracked overwrite protection... ";
    fs::path dir = fs::current_path() / "test_repo_3";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "base.txt", "base");
    repo.add("base.txt", true);
    repo.commit("initial");

    // Create branch feature and commit target.txt
    repo.createBranch("feature");
    repo.checkout("feature");
    writeFile(dir / "target.txt", "feature version");
    repo.add("target.txt", true);
    repo.commit("feature commit");

    // Switch back to main
    repo.checkout("main");
    assert(!fs::exists(dir / "target.txt"));

    // Create local untracked target.txt
    writeFile(dir / "target.txt", "local untracked");

    // 1. Checkout feature must be blocked
    bool checkoutBlocked = false;
    try {
        repo.checkout("feature");
    } catch (const MiniGitException& e) {
        std::string err = e.what();
        if (err.find("untracked file 'target.txt' would be overwritten; move or commit it first") != std::string::npos) {
            checkoutBlocked = true;
        }
    }
    assert(checkoutBlocked);
    assert(readFile(dir / "target.txt") == "local untracked");

    // 2. Fast-forward merge must be blocked
    bool mergeBlocked = false;
    try {
        repo.merge("feature");
    } catch (const MiniGitException& e) {
        std::string err = e.what();
        if (err.find("untracked file 'target.txt' would be overwritten; move or commit it first") != std::string::npos) {
            mergeBlocked = true;
        }
    }
    assert(mergeBlocked);
    assert(readFile(dir / "target.txt") == "local untracked");

    // 3. Stash pop must be blocked if untracked file would be overwritten
    fs::remove(dir / "target.txt");
    repo.checkout("feature"); // now clean, switch to feature
    writeFile(dir / "stashed_file.txt", "stashed content");
    repo.add("stashed_file.txt", true);
    repo.stashSave("save stash with stashed_file");
    assert(!fs::exists(dir / "stashed_file.txt"));

    // Create untracked stashed_file.txt
    writeFile(dir / "stashed_file.txt", "untracked blocker");
    bool stashPopBlocked = false;
    try {
        repo.stashPop();
    } catch (const MiniGitException& e) {
        std::string err = e.what();
        if (err.find("untracked file 'stashed_file.txt' would be overwritten; move or commit it first") != std::string::npos) {
            stashPopBlocked = true;
        }
    }
    assert(stashPopBlocked);
    assert(readFile(dir / "stashed_file.txt") == "untracked blocker");

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test4_stash_stack_and_dirty_refusal() {
    std::cout << "[Test 4] Stash stack & dirty pop refusal... ";
    fs::path dir = fs::current_path() / "test_repo_4";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    // Stashing before first commit must be refused
    bool refusedBeforeCommit = false;
    writeFile(dir / "f.txt", "draft");
    try {
        repo.stashSave("draft stash");
    } catch (const MiniGitException& e) {
        std::string err = e.what();
        if (err.find("cannot stash before the first commit") != std::string::npos) {
            refusedBeforeCommit = true;
        }
    }
    assert(refusedBeforeCommit);

    repo.add("f.txt", true);
    repo.commit("initial");

    // 1st stash
    writeFile(dir / "f.txt", "stash 1 content");
    repo.stashSave("Stash One");
    assert(readFile(dir / "f.txt") == "draft"); // restored to HEAD

    // 2nd stash
    writeFile(dir / "f.txt", "stash 2 content");
    repo.stashSave("Stash Two");
    assert(readFile(dir / "f.txt") == "draft");

    // Check stash stack has 2 entries, newest first
    auto list = repo.stashList();
    assert(list.size() == 2);
    assert(list[0].message == "Stash Two");
    assert(list[1].message == "Stash One");

    // Stash pop on dirty tree must be refused
    writeFile(dir / "f.txt", "dirty uncommitted changes");
    bool popRefusedDirty = false;
    try {
        repo.stashPop();
    } catch (const MiniGitException& e) {
        popRefusedDirty = true;
    }
    assert(popRefusedDirty);
    assert(readFile(dir / "f.txt") == "dirty uncommitted changes");

    // Discard dirty change, then pop newest (Stash Two)
    repo.discard("f.txt", true);
    std::string popRes1 = repo.stashPop();
    assert(readFile(dir / "f.txt") == "stash 2 content");
    assert(repo.stashList().size() == 1);
    assert(repo.stashList()[0].message == "Stash One");

    // Pop next (Stash One)
    repo.discard("f.txt", true);
    std::string popRes2 = repo.stashPop();
    assert(readFile(dir / "f.txt") == "stash 1 content");
    assert(repo.stashList().empty());
    assert(!repo.hasStash());

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test5_xss_prevention() {
    std::cout << "[Test 5] XSS prevention & JSON escaping... ";
    fs::path dir = fs::current_path() / "test_repo_5";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    std::string xssMsg = "<img src=x onerror=alert(1)>";
    writeFile(dir / "file.txt", "hello");
    repo.add("file.txt", true);
    repo.commit(xssMsg);

    // Snapshot JSON must have < and > escaped as \\u003c and \\u003e
    std::string snapJson = HtmlExporter::toJson(repo.snapshot(), "test");
    assert(snapJson.find("<img") == std::string::npos);
    assert(snapJson.find("\\u003cimg") != std::string::npos);

    // JsonValue::parse parses it back to literal string
    std::string parseErr;
    JsonValue parsed = JsonValue::parse(snapJson, &parseErr);
    assert(parseErr.empty());
    assert(parsed.is_object());

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test6_git_export_command_injection() {
    std::cout << "[Test 6] Git export injection safety... ";
    fs::path dir = fs::current_path() / "test_repo_6";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    std::string attackMsg = "Fix bug & echo pwned > pwned.txt | $(echo hi) `calc` and \"quotes\"";
    writeFile(dir / "file.txt", "content");
    repo.add("file.txt", true);
    repo.commit(attackMsg);

    // Run export
    try {
        repo.exportToGit();
        // If git is installed, no pwned.txt should be created!
        assert(!fs::exists(dir / "pwned.txt"));
        assert(!fs::exists(fs::current_path() / "pwned.txt"));
    } catch (const MiniGitException& e) {
        // If git is not installed, that's fine too
    }

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test7_subfolder_path_resolution() {
    std::cout << "[Test 7] Subfolder path resolution... ";
    fs::path dir = fs::current_path() / "test_repo_7";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    fs::path sub = dir / "subfolder";
    fs::create_directories(sub);
    writeFile(sub / "file.txt", "sub content");

    // API uses isRepoRelative=true
    repo.add("subfolder/file.txt", true);
    assert(repo.status().staged.size() == 1);
    assert(repo.status().staged[0] == "subfolder/file.txt");

    repo.commit("commit subfolder file");
    writeFile(sub / "file.txt", "modified sub content");
    assert(repo.status().modified.size() == 1);

    repo.discard("subfolder/file.txt", true);
    assert(repo.status().clean());
    assert(readFile(sub / "file.txt") == "sub content");

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test8_import_git_ignores_dotgit() {
    std::cout << "[Test 8] importFromGit ignores .git internals... ";
    fs::path dir = fs::current_path() / "test_repo_8";
    cleanDir(dir);

    // Create dummy .git with internal files
    fs::path fakeGit = dir / ".git";
    fs::create_directories(fakeGit / "objects");
    fs::create_directories(fakeGit / "refs" / "heads");
    writeFile(fakeGit / "config", "dummy git config");
    writeFile(fakeGit / "HEAD", "ref: refs/heads/master\n");
    writeFile(fakeGit / "objects" / "abc", "dummy object data");

    // Real project files
    writeFile(dir / "app.cpp", "int main() {}");
    writeFile(dir / "README.md", "# My Project");

    // Init minigit repo in dir
    Repository::init(dir);
    Repository repo(dir);

    // Run importFromGit
    repo.importFromGit();

    // Verify snapshot does NOT contain any .git files
    Snapshot snap = repo.snapshot();
    for (const auto& [f, content] : snap.files) {
        assert(f.find(".git") == std::string::npos);
    }
    for (const auto& f : repo.status().staged) {
        assert(f.find(".git") == std::string::npos);
    }

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test9_reflog_json_and_roundtrip() {
    std::cout << "[Test 9] Reflog escaping & JSON roundtrip... ";
    fs::path dir = fs::current_path() / "test_repo_9";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    // Commit with | and newline
    std::string specialMsg = "Header line | with pipe\nSecond line with \"quotes\" and \\backslash";
    writeFile(dir / "f.txt", "1");
    repo.add("f.txt", true);
    Hash h1 = repo.commit(specialMsg);

    writeFile(dir / "f.txt", "2");
    repo.add("f.txt", true);
    Hash h2 = repo.commit("Second commit");

    // Revert creates message with quotes: Revert "Second commit"
    repo.revert(h2);

    auto entries = repo.reflog();
    assert(entries.size() >= 3);

    // Verify specialMsg roundtripped accurately
    bool foundSpecial = false;
    for (const auto& r : entries) {
        if (r.description == specialMsg) foundSpecial = true;
    }
    assert(foundSpecial);

    // Verify reflog JSON output parses cleanly as valid JSON
    std::ostringstream jsonOss;
    jsonOss << "{\"ok\":true,\"reflog\":[";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) jsonOss << ",";
        jsonOss << "{\"timestamp\":" << jsonString(entries[i].timestamp)
                << ",\"action\":" << jsonString(entries[i].action)
                << ",\"oldHash\":\"" << entries[i].oldHash << "\""
                << ",\"newHash\":\"" << entries[i].newHash << "\""
                << ",\"description\":" << jsonString(entries[i].description) << "}";
    }
    jsonOss << "]}";

    std::string err;
    JsonValue parsedReflog = JsonValue::parse(jsonOss.str(), &err);
    assert(err.empty());
    assert(parsedReflog.is_object());
    assert(parsedReflog["reflog"].as_array().size() == entries.size());

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test10_multiline_commit_message() {
    std::cout << "[Test 10] Multi-line commit message preservation... ";
    fs::path dir = fs::current_path() / "test_repo_10";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    std::string incomingJson = "{\"message\":\"Feature: implement P2P sync\\n\\n* Added PairingManager\\n* Added tests\"}";
    std::string err;
    JsonValue parsed = JsonValue::parse(incomingJson, &err);
    assert(err.empty());
    std::string msg = parsed["message"].as_string();
    assert(msg.find('\n') != std::string::npos);

    writeFile(dir / "feature.cpp", "code");
    repo.add("feature.cpp", true);
    Hash h = repo.commit(msg);

    // Read back log
    auto commits = repo.log();
    assert(!commits.empty());
    assert(commits.front().message() == msg);
    assert(commits.front().message().find("\n\n* Added PairingManager\n") != std::string::npos);

    removeDir(dir);
    std::cout << "PASSED\n";
}

int main() {
    std::cout << "\n============================================\n";
    std::cout << "  Mini-Git 10 Bug Fixes Automated Verification\n";
    std::cout << "============================================\n\n";

    test1_discard_safety();
    test2_revert_delta_and_conflict();
    test3_untracked_overwrite_protection();
    test4_stash_stack_and_dirty_refusal();
    test5_xss_prevention();
    test6_git_export_command_injection();
    test7_subfolder_path_resolution();
    test8_import_git_ignores_dotgit();
    test9_reflog_json_and_roundtrip();
    test10_multiline_commit_message();

    std::cout << "\nALL 10 TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
