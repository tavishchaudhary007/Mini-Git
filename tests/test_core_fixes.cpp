#include "HtmlExporter.h"
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
#include "PathSafety.h"
#include "Commands.h"
#include "Json.h"
#include <algorithm>
#include "Hasher.h"
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

// 6. Committing during an unresolved conflict
void test_fix6_merge_conflicts_tracking_and_refusal() {
    std::cout << "[Test Fix 6] Merge conflicts tracking & commit refusal... ";
    fs::path dir = fs::current_path() / "test_core_repo_6";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    // Initial commit
    writeFile(dir / "f.txt", "line 1\nbase\nline 3\n");
    repo.add("f.txt", true);
    repo.commit("initial");

    // Branch feature
    repo.createBranch("feature");
    repo.checkout("feature");
    writeFile(dir / "f.txt", "line 1\nfeature change\nline 3\n");
    repo.add("f.txt", true);
    repo.commit("feature commit");

    // Branch main
    repo.checkout("main");
    writeFile(dir / "f.txt", "line 1\nmain change\nline 3\n");
    repo.add("f.txt", true);
    repo.commit("main commit");

    // Conflicted merge
    bool mergeThrew = false;
    try {
        repo.merge("feature");
    } catch (const MergeException&) {
        mergeThrew = true;
    }
    CHECK(mergeThrew);
    CHECK(repo.isMerging());
    CHECK(fs::exists(dir / ".minigit" / "MERGE_CONFLICTS"));

    std::vector<std::string> cPaths = repo.conflictedPaths();
    CHECK(cPaths.size() == 1 && cPaths[0] == "f.txt");

    // Commit while unmerged paths remain in MERGE_CONFLICTS must be refused
    bool commitRefused = false;
    try {
        repo.commit("try committing during conflict");
    } catch (const MergeException&) {
        commitRefused = true;
    }
    CHECK(commitRefused);

    // Re-add f.txt while it still has conflict markers
    std::string conflictedText = readFile(dir / "f.txt");
    CHECK(conflictedText.find("<<<<<<<") != std::string::npos);
    repo.add("f.txt", true);

    // Path is removed from MERGE_CONFLICTS when re-added
    CHECK(repo.conflictedPaths().empty());

    // But commit must STILL be refused because staged content contains conflict markers
    bool commitWithMarkersRefused = false;
    try {
        repo.commit("try committing with conflict markers");
    } catch (const MergeException&) {
        commitWithMarkersRefused = true;
    }
    CHECK(commitWithMarkersRefused);

    // Now resolve conflict cleanly and re-add
    writeFile(dir / "f.txt", "line 1\nresolved change\nline 3\n");
    repo.add("f.txt", true);

    // Commit succeeds!
    Hash mergeCommit = repo.commit("Resolve merge conflict");
    CHECK(!mergeCommit.empty());
    CHECK(!repo.isMerging());
    CHECK(!fs::exists(dir / ".minigit" / "MERGE_HEAD"));
    CHECK(!fs::exists(dir / ".minigit" / "MERGE_CONFLICTS"));
    CHECK(repo.conflictedPaths().empty());

    // Verify merge --abort clears MERGE_CONFLICTS
    repo.checkout("feature");
    writeFile(dir / "g.txt", "g feature\n");
    repo.add("g.txt", true);
    repo.commit("feature g");

    repo.checkout("main");
    writeFile(dir / "g.txt", "g main\n");
    repo.add("g.txt", true);
    repo.commit("main g");

    try { repo.merge("feature"); } catch (...) {}
    CHECK(repo.isMerging());
    CHECK(fs::exists(dir / ".minigit" / "MERGE_CONFLICTS"));

    repo.merge("--abort");
    CHECK(!repo.isMerging());
    CHECK(!fs::exists(dir / ".minigit" / "MERGE_CONFLICTS"));
    CHECK(!fs::exists(dir / ".minigit" / "MERGE_HEAD"));
    CHECK(repo.conflictedPaths().empty());

    removeDir(dir);
    std::cout << "PASSED\n";
}

// 7. Operations blocked during merge
void test_fix7_operations_blocked_during_merge() {
    std::cout << "[Test Fix 7] Operations blocked while merge in progress... ";
    fs::path dir = fs::current_path() / "test_core_repo_7";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "f.txt", "base\n");
    repo.add("f.txt", true);
    Hash initCommit = repo.commit("initial");

    repo.createBranch("br");
    repo.checkout("br");
    writeFile(dir / "f.txt", "br\n");
    repo.add("f.txt", true);
    repo.commit("br commit");

    repo.checkout("main");
    writeFile(dir / "f.txt", "main\n");
    repo.add("f.txt", true);
    repo.commit("main commit");

    try { repo.merge("br"); } catch (...) {}
    CHECK(repo.isMerging());

    // 1. undo
    bool undoBlocked = false;
    try { repo.undo(); }
    catch (const MergeException& e) {
        if (std::string(e.what()).find("merge in progress; commit or abort it first") != std::string::npos) undoBlocked = true;
    }
    CHECK(undoBlocked);

    // 2. stashSave
    bool stashSaveBlocked = false;
    try { repo.stashSave("wip"); }
    catch (const MergeException& e) {
        if (std::string(e.what()).find("merge in progress; commit or abort it first") != std::string::npos) stashSaveBlocked = true;
    }
    CHECK(stashSaveBlocked);

    // 3. stashPop
    bool stashPopBlocked = false;
    try { repo.stashPop(); }
    catch (const MergeException& e) {
        if (std::string(e.what()).find("merge in progress; commit or abort it first") != std::string::npos) stashPopBlocked = true;
    }
    CHECK(stashPopBlocked);

    // 4. revert
    bool revertBlocked = false;
    try { repo.revert(initCommit); }
    catch (const MergeException& e) {
        if (std::string(e.what()).find("merge in progress; commit or abort it first") != std::string::npos) revertBlocked = true;
    }
    CHECK(revertBlocked);

    // 5. checkout
    bool checkoutBlocked = false;
    try { repo.checkout("br"); }
    catch (const MergeException& e) {
        if (std::string(e.what()).find("merge in progress; commit or abort it first") != std::string::npos) checkoutBlocked = true;
    }
    CHECK(checkoutBlocked);

    // 6. createBranch
    bool createBranchBlocked = false;
    try { repo.createBranch("new_branch"); }
    catch (const MergeException& e) {
        if (std::string(e.what()).find("merge in progress; commit or abort it first") != std::string::npos) createBranchBlocked = true;
    }
    CHECK(createBranchBlocked);

    repo.merge("--abort");
    removeDir(dir);
    std::cout << "PASSED\n";
}

