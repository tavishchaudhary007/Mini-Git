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
    CHECK(threwEmpty);

    bool threwDot = false;
    try { repo.discard(".", true); } catch (const MiniGitException&) { threwDot = true; }
    CHECK(threwDot);

    bool threwDotDot = false;
    try { repo.discard("..", true); } catch (const MiniGitException&) { threwDotDot = true; }
    CHECK(threwDotDot);

    bool threwAbs = false;
    try { repo.discard((dir / "a.txt").string(), true); } catch (const MiniGitException&) { threwAbs = true; }
    CHECK(threwAbs);

    // Repo root and .minigit must still exist and files intact
    CHECK(fs::exists(dir));
    CHECK(fs::exists(dir / ".minigit"));
    CHECK(fs::exists(dir / "a.txt"));
    CHECK(fs::exists(dir / "b.txt"));
    CHECK(readFile(dir / "a.txt") == "hello a");

    // Untracked file discard works and deletes file
    writeFile(dir / "untracked.txt", "remove me");
    CHECK(fs::exists(dir / "untracked.txt"));
    repo.discard("untracked.txt", true);
    CHECK(!fs::exists(dir / "untracked.txt"));
    CHECK(fs::exists(dir / ".minigit"));

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
    CHECK(!revB.empty());
    CHECK(readFile(dir / "f.txt") == "2");
    CHECK(!fs::exists(dir / "g.txt")); // g was added in B, so revert removes it

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
    CHECK(conflictDetected);

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
    CHECK(!fs::exists(dir / "target.txt"));

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
    CHECK(checkoutBlocked);
    CHECK(readFile(dir / "target.txt") == "local untracked");

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
    CHECK(mergeBlocked);
    CHECK(readFile(dir / "target.txt") == "local untracked");

    // 3. Stash pop must be blocked if untracked file would be overwritten
    fs::remove(dir / "target.txt");
    repo.checkout("feature"); // now clean, switch to feature
    writeFile(dir / "stashed_file.txt", "stashed content");
    repo.add("stashed_file.txt", true);
    repo.stashSave("save stash with stashed_file");
    CHECK(!fs::exists(dir / "stashed_file.txt"));

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
    CHECK(stashPopBlocked);
    CHECK(readFile(dir / "stashed_file.txt") == "untracked blocker");

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
    CHECK(refusedBeforeCommit);

    repo.add("f.txt", true);
    repo.commit("initial");

    // 1st stash
    writeFile(dir / "f.txt", "stash 1 content");
    repo.stashSave("Stash One");
    CHECK(readFile(dir / "f.txt") == "draft"); // restored to HEAD

    // 2nd stash
    writeFile(dir / "f.txt", "stash 2 content");
    repo.stashSave("Stash Two");
    CHECK(readFile(dir / "f.txt") == "draft");

    // Check stash stack has 2 entries, newest first
    auto list = repo.stashList();
    CHECK(list.size() == 2);
    CHECK(list[0].message == "Stash Two");
    CHECK(list[1].message == "Stash One");

    // Stash pop on dirty tree must be refused
    writeFile(dir / "f.txt", "dirty uncommitted changes");
    bool popRefusedDirty = false;
    try {
        repo.stashPop();
    } catch (const MiniGitException& e) {
        popRefusedDirty = true;
    }
    CHECK(popRefusedDirty);
    CHECK(readFile(dir / "f.txt") == "dirty uncommitted changes");

    // Discard dirty change, then pop newest (Stash Two)
    repo.discard("f.txt", true);
    std::string popRes1 = repo.stashPop();
    CHECK(readFile(dir / "f.txt") == "stash 2 content");
    CHECK(repo.stashList().size() == 1);
    CHECK(repo.stashList()[0].message == "Stash One");

    // Pop next (Stash One)
    repo.discard("f.txt", true);
    std::string popRes2 = repo.stashPop();
    CHECK(readFile(dir / "f.txt") == "stash 1 content");
    CHECK(repo.stashList().empty());
    CHECK(!repo.hasStash());

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
    CHECK(snapJson.find("<img") == std::string::npos);
    CHECK(snapJson.find("\\u003cimg") != std::string::npos);

    // JsonValue::parse parses it back to literal string
    std::string parseErr;
    JsonValue parsed = JsonValue::parse(snapJson, &parseErr);
    CHECK(parseErr.empty());
    CHECK(parsed.is_object());

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
        CHECK(!fs::exists(dir / "pwned.txt"));
        CHECK(!fs::exists(fs::current_path() / "pwned.txt"));
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
    CHECK(repo.status().staged.size() == 1);
    CHECK(repo.status().staged[0] == "subfolder/file.txt");

    repo.commit("commit subfolder file");
    writeFile(sub / "file.txt", "modified sub content");
    CHECK(repo.status().modified.size() == 1);

    repo.discard("subfolder/file.txt", true);
    CHECK(repo.status().clean());
    CHECK(readFile(sub / "file.txt") == "sub content");

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
        (void)content;
        CHECK(f.find(".git") == std::string::npos);
    }
    for (const auto& f : repo.status().staged) {
        CHECK(f.find(".git") == std::string::npos);
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
    CHECK(entries.size() >= 3);

    // Verify specialMsg roundtripped accurately
    bool foundSpecial = false;
    for (const auto& r : entries) {
        if (r.description == specialMsg) foundSpecial = true;
    }
    CHECK(foundSpecial);

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
    CHECK(err.empty());
    CHECK(parsedReflog.is_object());
    CHECK(parsedReflog["reflog"].as_array().size() == entries.size());

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
    CHECK(err.empty());
    std::string msg = parsed["message"].as_string();
    CHECK(msg.find('\n') != std::string::npos);

    writeFile(dir / "feature.cpp", "code");
    repo.add("feature.cpp", true);
    Hash h = repo.commit(msg);

    // Read back log
    auto commits = repo.log();
    CHECK(!commits.empty());
    CHECK(commits.front().message() == msg);
    CHECK(commits.front().message().find("\n\n* Added PairingManager\n") != std::string::npos);

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test11_merge_conflict_markers_and_abort() {
    std::cout << "[Test 11] Merge conflict markers & abort restore... ";
    fs::path dir = fs::current_path() / "test_repo_11";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "f.txt", "line 1\nline 2 base\nline 3\n");
    repo.add("f.txt", true);
    repo.commit("Base commit");
    repo.createBranch("feat");

    // Edit on main
    writeFile(dir / "f.txt", "line 1\nline 2 ours\nline 3\n");
    repo.add("f.txt", true);
    Hash oursCommit = repo.commit("Ours edit");

    // Edit on feat
    repo.checkout("feat");
    writeFile(dir / "f.txt", "line 1\nline 2 theirs\nline 3\n");
    repo.add("f.txt", true);
    repo.commit("Theirs edit");

    // Switch back to main
    repo.checkout("main");

    bool conflictThrew = false;
    try {
        repo.merge("feat");
    } catch (const MergeException& e) {
        conflictThrew = true;
        std::string err = e.what();
        CHECK(err.find("conflict") != std::string::npos || err.find("Conflict") != std::string::npos);
    }
    CHECK(conflictThrew);

    // Assert conflict markers written
    std::string conflictedContent = readFile(dir / "f.txt");
    CHECK(conflictedContent.find("<<<<<<< ours\n") != std::string::npos);
    CHECK(conflictedContent.find("line 2 ours") != std::string::npos);
    CHECK(conflictedContent.find("=======\n") != std::string::npos);
    CHECK(conflictedContent.find("line 2 theirs") != std::string::npos);
    CHECK(conflictedContent.find(">>>>>>> feat\n") != std::string::npos);

    // Assert nothing committed
    CHECK(repo.head() == oursCommit);
    CHECK(fs::exists(dir / ".minigit" / "MERGE_HEAD"));

    // Merge abort restores file to HEAD
    std::string abortRes = repo.merge("--abort");
    CHECK(abortRes.find("Merge aborted") != std::string::npos);
    CHECK(!fs::exists(dir / ".minigit" / "MERGE_HEAD"));
    CHECK(readFile(dir / "f.txt") == "line 1\nline 2 ours\nline 3\n");
    CHECK(repo.head() == oursCommit);

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test12_commit_hash_preservation() {
    std::cout << "[Test 12] Commit hash without parent2 preservation... ";
    Commit c(Hash("0123456789012345678901234567890123456789"), Hash(), "User <user@test.com>", "Initial commit", 1700000000);
    CHECK(c.hash().str() == "767a84561230db7e4e9cc8c8967cfd6a2aad1035");

    Commit c2(Hash("0123456789012345678901234567890123456789"), Hash("1111111111111111111111111111111111111111"), "User <user@test.com>", "Initial commit", 1700000000);
    CHECK(c2.hash().str() == "216b583c3249da911e56f984e717c242073cbe84");

    std::cout << "PASSED\n";
}

int main() {
    std::cout << "\n============================================\n";
    std::cout << "  Mini-Git Automated Verification\n";
    std::cout << "============================================\n\n";

    auto runTest = [](const char* name, auto fn) {
        std::cout << "Running " << name << "...\n" << std::flush;
        try {
            fn();
        } catch (const std::exception& e) {
            std::cerr << "FAILED in " << name << ": " << e.what() << "\n" << std::flush;
            throw;
        }
    };

    runTest("test1", test1_discard_safety);
    runTest("test2", test2_revert_delta_and_conflict);
    runTest("test3", test3_untracked_overwrite_protection);
    runTest("test4", test4_stash_stack_and_dirty_refusal);
    runTest("test5", test5_xss_prevention);
    runTest("test6", test6_git_export_command_injection);
    runTest("test7", test7_subfolder_path_resolution);
    runTest("test8", test8_import_git_ignores_dotgit);
    runTest("test9", test9_reflog_json_and_roundtrip);
    runTest("test10", test10_multiline_commit_message);
    runTest("test11", test11_merge_conflict_markers_and_abort);
    runTest("test12", test12_commit_hash_preservation);

    std::cout << "\nALL 12 TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
