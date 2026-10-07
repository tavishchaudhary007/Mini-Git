#pragma once
#include <map>
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

// Backend facade. Composes ObjectStore + Index + RefManager; contains no I/O to cout/cin.
class Repository {
    fs::path root_, gitDir_;
    ObjectStore store_;
    Index index_;
    RefManager refs_;

    std::string relPath(const fs::path& p) const;
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

    void add(const std::string& path);
    Hash commit(const std::string& message);
    std::vector<Commit> log() const;
    Status status() const;
    std::string diff(const std::string& path) const;

    void createBranch(const std::string& name);
    std::vector<std::string> branches() const { return refs_.listBranches(); }
    std::string currentBranch() const { return refs_.currentBranch(); }
    Hash head() const { return refs_.headCommit(); }
    void checkout(const std::string& target);
    std::string merge(const std::string& branch);
};