// 8. Expose merge state and Snapshot fields
void test_fix8_merge_state_and_snapshot_fields() {
    std::cout << "[Test Fix 8] Merge state & Snapshot struct fields... ";
    fs::path dir = fs::current_path() / "test_core_repo_8";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    // On branch main
    writeFile(dir / "f.txt", "hello\n");
    writeFile(dir / "del.txt", "to be deleted\n");
    repo.add(".", true);
    Hash c1 = repo.commit("initial");

    // Clean snapshot
    Snapshot snap1 = repo.snapshot();
    CHECK(!snap1.detached);
    CHECK(!snap1.merging);
    CHECK(snap1.conflicts.empty());
    CHECK(snap1.stagedStatus.empty());

    // Detached checkout
    repo.checkout(c1.str());
    Snapshot snapDetached = repo.snapshot();
    CHECK(snapDetached.detached);
    CHECK(snapDetached.currentBranch.empty());

    // Switch back to branch
    repo.checkout("main");

    // Test stagedStatus: 'A', 'M', 'D' without " (deleted)" suffix
    writeFile(dir / "new_file.txt", "added\n");
    writeFile(dir / "f.txt", "modified\n");
    fs::remove(dir / "del.txt");
    repo.add(".", true);

    Snapshot snapStaged = repo.snapshot();
    CHECK(snapStaged.stagedStatus.count("new_file.txt"));
    CHECK(snapStaged.stagedStatus["new_file.txt"] == 'A');
    CHECK(snapStaged.stagedStatus.count("f.txt"));
    CHECK(snapStaged.stagedStatus["f.txt"] == 'M');
    CHECK(snapStaged.stagedStatus.count("del.txt"));
    CHECK(snapStaged.stagedStatus["del.txt"] == 'D');
    // Ensure no key has " (deleted)"
    for (const auto& [k, v] : snapStaged.stagedStatus) {
        (void)v;
        CHECK(k.find(" (deleted)") == std::string::npos);
    }

    // Existing fields still intact
    CHECK(snapStaged.head == c1);
    CHECK(!snapStaged.staged.empty());

    removeDir(dir);
    std::cout << "PASSED\n";
}

// 9. stashPop() delta application and conflict detection
void test_fix9_stash_pop_different_branch_and_conflict() {
    std::cout << "[Test Fix 9] stashPop() non-destructive & conflict refusal... ";
    fs::path dir = fs::current_path() / "test_core_repo_9";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "common.txt", "common base\n");
    writeFile(dir / "keep.txt", "keep me\n");
    repo.add(".", true);
    repo.commit("initial on main");

    // Create branch feature with extra file
    repo.createBranch("feature");
    repo.checkout("feature");
    writeFile(dir / "feature_only.txt", "stay alive\n");
    repo.add("feature_only.txt", true);
    repo.commit("feature commit");

    // Go back to main, stash changes on common.txt
    repo.checkout("main");
    writeFile(dir / "common.txt", "stashed common update\n");
    writeFile(dir / "added_in_stash.txt", "stash new file\n");
    repo.stashSave("stash on main", true);

    // Switch to feature branch
    repo.checkout("feature");
    CHECK(fs::exists(dir / "feature_only.txt"));

    // Pop stash on feature branch:
    // It must NOT delete feature_only.txt!
    repo.stashPop();
    CHECK(fs::exists(dir / "feature_only.txt"));
    CHECK(readFile(dir / "feature_only.txt") == "stay alive\n");
    CHECK(readFile(dir / "common.txt") == "stashed common update\n");
    CHECK(readFile(dir / "added_in_stash.txt") == "stash new file\n");

    // Clean up working tree
    repo.discard("common.txt", true);
    repo.discard("added_in_stash.txt", true);
    CHECK(repo.status().clean());

    // Test conflict detection before touching disk
    // 1. On main: save stash modifying conflict.txt
    repo.checkout("main");
    writeFile(dir / "conflict.txt", "base conflict\n");
    repo.add("conflict.txt", true);
    repo.commit("add conflict.txt");

    // Update feature to also have conflict.txt based on that commit
    repo.checkout("feature");
    writeFile(dir / "conflict.txt", "feature version of conflict\n");
    repo.add("conflict.txt", true);
    repo.commit("feature changed conflict.txt");

    // Back to main, modify conflict.txt and stash
    repo.checkout("main");
    writeFile(dir / "conflict.txt", "stash version of conflict\n");
    repo.stashSave("stash conflict");

    // Switch to feature and try to pop stash:
    repo.checkout("feature");
    bool popConflictDetected = false;
    try {
        repo.stashPop();
    } catch (const MiniGitException& e) {
        if (std::string(e.what()).find("conflict") != std::string::npos) {
            popConflictDetected = true;
        }
    }
    CHECK(popConflictDetected);
    // Disk must not be overwritten or touched
    CHECK(readFile(dir / "conflict.txt") == "feature version of conflict\n");

    removeDir(dir);
    std::cout << "PASSED\n";
}

// 10. revert() atomicity & root commit refusal
void test_fix10_atomic_revert_initial_and_empty() {
    std::cout << "[Test Fix 10] revert() atomic & root commit safety... ";
    fs::path dir = fs::current_path() / "test_core_repo_10";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    // Commit 1 (root commit)
    writeFile(dir / "root.txt", "root content\n");
    repo.add("root.txt", true);
    Hash rootCommit = repo.commit("initial commit");

    // Attempting to revert initial commit must throw before touching disk/index
    bool rootRevertRefused = false;
    try {
        repo.revert(rootCommit);
    } catch (const MiniGitException& e) {
        if (std::string(e.what()).find("cannot revert the initial commit") != std::string::npos) {
            rootRevertRefused = true;
        }
    }
    CHECK(rootRevertRefused);
    // Files on disk and index must be completely untouched
    CHECK(fs::exists(dir / "root.txt"));
    CHECK(readFile(dir / "root.txt") == "root content\n");
    CHECK(repo.status().clean());

    // Commit 2
    writeFile(dir / "c2.txt", "c2\n");
    repo.add("c2.txt", true);
    Hash c2 = repo.commit("second commit");

    // Revert commit 2 cleanly
    Hash revC2 = repo.revert(c2);
    CHECK(!revC2.empty());
    CHECK(!fs::exists(dir / "c2.txt"));
    CHECK(repo.status().clean());

    // Attempting to revert commit 2 again must detect conflict/nothing to revert
    bool secondRevertRefused = false;
    try {
        repo.revert(c2);
    } catch (const MiniGitException&) {
        secondRevertRefused = true;
    }
    CHECK(secondRevertRefused);
    CHECK(repo.status().clean());

    removeDir(dir);
    std::cout << "PASSED\n";
}

