#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "FileUtil.h"
#include "Index.h"
#include "Object.h"
#include "ObjectStore.h"
#include "RefManager.h"

struct Status {
    std::vector<std::string> staged, modified, deleted, untracked;
    bool clean() const { return staged.empty() && modified.empty() && deleted.empty() && untracked.empty(); }
};

struct ReflogEntry {
    std::string timestamp;
    std::string action;
    std::string oldHash;
    std::string newHash;
    std::string description;
};

// Read-only copy of everything a frontend needs to display a repository.
struct Snapshot {
    std::string currentBranch;                      // "" when HEAD is detached
    Hash head;                                      // empty before the first commit
    std::map<std::string, Hash> branches;           // branch -> tip commit
    std::vector<std::unique_ptr<Object>> objects;   // every blob / tree / commit in the store
    std::map<std::string, std::string> files;       // working directory (text files)
    std::map<std::string, Hash> staged;             // index entries that differ from HEAD
    std::vector<std::string> stagedDeleted;         // in HEAD but removed from the index
};

// Backend facade. Composes ObjectStore + Index + RefManager; contains no I/O to cout/cin.
class Repository {
    fs::path root_, gitDir_;
    ObjectStore store_;
    Index index_;
    RefManager refs_;

    std::string relPath(const fs::path& p) const;
    bool isIgnored(const fs::path& fullPath, const std::string& relPath, bool isDir = false) const;
    std::vector<std::string> workingFiles() const;
    std::map<std::string, Hash> headTree() const;
    Hash hashWorking(const std::string& rel) const;
    void addFile(const std::string& rel);
    void restoreCommit(const Hash& c);
    bool isAncestor(const Hash& ancestor, const Hash& descendant) const;
    bool isDirty() const;
    static std::string author();

public:
    explicit Repository(const fs::path& root);
    static void init(const fs::path& dir);
    static fs::path findRoot(fs::path start);
    const fs::path& root() const { return root_; }

    void add(const std::string& path);
    void unstage(const std::string& path);
    void discard(const std::string& path);
    Hash commit(const std::string& message);
    Hash revert(const Hash& commitHash);
    std::vector<Commit> log() const;
    Status status() const;
    std::string diff(const std::string& path) const;
    Snapshot snapshot() const;

    void createBranch(const std::string& name);
    void deleteBranch(const std::string& name) { refs_.deleteBranch(name); }
    std::vector<std::string> branches() const { return refs_.listBranches(); }
    std::string currentBranch() const { return refs_.currentBranch(); }
    Hash head() const { return refs_.headCommit(); }
    Hash branchTip(const std::string& name) const { return refs_.branchTip(name); }
    bool branchExists(const std::string& name) const { return refs_.branchExists(name); }
    void checkout(const std::string& target);
    std::string merge(const std::string& branch);

    // Safety Nets & Reflog
    void recordReflog(const std::string& action, const Hash& oldH, const Hash& newH, const std::string& desc);
    std::vector<ReflogEntry> reflog() const;
    std::string undo();
    std::string stashSave(const std::string& message = "");
    std::string stashPop();
    bool hasStash() const;

    // Serverless P2P & LAN Sync Bundle
    std::string exportBundleJson() const;
    std::string importBundleJson(const std::string& bundleJson);

    // Git Interoperability Bridge
    std::string gitStatus() const;
    std::string exportToGit();
    std::string importFromGit();
};
