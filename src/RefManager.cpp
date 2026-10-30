#include "RefManager.h"
#include <algorithm>
#include <cctype>

static const std::string kPrefix = "ref: refs/heads/";

void RefManager::initHead(const std::string& b) const { writeFile(headFile(), kPrefix + b + "\n"); }

bool RefManager::onBranch() const { return trim(readFile(headFile())).rfind(kPrefix, 0) == 0; }

std::string RefManager::currentBranch() const {
    std::string h = trim(readFile(headFile()));
    return h.rfind(kPrefix, 0) == 0 ? h.substr(kPrefix.size()) : "";
}

Hash RefManager::headCommit() const {
    if (onBranch()) {
        std::string b = currentBranch();
        return branchExists(b) ? branchTip(b) : Hash();
    }
    return Hash(trim(readFile(headFile())));
}

void RefManager::updateHead(const Hash& h) const {
    if (onBranch()) writeFile(branchFile(currentBranch()), h.str() + "\n");
    else            writeFile(headFile(), h.str() + "\n");
}

bool RefManager::branchExists(const std::string& n) const {
    if (!isValidBranchName(n)) throw BranchException("invalid branch name: '" + n + "'");
    return fs::exists(branchFile(n));
}

Hash RefManager::branchTip(const std::string& n) const {
    if (!isValidBranchName(n)) throw BranchException("invalid branch name: '" + n + "'");
    if (!branchExists(n)) throw BranchException("no such branch: " + n);
    return Hash(trim(readFile(branchFile(n))));
}

bool RefManager::isValidBranchName(const std::string& n) {
    if (n.empty() || n.size() > 100 || n[0] == '.' || n[0] == '-') return false;
    if (n == "HEAD") return false;
    if (n.size() >= 5 && n.compare(n.size() - 5, 5, ".lock") == 0) return false;
    if (n.rfind("peer-", 0) == 0) {
        std::string suffix = n.substr(5);
        if (suffix.empty() || suffix[0] == '.' || suffix[0] == '-') return false;
    }
    return std::all_of(n.begin(), n.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.';
    });
}

void RefManager::createBranch(const std::string& n, const Hash& at) const {
    if (!isValidBranchName(n)) throw BranchException("invalid branch name: '" + n + "'");
    if (branchExists(n)) throw BranchException("branch already exists: " + n);
    writeFile(branchFile(n), at.str() + "\n");
}

void RefManager::setBranch(const std::string& n, const Hash& at) const {
    if (!isValidBranchName(n)) throw BranchException("invalid branch name: '" + n + "'");
    writeFile(branchFile(n), at.str() + "\n");
}

void RefManager::deleteBranch(const std::string& n) const {
    if (!isValidBranchName(n)) throw BranchException("invalid branch name: '" + n + "'");
    if (!branchExists(n)) throw BranchException("branch does not exist: " + n);
    if (onBranch() && currentBranch() == n) throw BranchException("cannot delete currently checked-out branch: " + n);
    fs::remove(branchFile(n));
}

std::vector<std::string> RefManager::listBranches() const {
    std::vector<std::string> names;
    fs::path dir = gitDir_ / "refs" / "heads";
    if (fs::exists(dir))
        for (const auto& e : fs::directory_iterator(dir)) names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

void RefManager::switchToBranch(const std::string& n) const {
    if (!isValidBranchName(n)) throw BranchException("invalid branch name: '" + n + "'");
    writeFile(headFile(), kPrefix + n + "\n");
}
void RefManager::detach(const Hash& h) const { writeFile(headFile(), h.str() + "\n"); }