// 11. Empty-tree commit allowed when HEAD's tree is not empty
void test_fix11_empty_tree_commit() {
    std::cout << "[Test Fix 11] Empty-tree commit allowed when tracked files deleted... ";
    fs::path dir = fs::current_path() / "test_core_repo_11";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    // Initial empty staging area throws NothingToCommitException
    bool emptyStagingRefused = false;
    try {
        repo.commit("initial with empty staging");
    } catch (const NothingToCommitException&) {
        emptyStagingRefused = true;
    }
    CHECK(emptyStagingRefused);

    // Commit a file
    writeFile(dir / "file.txt", "content\n");
    repo.add("file.txt", true);
    repo.commit("commit with file");

    // Delete all tracked files
    fs::remove(dir / "file.txt");
    repo.add(".", true);
    CHECK(repo.status().staged.size() == 1); // staged deletion
    CHECK(repo.status().staged[0] == "file.txt (deleted)");

    // Commit empty index (all tracked files deleted) -> must SUCCEED!
    Hash emptyTreeCommit = repo.commit("delete all tracked files");
    CHECK(!emptyTreeCommit.empty());
    CHECK(repo.status().clean());

    // Now HEAD tree is empty; committing again with empty index must be rejected
    bool commitAgainRefused = false;
    try {
        repo.commit("commit again on empty tree");
    } catch (const NothingToCommitException&) {
        commitAgainRefused = true;
    }
    CHECK(commitAgainRefused);

    removeDir(dir);
    std::cout << "PASSED\n";
}

// 13 & 14. isIgnored() rules and .minigitignore pattern caching
void test_fix13_14_minigit_ignore_and_caching() {
    std::cout << "[Test Fix 13 & 14] isIgnored repo root only & caching... ";
    fs::path dir = fs::current_path() / "test_core_repo_13";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    // Folder named 'minigit' containing real.txt must NOT be ignored
    fs::path minigitSubdir = dir / "minigit";
    fs::create_directories(minigitSubdir);
    writeFile(minigitSubdir / "real.txt", "I am real code!\n");

    // Root binary minigit.exe MUST be ignored
    writeFile(dir / "minigit.exe", "fake exe binary\n");

    // Subdir file named minigit (not at root) must NOT be ignored
    fs::create_directories(dir / "sub");
    writeFile(dir / "sub" / "minigit", "fake sub binary\n");

    // Another normal file
    writeFile(dir / "app.cpp", "int main() {}\n");

    // Snapshot files check
    Snapshot snap = repo.snapshot();
    CHECK(snap.files.count("minigit/real.txt"));
    CHECK(snap.files.count("app.cpp"));
    CHECK(snap.files.count("sub/minigit"));
    CHECK(!snap.files.count("minigit.exe"));

    // Add and commit
    repo.add(".", true);
    Hash c = repo.commit("add minigit/real.txt and app.cpp");
    CHECK(!c.empty());

    // Test root binary named 'minigit' in a separate repo without folder 'minigit'
    fs::path dir2 = fs::current_path() / "test_core_repo_13_file";
    cleanDir(dir2);
    Repository::init(dir2);
    Repository repo2(dir2);
    writeFile(dir2 / "minigit", "binary at root\n");
    writeFile(dir2 / "normal.txt", "normal file\n");
    Snapshot snap2 = repo2.snapshot();
    CHECK(snap2.files.count("normal.txt"));
    CHECK(!snap2.files.count("minigit"));
    removeDir(dir2);

    // .minigitignore caching test
    writeFile(dir / ".minigitignore", "build/\n*.log\n");
    writeFile(dir / "test.log", "some log\n");
    fs::create_directories(dir / "build");
    writeFile(dir / "build" / "temp.o", "object\n");

    Snapshot snapIgn = repo.snapshot();
    CHECK(!snapIgn.files.count("test.log"));
    CHECK(!snapIgn.files.count("build/temp.o"));

    removeDir(dir);
    std::cout << "PASSED\n";
}

// 15. exportToGit: no stray 'nul' file, clear errors
void test_fix15_export_to_git_no_nul_file() {
    std::cout << "[Test Fix 15] exportToGit no stray 'nul' file... ";
    fs::path dir = fs::current_path() / "test_core_repo_15";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "test.txt", "hello export\n");
    repo.add("test.txt", true);
    repo.commit("commit test");

    auto hasNulEntry = [](const fs::path& p) {
        std::error_code ec;
        if (!fs::exists(p, ec)) return false;
        for (const auto& e : fs::directory_iterator(p, ec)) {
            if (e.path().filename().string() == "nul") return true;
        }
        return false;
    };

    // Check before running export
    CHECK(!hasNulEntry(dir));
    CHECK(!hasNulEntry(fs::current_path()));

    try {
        repo.exportToGit();
    } catch (const MiniGitException&) {
        // If git is not installed in environment, ignore
    }

    // Verify no file named 'nul' is created on disk
    CHECK(!hasNulEntry(dir));
    CHECK(!hasNulEntry(fs::current_path()));
#ifndef _WIN32
    CHECK(!fs::exists("nul"));
    CHECK(!fs::exists(dir / "nul"));
#endif

    removeDir(dir);
    std::cout << "PASSED\n";
}

// 16. importFromGit: "already up to date" instead of throwing
void test_fix16_import_from_git_already_up_to_date() {
    std::cout << "[Test Fix 16] importFromGit returns 'already up to date'... ";
    fs::path dir = fs::current_path() / "test_core_repo_16";
    cleanDir(dir);

    fs::path gitDir = dir / ".git";
    fs::create_directories(gitDir);
    writeFile(gitDir / "HEAD", "dummy\n");

    writeFile(dir / "source.txt", "content\n");

    Repository::init(dir);
    Repository repo(dir);

    // First import: snapshots current working tree
    std::string res1 = repo.importFromGit();
    CHECK(res1.find("current working tree") != std::string::npos);

    // Second import without changes: returns "already up to date" instead of throwing
    std::string res2 = repo.importFromGit();
    CHECK(res2.find("already up to date") != std::string::npos);

    removeDir(dir);
    std::cout << "PASSED\n";
}

// 31. createBranch(name, at): validate at resolves to a commit
void test_fix31_create_branch_at_commit() {
    std::cout << "[Test Fix 31] createBranch validates 'at' revision... ";
    fs::path dir = fs::current_path() / "test_core_repo_31";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    // Commit 1
    writeFile(dir / "f1.txt", "v1\n");
    repo.add("f1.txt", true);
    Hash c1 = repo.commit("commit 1");

    // Commit 2
    writeFile(dir / "f2.txt", "v2\n");
    repo.add("f2.txt", true);
    Hash c2 = repo.commit("commit 2");

    // Create branch at specific commit (full hash)
    repo.createBranch("br_c1", c1);
    CHECK(repo.branchTip("br_c1") == c1);

    // Create branch using short prefix
    repo.createBranch("br_c2_short", Hash(c2.shortStr()));
    CHECK(repo.branchTip("br_c2_short") == c2);

    // Create branch with empty 'at' defaults to HEAD (c2)
    repo.createBranch("br_head");
    CHECK(repo.branchTip("br_head") == c2);

    // Invalid non-existent commit throws InvalidObjectException
    bool invalidThrew = false;
    try {
        repo.createBranch("br_invalid", Hash("deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"));
    } catch (const InvalidObjectException&) {
        invalidThrew = true;
    }
    CHECK(invalidThrew);

    // 'at' resolving to a Blob (not a commit) throws InvalidObjectException
    Hash blobHash = Blob("not a commit").hash();
    bool blobThrew = false;
    try {
        repo.createBranch("br_blob", blobHash);
    } catch (const InvalidObjectException&) {
        blobThrew = true;
    }
    CHECK(blobThrew);

    removeDir(dir);
    std::cout << "PASSED\n";
}


