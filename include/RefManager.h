#pragma once
#include <string>
#include <vector>
#include "FileUtil.h"
#include "Hash.h"

// HEAD and branch pointers: .minigit/HEAD, .minigit/refs/heads/<name>
class RefManager {
    fs::path gitDir_;
    fs::path headFile() const { return gitDir_ / "HEAD"; }
    fs::path branchFile(const std::string& n) const { return gitDir_ / "refs" / "heads" / n; }
public:
    explicit RefManager(fs::path gitDir) : gitDir_(std::move(gitDir)) {}

    void initHead(const std::string& branch) const;
    bool onBranch() const;
    std::string currentBranch() const;  // "" when detached
    Hash headCommit() const;            // empty Hash if no commits yet
    void updateHead(const Hash& h) const;

    bool branchExists(const std::string& n) const { return fs::exists(branchFile(n)); }
    Hash branchTip(const std::string& n) const;
    void createBranch(const std::string& n, const Hash& at) const;
    std::vector<std::string> listBranches() const;
    void switchToBranch(const std::string& n) const;
    void detach(const Hash& h) const;
};
