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

    std::cout << "\nALL 10 CORE FIXES TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