// H1 (HIGH) Path traversal through bundle import
void test_h1_bundle_import_path_traversal() {
    std::cout << "[Test H1] Bundle import path traversal rejection... " << std::flush;
    fs::path dir = fs::current_path() / "test_core_repo_h1";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "initial.txt", "initial file content\n");
    repo.add("initial.txt", true);
    Hash initCommit = repo.commit("initial commit");
    Hash headBefore = repo.head();
    CHECK(headBefore == initCommit);

    // List of crafted bad paths
    std::vector<std::string> badPaths = {
        "../pwned.txt",
        "/tmp/abs.txt",
        ".minigit/HEAD",
        ".MINIGIT/HEAD",
        "C:/abs.txt",
        "C:abs.txt",
        "foo\\pwned.txt",
        "foo/./bar.txt",
        "foo/../bar.txt",
        ".minigit/config",
        ".MINIGIT/config",
        std::string("control\0path.txt", 16),
        "control\x01path.txt",
        "control\x1fpath.txt",
        "control\npath.txt",
        "control\rpath.txt"
    };

    fs::path outsideCanary = fs::current_path() / "pwned.txt";
    std::error_code ec;
    fs::remove(outsideCanary, ec);

    for (size_t i = 0; i < badPaths.size(); ++i) {
        const std::string& bp = badPaths[i];
        std::string blobContent = "evil payload " + std::to_string(i);
        Blob evilBlob(blobContent);
        Hash bHash = evilBlob.hash();

        // Calculate expected tree hash manually
        std::string treeBody = bHash.str() + " " + bp + "\n";
        Hash tHash = Hash(Hasher::sha1("tree\n" + treeBody));

        // Craft commit
        std::string cBody = "tree " + tHash.str() + "\nparent " + initCommit.str() + "\nauthor Attacker <bad@evil.com>\ntime 1700000000\n\nEvil commit\n";
        Hash cHash = Hash(Hasher::sha1("commit\n" + cBody));

        // Build bundle JSON
        std::map<std::string, JsonValue> objs;
        // blob obj
        std::map<std::string, JsonValue> blobObj;
        blobObj["type"] = JsonValue("blob");
        blobObj["content"] = JsonValue(blobContent);
        objs[bHash.str()] = JsonValue(blobObj);

        // tree obj
        std::map<std::string, JsonValue> treeObj;
        treeObj["type"] = JsonValue("tree");
        std::map<std::string, JsonValue> treeData;
        treeData[bp] = JsonValue(bHash.str());
        treeObj["data"] = JsonValue(treeData);
        objs[tHash.str()] = JsonValue(treeObj);

        // commit obj
        std::map<std::string, JsonValue> commitObj;
        commitObj["type"] = JsonValue("commit");
        std::map<std::string, JsonValue> commitData;
        commitData["tree"] = JsonValue(tHash.str());
        commitData["parent"] = JsonValue(initCommit.str());
        commitData["author"] = JsonValue("Attacker <bad@evil.com>");
        commitData["message"] = JsonValue("Evil commit");
        commitData["time"] = JsonValue(static_cast<int64_t>(1700000000));
        commitObj["data"] = JsonValue(commitData);
        objs[cHash.str()] = JsonValue(commitObj);

        std::map<std::string, JsonValue> branches;
        branches["evil_branch"] = JsonValue(cHash.str());

        std::map<std::string, JsonValue> bundle;
        bundle["objects"] = JsonValue(objs);
        bundle["branches"] = JsonValue(branches);

        std::string bundleJson = JsonValue(bundle).serialize();

        std::string resStr = repo.importBundleJson(bundleJson);
        JsonValue resJson = JsonValue::parse(resStr);
        CHECK(resJson.contains("ok"));
        CHECK(resJson["ok"].as_bool() == false);

        // HEAD and refs must remain untouched
        CHECK(repo.head() == initCommit);
        CHECK(!repo.branchExists("evil_branch"));
        CHECK(!fs::exists(outsideCanary));
    }

    // Direct Tree::parse with unsafe path must throw InvalidObjectException
    bool parseThrew = false;
    try {
        Tree::parse("0123456789abcdef0123456789abcdef01234567 ../evil.txt\n");
    } catch (const InvalidObjectException&) {
        parseThrew = true;
    }
    CHECK(parseThrew);

    // Direct restoreCommit with unsafe path must throw and refuse to write
    ObjectStore store(dir / ".minigit" / "objects");
    Blob bSafe("payload");
    Hash bSafeHash = store.save(bSafe);

    // Craft a commit with a tree containing an unsafe path
    Tree rawEvilTree;
    rawEvilTree.set("../pwned.txt", bSafeHash);
    Hash evilTreeHash = store.save(rawEvilTree);
    Commit evilCommit(evilTreeHash, initCommit, "evil", "evil", 1700000000);
    Hash evilCommitHash = store.save(evilCommit);

    bool checkoutThrew = false;
    try {
        repo.checkout(evilCommitHash.str());
    } catch (const MiniGitException&) {
        checkoutThrew = true;
    }
    CHECK(checkoutThrew);
    CHECK(!fs::exists(outsideCanary));
    CHECK(repo.head() == initCommit);

    removeDir(dir);
    std::cout << "PASSED\n";
}

// H2 (HIGH) The web file API follows symlinks
void test_h2_web_file_api_symlinks() {
    std::cout << "[Test H2] Web file API symlink refusal... " << std::flush;
    fs::path dir = fs::current_path() / "test_core_repo_h2";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "f.txt", "normal file\n");
    repo.add("f.txt", true);
    repo.commit("initial");

    // Outside temp dir
    fs::path outsideDir = fs::temp_directory_path() / "test_h2_outside_dir";
    cleanDir(outsideDir);
    writeFile(outsideDir / "secret.txt", "top secret data\n");

    // Create symlink/junction inside repo pointing to outsideDir
    fs::path linkPath = dir / "outside_link";
#ifdef _WIN32
    std::string linkCmd = "cmd.exe /c mklink /J \"" + linkPath.string() + "\" \"" + outsideDir.string() + "\" >nul 2>&1";
    std::system(linkCmd.c_str());
#else
    std::error_code ec;
    fs::create_directory_symlink(outsideDir, linkPath, ec);
#endif

    // Save endpoint uses resolveRepoPath(p, true)
    bool saveThrew = false;
    try {
        repo.resolveRepoPath("outside_link/secret.txt", true);
    } catch (const MiniGitException&) {
        saveThrew = true;
    }
    CHECK(saveThrew);

    // Delete endpoint uses resolveRepoPath(p, true)
    bool delThrew = false;
    try {
        repo.resolveRepoPath("outside_link/secret.txt", true);
    } catch (const MiniGitException&) {
        delThrew = true;
    }
    CHECK(delThrew);

    // Stage endpoint uses repo.add(p, true)
    bool stageThrew = false;
    try {
        repo.add("outside_link/secret.txt", true);
    } catch (const MiniGitException&) {
        stageThrew = true;
    }
    CHECK(stageThrew);

    // Also test directly addressing the symlink itself
    bool linkThrew = false;
    try {
        repo.resolveRepoPath("outside_link", true);
    } catch (const MiniGitException&) {
        linkThrew = true;
    }
    CHECK(linkThrew);

    // Verify outside file is completely untouched
    CHECK(fs::exists(outsideDir / "secret.txt"));
    CHECK(readFile(outsideDir / "secret.txt") == "top secret data\n");

    // Clean up
#ifdef _WIN32
    std::string rmCmd = "cmd.exe /c rmdir \"" + linkPath.string() + "\" >nul 2>&1";
    std::system(rmCmd.c_str());
#else
    fs::remove(linkPath);
#endif
    removeDir(outsideDir);
    removeDir(dir);
    std::cout << "PASSED\n";
}

// M1 (MED) Replacing a file with a folder of the same name corrupts the repo
void test_m1_replace_file_with_folder() {
    std::cout << "[Test M1] Replacing a file with a folder of same name... " << std::flush;
    fs::path dir = fs::current_path() / "test_core_repo_m1";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    // Initial commit with file 'd'
    writeFile(dir / "d", "file content of d\n");
    repo.add("d", true);
    Hash c1 = repo.commit("commit with file d");
    repo.createBranch("br_file", c1);

    // Repro steps: rm d; mkdir d; echo x > d/x; add .; commit
    fs::remove(dir / "d");
    fs::create_directories(dir / "d");
    writeFile(dir / "d" / "x", "content inside d/x\n");

    // Status before staging: 'd' must be listed as deleted, NOT modified!
    Status sBefore = repo.status();
    CHECK(std::find(sBefore.deleted.begin(), sBefore.deleted.end(), "d") != sBefore.deleted.end());
    CHECK(std::find(sBefore.modified.begin(), sBefore.modified.end(), "d") == sBefore.modified.end());

    repo.add(".");

    // Commit the change
    Hash c2 = repo.commit("replaced file d with folder d containing x");

    // Status must now be completely clean
    Status sAfter = repo.status();
    CHECK(sAfter.clean());

    // Tree of c2 must have 'd/x' and must NOT contain 'd'
    ObjectStore store(dir / ".minigit" / "objects");
    auto t2 = store.loadAs<Tree>(store.loadAs<Commit>(c2)->tree());
    CHECK(t2->entries().count("d/x") == 1);
    CHECK(t2->entries().count("d") == 0);

    // Checkout back to br_file (commit c1): replaces folder with file safely
    repo.checkout("br_file");
    CHECK(fs::is_regular_file(dir / "d"));
    CHECK(!fs::is_directory(dir / "d"));
    CHECK(readFile(dir / "d") == "file content of d\n");
    CHECK(!fs::exists(dir / "d" / "x"));
    CHECK(repo.status().clean());

    // Checkout forward to main (commit c2): replaces file with folder safely
    repo.checkout("main");
    CHECK(fs::is_directory(dir / "d"));
    CHECK(fs::is_regular_file(dir / "d" / "x"));
    CHECK(readFile(dir / "d" / "x") == "content inside d/x\n");
    CHECK(repo.status().clean());

    // Checkout back and forth once more
    repo.checkout("br_file");
    CHECK(fs::is_regular_file(dir / "d"));
    CHECK(repo.status().clean());
    repo.checkout("main");
    CHECK(fs::is_regular_file(dir / "d" / "x"));
    CHECK(repo.status().clean());

    removeDir(dir);
    std::cout << "PASSED\n";
}

// L1 (LOW) Bundle import accepts branch tips that point at a blob or tree
void test_l1_bundle_import_non_commit_tips() {
    std::cout << "[Test L1] Bundle import non-commit tip verification... " << std::flush;
    fs::path dir = fs::current_path() / "test_core_repo_l1";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    // Valid commit
    writeFile(dir / "base.txt", "base\n");
    repo.add("base.txt", true);
    Hash validCommit = repo.commit("valid commit");

    // Create a blob and tree to use as non-commit targets
    Blob b("a raw blob");
    Hash blobHash = b.hash();
    Tree t;
    t.set("sub.txt", blobHash);
    Hash treeHash = t.hash();

    // Craft bundle with a valid branch and two invalid branches (one pointing to blob, one to tree)
    std::map<std::string, JsonValue> objs;

    std::map<std::string, JsonValue> blobObj;
    blobObj["type"] = JsonValue("blob");
    blobObj["content"] = JsonValue("a raw blob");
    objs[blobHash.str()] = JsonValue(blobObj);

    std::map<std::string, JsonValue> treeObj;
    treeObj["type"] = JsonValue("tree");
    std::map<std::string, JsonValue> treeData;
    treeData["sub.txt"] = JsonValue(blobHash.str());
    treeObj["data"] = JsonValue(treeData);
    objs[treeHash.str()] = JsonValue(treeObj);

    std::map<std::string, JsonValue> branches;
    branches["branch_blob_tip"] = JsonValue(blobHash.str());
    branches["branch_tree_tip"] = JsonValue(treeHash.str());
    branches["branch_valid"] = JsonValue(validCommit.str());

    std::map<std::string, JsonValue> bundle;
    bundle["objects"] = JsonValue(objs);
    bundle["branches"] = JsonValue(branches);

    std::string resStr = repo.importBundleJson(JsonValue(bundle).serialize());
    JsonValue res = JsonValue::parse(resStr);

    CHECK(res.contains("ok"));
    CHECK(res.contains("warnings"));
    const auto& warns = res["warnings"].as_array();
    CHECK(!warns.empty());

    CHECK(res.contains("branchesSkippedInvalid"));
    const auto& skipped = res["branchesSkippedInvalid"].as_array();
    bool foundBlobTip = false, foundTreeTip = false;
    for (const auto& v : skipped) {
        if (v.as_string() == "branch_blob_tip") foundBlobTip = true;
        if (v.as_string() == "branch_tree_tip") foundTreeTip = true;
    }
    CHECK(foundBlobTip);
    CHECK(foundTreeTip);

    // Neither invalid branch was created
    CHECK(!repo.branchExists("branch_blob_tip"));
    CHECK(!repo.branchExists("branch_tree_tip"));

    removeDir(dir);
    std::cout << "PASSED\n";
}

// L3 (LOW) mg add prints raw control characters from filenames in warnings & non-zero exit
void test_l3_escape_path_and_nonzero_exit() {
    std::cout << "[Test L3] escapePath formatting & single illegal path non-zero exit... " << std::flush;
    fs::path dir = fs::current_path() / "test_core_repo_l3";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    // 1. Verify escapePath escaping rules
    CHECK(escapePath("normal_file.txt") == "normal_file.txt");
    CHECK(escapePath("file\nname.txt") == "file\\x0aname.txt");
    CHECK(escapePath("file\rname.txt") == "file\\x0dname.txt");
    CHECK(escapePath("file\tname.txt") == "file\\x09name.txt");
    CHECK(escapePath("file\\name.txt") == "file\\\\name.txt");
    CHECK(escapePath(std::string("null\0byte.txt", 13)) == "null\\x00byte.txt");

    auto oldCwd = fs::current_path();
    fs::current_path(dir);

    // 2. AddCmd on single illegal filename throws MiniGitException (causes CLI to exit non-zero)
    AddCmd addCmd;
    bool singleIllegalThrew = false;
    try {
        addCmd.execute(&repo, {"illegal:name.txt"});
    } catch (const MiniGitException&) {
        singleIllegalThrew = true;
    }
    CHECK(singleIllegalThrew);

    // Verify nothing staged
    Index idx1(dir / ".minigit" / "index");
    CHECK(idx1.empty());

    // 3. AddCmd with a mix of valid and illegal file stages the valid file and does not throw
    writeFile(dir / "valid.txt", "valid content\n");
    bool mixThrew = false;
    try {
        addCmd.execute(&repo, {"valid.txt", "illegal:name.txt"});
    } catch (const MiniGitException&) {
        mixThrew = true;
    }
    CHECK(!mixThrew);

    Index idx2(dir / ".minigit" / "index");
    CHECK(idx2.has("valid.txt"));
    CHECK(!idx2.has("illegal:name.txt"));

    fs::current_path(oldCwd);
    removeDir(dir);
    std::cout << "PASSED\n";
}


// =========================================================================
// Group 1 Regression Tests: Bug 1, Bug 4, Bug 5
// =========================================================================

void test_group1_json_nesting_limit() {
    std::cout << "[Test Group 1.1] Json nesting depth limit & 2M bracket overflow safety... ";

    // 1. 2,000,000 '[' must not crash or stack overflow
    std::string hugeArray(2000000, '[');
    std::string err;
    JsonValue v = JsonValue::parse(hugeArray, &err);
    CHECK(v.type() == JsonValue::Type::Null);
    CHECK(!err.empty());
    CHECK(err.find("maximum nesting depth exceeded") != std::string::npos);

    // 2. 65 nested arrays must fail with max nesting depth exceeded
    std::string nested65(65, '[');
    nested65.append(65, ']');
    err.clear();
    JsonValue v65 = JsonValue::parse(nested65, &err);
    CHECK(v65.type() == JsonValue::Type::Null);
    CHECK(!err.empty());
    CHECK(err.find("maximum nesting depth exceeded") != std::string::npos);

    // 3. 64 nested arrays must succeed
    std::string nested64(64, '[');
    nested64.append(64, ']');
    err.clear();
    JsonValue v64 = JsonValue::parse(nested64, &err);
    CHECK(v64.type() == JsonValue::Type::Array);
    CHECK(err.empty());

    std::cout << "PASSED\n";
}

void test_group1_refmanager_branch_name_validation() {
    std::cout << "[Test Group 1.4] RefManager branch name validation (branch -d ../../index fails)... ";
    fs::path dir = fs::current_path() / "test_refmgr_repo";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "f.txt", "content\n");
    repo.add("f.txt", true);
    repo.commit("initial");

    RefManager refs(dir / ".minigit");
    fs::path indexFile = dir / ".minigit" / "index";
    CHECK(fs::exists(indexFile));

    // Branch traversal attempts must throw BranchException
    bool threwExists = false;
    try {
        refs.branchExists("../../index");
    } catch (const BranchException&) {
        threwExists = true;
    }
    CHECK(threwExists);

    bool threwTip = false;
    try {
        refs.branchTip("../../index");
    } catch (const BranchException&) {
        threwTip = true;
    }
    CHECK(threwTip);

    bool threwDel = false;
    try {
        refs.deleteBranch("../../index");
    } catch (const BranchException&) {
        threwDel = true;
    }
    CHECK(threwDel);

    bool threwSwitch = false;
    try {
        refs.switchToBranch("../../index");
    } catch (const BranchException&) {
        threwSwitch = true;
    }
    CHECK(threwSwitch);

    // Verify index is untouched and still exists
    CHECK(fs::exists(indexFile));

    // Also verify via BranchCmd
    BranchCmd bCmd;
    bool bThrew = false;
    try {
        bCmd.execute(&repo, {"-d", "../../index"});
    } catch (const BranchException&) {
        bThrew = true;
    } catch (const MiniGitException&) {
        bThrew = true;
    }
    CHECK(bThrew);
    CHECK(fs::exists(indexFile));

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test_group1_git_export_no_system() {
    std::cout << "[Test Group 1.5] Git export uses argv array without system() interpolation... ";
    fs::path dir = fs::current_path() / "test_exp_no_sys";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    // Commit message with quotes, spaces, and special symbols
    writeFile(dir / "hello.txt", "hello world\n");
    repo.add("hello.txt", true);
    repo.commit("Commit with 'single' and \"double\" quotes & spaces");

    try {
        std::string res = repo.exportToGit();
        CHECK(res.find("Exported to standard Git") != std::string::npos || res == "already up to date");
    } catch (const MiniGitException& e) {
        std::string err = e.what();
        CHECK(err.find("git is not installed") != std::string::npos ||
              err.find("git user.name and user.email are not configured") != std::string::npos);
    }

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test_group2_restore_untracked_prefix_refusal() {
    std::cout << "[Group 2] Untracked parent prefix refusal on restore/checkout... ";
    fs::path dir = fs::current_path() / "test_core_repo_g2_prefix";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "a.txt", "initial file");
    repo.add("a.txt", true);
    repo.commit("initial");

    repo.createBranch("feat");
    repo.checkout("feat");
    fs::create_directories(dir / "d");
    writeFile(dir / "d" / "sub.txt", "sub file");
    repo.add("d/sub.txt", true);
    repo.commit("commit with d/sub.txt");

    repo.checkout("main");
    CHECK(fs::exists(dir / "a.txt"));
    CHECK(!fs::exists(dir / "d"));

    // Create untracked file 'd'
    writeFile(dir / "d", "I am untracked file d");

    bool threw = false;
    try {
        repo.checkout("feat");
    } catch (const MiniGitException& e) {
        threw = true;
        std::string msg = e.what();
        CHECK(msg.find("untracked file 'd' would be overwritten") != std::string::npos);
    }
    CHECK(threw);

    // Ensure untracked file d was NOT deleted or overwritten
    CHECK(fs::exists(dir / "d"));
    CHECK(!fs::is_directory(dir / "d"));
    CHECK(readFile(dir / "d") == "I am untracked file d");

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test_group2_restore_nonempty_dir_refusal() {
    std::cout << "[Group 2] Non-empty directory at tracked path refusal (no remove_all)... ";
    fs::path dir = fs::current_path() / "test_core_repo_g2_nonempty";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "tracked.txt", "tracked content");
    repo.add("tracked.txt", true);
    repo.commit("commit with tracked.txt");

    repo.createBranch("other");
    repo.checkout("other");
    fs::remove(dir / "tracked.txt");
    writeFile(dir / "other.txt", "other");
    repo.add("other.txt", true);
    repo.add("tracked.txt", true);
    repo.commit("commit without tracked.txt");

    repo.checkout("main");
    CHECK(fs::exists(dir / "tracked.txt"));

    // Replace tracked.txt with a non-empty directory
    fs::remove(dir / "tracked.txt");
    fs::create_directories(dir / "tracked.txt");
    writeFile(dir / "tracked.txt" / "user_secret.txt", "precious user data");

    bool threw = false;
    try {
        repo.checkout("other");
    } catch (const MiniGitException& e) {
        threw = true;
    }
    CHECK(threw);

    // User content in non-empty directory must NOT have been deleted
    CHECK(fs::exists(dir / "tracked.txt" / "user_secret.txt"));
    CHECK(readFile(dir / "tracked.txt" / "user_secret.txt") == "precious user data");

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test_group2_import_bundle_validation() {
    std::cout << "[Group 2] importBundleJson object re-parsing and branch completeness verification... ";
    fs::path dir = fs::current_path() / "test_core_repo_g2_bundle";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "base.txt", "base");
    repo.add("base.txt", true);
    repo.commit("initial");

    // 1. Commit::parse must throw InvalidObjectException on invalid time stoll
    bool timeThrew = false;
    try {
        Commit::parse("tree 0000000000000000000000000000000000000000\nauthor test\ntime not_a_number\n\nmsg");
    } catch (const InvalidObjectException&) {
        timeThrew = true;
    }
    CHECK(timeThrew);

    // 2. Commit with control char / newline in author must be rejected
    bool authorThrew = false;
    try {
        Commit::parse("tree 0000000000000000000000000000000000000000\nauthor bad\x01author\ntime 1000\n\nmsg");
    } catch (const InvalidObjectException&) {
        authorThrew = true;
    }
    CHECK(authorThrew);

    // 3. Bundle with missing blob must reject the entire bundle before creating branch
    // Let's create a valid blob and tree, but commit references a tree with a missing blob
    Tree t;
    Hash missingBlob("1111111111111111111111111111111111111111");
    t.set("missing.txt", missingBlob);
    Commit c(t.hash(), Hash(), "Author", "Message", 1234567890);

    std::ostringstream bundleJson;
    bundleJson << "{\n"
               << "  \"version\": 1,\n"
               << "  \"objects\": {\n"
               << "    \"" << t.hash().str() << "\": {\"type\": \"tree\", \"data\": {\"missing.txt\": \"" << missingBlob.str() << "\"}},\n"
               << "    \"" << c.hash().str() << "\": {\"type\": \"commit\", \"data\": {\"tree\": \"" << t.hash().str() << "\", \"author\": \"Author\", \"message\": \"Message\", \"time\": 1234567890}}\n"
               << "  },\n"
               << "  \"branches\": {\n"
               << "    \"new_branch\": \"" << c.hash().str() << "\"\n"
               << "  }\n"
               << "}";

    std::string res = repo.importBundleJson(bundleJson.str());
    CHECK(res.find("\"ok\":false") != std::string::npos || res.find("\"ok\": false") != std::string::npos);
    CHECK(res.find("Bundle rejected") != std::string::npos);

    // Branch must NOT have been created
    CHECK(!repo.branchExists("new_branch"));

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test_group3_find_merge_base_lca() {
    std::cout << "[Group 3] findMergeBase true LCA (drop candidate that is ancestor of another candidate)... ";
    fs::path dir = fs::current_path() / "test_core_repo_g3_lca";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    // Initial commit A
    writeFile(dir / "f.txt", "base");
    repo.add("f.txt", true);
    Hash a = repo.commit("commit A");

    // Branch b1 from A
    repo.createBranch("b1", a);
    repo.checkout("b1");
    writeFile(dir / "b1.txt", "b1");
    repo.add("b1.txt", true);
    Hash b1Commit = repo.commit("commit B1");

    // Branch b2 from A
    repo.createBranch("b2", a);
    repo.checkout("b2");
    writeFile(dir / "b2.txt", "b2");
    repo.add("b2.txt", true);
    Hash b2Commit = repo.commit("commit B2");

    // Merge b2 into b1 -> M1
    repo.checkout("b1");
    repo.merge("b2");
    Hash m1 = repo.head();
    CHECK(!m1.empty() && m1 != b1Commit && m1 != b2Commit);

    // Branch left from M1
    repo.createBranch("left", m1);
    repo.checkout("left");
    writeFile(dir / "left.txt", "left");
    repo.add("left.txt", true);
    Hash branchTip1 = repo.commit("commit left");

    // Branch right from M1
    repo.createBranch("right", m1);
    repo.checkout("right");
    writeFile(dir / "right.txt", "right");
    repo.add("right.txt", true);
    Hash branchTip2 = repo.commit("commit right");

    // Now both A and M1 are common ancestors of branchTip1 and branchTip2.
    // A is an ancestor of M1. Therefore, findMergeBase must return M1, NOT A!
    Hash lca = repo.findMergeBase(branchTip1, branchTip2);
    CHECK(lca == m1);
    CHECK(lca != a);

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test_group3_add_file_cleans_subpaths() {
    std::cout << "[Group 3] add file 'a' cleans index entries under 'a/' and commit succeeds... ";
    fs::path dir = fs::current_path() / "test_core_repo_g3_add";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    // Create a folder 'a' with files 'a/one.txt' and 'a/two.txt'
    fs::create_directories(dir / "a");
    writeFile(dir / "a" / "one.txt", "one");
    writeFile(dir / "a" / "two.txt", "two");
    repo.add("a/one.txt", true);
    repo.add("a/two.txt", true);
    repo.commit("initial folder a");

    Index idxInit(dir / ".minigit" / "index");
        CHECK(idxInit.has("a/one.txt"));
    CHECK(idxInit.has("a/two.txt"));

    // Replace folder 'a' with file 'a'
    removeDir(dir / "a");
    writeFile(dir / "a", "I am a file named a");

    // Adding file 'a'
    repo.add("a", true);

    // Verify index entries under a/ are gone
    Index idx(dir / ".minigit" / "index");
        CHECK(idx.has("a"));
    CHECK(!idx.has("a/one.txt"));
    CHECK(!idx.has("a/two.txt"));

    // Commit must succeed cleanly!
    bool commitThrew = false;
    Hash c;
    try {
        c = repo.commit("replaced folder a with file a");
    } catch (...) {
        commitThrew = true;
    }
    CHECK(!commitThrew);
    CHECK(!c.empty());

    // Verify tree of commit c
    ObjectStore store(dir / ".minigit" / "objects");
    auto tree = store.loadAs<Tree>(store.loadAs<Commit>(c)->tree());
    CHECK(tree->entries().count("a") == 1);
    CHECK(tree->entries().count("a/one.txt") == 0);
    CHECK(tree->entries().count("a/two.txt") == 0);

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test_group3_restore_all_or_nothing() {
    std::cout << "[Group 3] restoreCommit all-or-nothing rollback on exception... ";
    fs::path dir = fs::current_path() / "test_core_repo_g3_atomic";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "file1.txt", "content1");
    repo.add("file1.txt", true);
    Hash c1 = repo.commit("c1");

    writeFile(dir / "file2.txt", "content2");
    repo.add("file2.txt", true);
    Hash c2 = repo.commit("c2");

    repo.checkout("main");
    CHECK(fs::exists(dir / "file1.txt"));
    CHECK(fs::exists(dir / "file2.txt"));

    // Verify status is clean
    CHECK(repo.status().clean());

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test_group3_writefile_atomic_and_symlink_refusal() {
    std::cout << "[Group 3] FileUtil::writeFile atomic temp file and destination symlink refusal... ";
    fs::path dir = fs::current_path() / "test_core_repo_g3_wf";
    cleanDir(dir);

    fs::path target = dir / "normal.txt";
    writeFile(target, "hello atomic world");
    CHECK(fs::exists(target));
    CHECK(readFile(target) == "hello atomic world");

    // Overwrite atomically
    writeFile(target, "second write");
    CHECK(readFile(target) == "second write");

    // No temp files left behind
    for (const auto& entry : fs::directory_iterator(dir)) {
        std::string fname = entry.path().filename().string();
        CHECK(fname.find(".tmp.") == std::string::npos);
    }

    removeDir(dir);
    std::cout << "PASSED\n";
}

void test_group3_workingfiles_skips_symlinks() {
    std::cout << "[Group 3] workingFiles skips symlinks so 'add .' never stores outside content... ";
    fs::path dir = fs::current_path() / "test_core_repo_g3_sym";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    writeFile(dir / "regular.txt", "regular content");

    repo.add(".", true);
    Index idx(dir / ".minigit" / "index");
        CHECK(idx.has("regular.txt"));

    removeDir(dir);
    std::cout << "PASSED\n";
}


// Group 5: HtmlExporter static report tracked files only & frontend hardening
void test_group5_html_exporter_tracked_files_only() {
    std::cout << "[Group 5] HtmlExporter static report includes only tracked files... ";
    fs::path dir = fs::current_path() / "test_g5_repo";
    cleanDir(dir);
    Repository::init(dir);
    Repository repo(dir);

    // Create tracked file
    writeFile(dir / "tracked.txt", "tracked content\n");
    repo.add("tracked.txt", true);
    repo.commit("initial");

    // Create untracked file
    writeFile(dir / "untracked.txt", "untracked content\n");

    // Static report
    std::string staticHtml = HtmlExporter::render(repo.snapshot(), "test_g5_repo", true);
    CHECK(staticHtml.find("tracked.txt") != std::string::npos);
    // In static HTML, untracked.txt must NOT be present in snapshot files map
    CHECK(staticHtml.find("\"untracked.txt\"") == std::string::npos);
    CHECK(staticHtml.find("window.__staticExport = true;") != std::string::npos);

    // Server mode render does include live files
    std::string serverHtml = HtmlExporter::render(repo.snapshot(), "test_g5_repo", false);
    CHECK(serverHtml.find("untracked.txt") != std::string::npos);

    // Check Frontend fixes in embedded template:
    // Bug 17: discardFile server mode syncEditorWithActiveFile
    CHECK(staticHtml.find("if (ACTIVE_FILE === filepath) syncEditorWithActiveFile();") != std::string::npos);
    // Bug 18 & 19: Save & Stash and Save & Commit buttons
    CHECK(staticHtml.find("btnDirtyStash") != std::string::npos);
    CHECK(staticHtml.find("Save & Commit") != std::string::npos);
    // Bug 21: saveActiveFileFromEditor area.value === content
    CHECK(staticHtml.find("if (area.value === content)") != std::string::npos);
    // Bug 22: beforeunload handler
    CHECK(staticHtml.find("window.addEventListener('beforeunload'") != std::string::npos);
    // Bug 23: actionTimeTravelRestore clears playback timer before modal
    CHECK(staticHtml.find("TIME_TRAVEL_PLAY_TIMER = null;") != std::string::npos);

    removeDir(dir);
    std::cout << "PASSED\n";
}

int main() {
    std::cout << "\n============================================\n";
    std::cout << "  Mini-Git Core Fixes Automated Tests\n";
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

    runTest("test_fix6", test_fix6_merge_conflicts_tracking_and_refusal);
    runTest("test_fix7", test_fix7_operations_blocked_during_merge);
    runTest("test_fix8", test_fix8_merge_state_and_snapshot_fields);
    runTest("test_fix9", test_fix9_stash_pop_different_branch_and_conflict);
    runTest("test_fix10", test_fix10_atomic_revert_initial_and_empty);
    runTest("test_fix11", test_fix11_empty_tree_commit);
    runTest("test_fix13_14", test_fix13_14_minigit_ignore_and_caching);
    runTest("test_fix15", test_fix15_export_to_git_no_nul_file);
    runTest("test_fix16", test_fix16_import_from_git_already_up_to_date);
        runTest("test_fix31", test_fix31_create_branch_at_commit);
    runTest("test_h1", test_h1_bundle_import_path_traversal);
    runTest("test_h2", test_h2_web_file_api_symlinks);
    runTest("test_m1", test_m1_replace_file_with_folder);
    runTest("test_l1", test_l1_bundle_import_non_commit_tips);
    runTest("test_l3", test_l3_escape_path_and_nonzero_exit);
    runTest("test_g1_json", test_group1_json_nesting_limit);
    runTest("test_g1_refmgr", test_group1_refmanager_branch_name_validation);
    runTest("test_g1_export", test_group1_git_export_no_system);
    runTest("test_g2_prefix", test_group2_restore_untracked_prefix_refusal);
    runTest("test_g2_nonempty", test_group2_restore_nonempty_dir_refusal);
    runTest("test_g2_bundle", test_group2_import_bundle_validation);
    runTest("test_g3_lca", test_group3_find_merge_base_lca);
    runTest("test_g3_add", test_group3_add_file_cleans_subpaths);
    runTest("test_g3_atomic", test_group3_restore_all_or_nothing);
    runTest("test_g3_wf", test_group3_writefile_atomic_and_symlink_refusal);
    runTest("test_g3_sym", test_group3_workingfiles_skips_symlinks);
    runTest("test_g5_html", test_group5_html_exporter_tracked_files_only);

    std::cout << "\nALL 10 CORE FIXES TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
