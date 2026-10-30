#include "Repository.h"
#include "PathSafety.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <queue>
#include <unordered_set>
#include "DiffEngine.h"
#include "HtmlExporter.h"
#include "Base64.h"
#include "Json.h"
#include <set>

Repository::Repository(const fs::path& root)
    : root_(root), gitDir_(root / ".minigit"), store_(gitDir_ / "objects"),
      index_(gitDir_ / "index"), refs_(gitDir_) {}

void Repository::reload() {
    index_.reload();
}

void Repository::init(const fs::path& dir) {
    fs::path g = dir / ".minigit";
    if (fs::exists(g)) throw MiniGitException("repository already initialized");
    fs::create_directories(g / "objects");
    fs::create_directories(g / "refs" / "heads");
    RefManager(g).initHead("main");
}

fs::path Repository::findRoot(fs::path start) {
    start = fs::absolute(start);
    while (true) {
        if (fs::exists(start / ".minigit")) return fs::canonical(start);
        if (!start.has_parent_path() || start.parent_path() == start)
            throw NotARepoException("not a mini-git repository (run 'minigit init')");
        start = start.parent_path();
    }
}

std::string Repository::author() {
    for (const char* v : {"MINIGIT_AUTHOR", "USER", "USERNAME"})
        if (const char* e = std::getenv(v)) return e;
    return "unknown";
}

std::string Repository::relPath(const fs::path& p) const {
    fs::path rel = fs::weakly_canonical(fs::absolute(p)).lexically_relative(fs::weakly_canonical(root_));
    std::string s = rel.generic_string();
    if (s == "." || s.empty()) return "";
    if (s.rfind("..", 0) == 0) throw MiniGitException("path is outside the repository: " + p.generic_string());
    return s;
}

std::string Repository::resolveRepoPath(const std::string& pathArg, bool isRepoRelative) const {
    for (char c : pathArg) {
        if (static_cast<unsigned char>(c) < 32 || c == 127 ||
            c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            throw MiniGitException("invalid path: contains illegal character: " + pathArg);
        }
    }
    if (pathArg.empty() || pathArg == "." || pathArg == "..") {
        throw MiniGitException("invalid path: cannot be empty, '.', or '..'");
    }
    fs::path inPath(pathArg);
    if (inPath.is_absolute()) {
        throw MiniGitException("invalid path: absolute paths are not allowed: " + pathArg);
    }
    fs::path targetPath = isRepoRelative ? (root_ / inPath) : fs::absolute(inPath);
    fs::path normRoot = fs::weakly_canonical(root_);

    fs::path cur = normRoot;
    fs::path relIn = isRepoRelative ? inPath : targetPath.lexically_relative(normRoot);
    for (const auto& elem : relIn) {
        cur /= elem;
        if (isLinkOrSymlink(cur)) {
            throw MiniGitException("path is outside the repository: " + pathArg);
        }
    }

    fs::path full = fs::weakly_canonical(targetPath);
    fs::path rel = full.lexically_relative(normRoot);
    std::string relStr = rel.generic_string();
    if (relStr.empty() || relStr == ".") {
        throw MiniGitException("invalid path: refers to repository root");
    }
    if (relStr.rfind("..", 0) == 0 || rel.is_absolute()) {
        throw MiniGitException("path is outside the repository: " + pathArg);
    }
    if (relStr == ".minigit" || relStr.rfind(".minigit/", 0) == 0) {
        throw MiniGitException("invalid path: cannot access .minigit: " + pathArg);
    }
    return relStr;
}

static void removeConflicts(const fs::path& gitDir, const std::vector<std::string>& resolvedPaths) {
    fs::path p = gitDir / "MERGE_CONFLICTS";
    if (!fs::exists(p) || resolvedPaths.empty()) return;
    std::set<std::string> resolved(resolvedPaths.begin(), resolvedPaths.end());
    std::vector<std::string> remaining;
    std::ifstream ifs(p);
    std::string line;
    while (std::getline(ifs, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        while (!line.empty() && (line.front() == ' ')) line.erase(line.begin());
        if (!line.empty() && !resolved.count(line)) {
            remaining.push_back(line);
        }
    }
    ifs.close();
    std::error_code ec;
    if (remaining.empty()) {
        fs::remove(p, ec);
    } else {
        std::ofstream ofs(p, std::ios::trunc);
        for (const auto& rem : remaining) {
            ofs << rem << "\n";
        }
    }
}

bool Repository::isIgnored(const fs::path& fullPath, const std::string& relPath, bool isDir) const {
    std::string fname = fullPath.filename().generic_string();
    if (fname == ".git" || fname == ".minigit" || fname == "node_modules") {
        return true;
    }
    if (relPath == ".git" || relPath.rfind(".git/", 0) == 0 ||
        relPath == ".minigit" || relPath.rfind(".minigit/", 0) == 0 ||
        relPath == "node_modules" || relPath.rfind("node_modules/", 0) == 0) {
        return true;
    }
    // Only ignore minigit / minigit.exe at the repo root
    if (!isDir && (relPath == "minigit" || relPath == "minigit.exe")) {
        return true;
    }

    fs::path ignPath = root_ / ".minigitignore";
    std::error_code ec;
    bool ignExists = fs::exists(ignPath, ec);
    if (!ignExists) {
        if (!ignoreFileCached_ || !ignorePatterns_.empty()) {
            ignorePatterns_.clear();
            ignoreFileCached_ = true;
            ignoreMtime_ = fs::file_time_type{};
        }
    } else {
        auto mtime = fs::last_write_time(ignPath, ec);
        if (!ignoreFileCached_ || mtime != ignoreMtime_) {
            ignoreMtime_ = mtime;
            ignoreFileCached_ = true;
            ignorePatterns_.clear();
            std::ifstream ifs(ignPath);
            std::string line;
            while (std::getline(ifs, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
                while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
                if (line.empty() || line.front() == '#') continue;
                ignorePatterns_.push_back(line);
            }
        }
    }

    for (const auto& line : ignorePatterns_) {
        if (line.back() == '/') {
            std::string dirPattern = line.substr(0, line.size() - 1);
            if (fname == dirPattern || relPath == dirPattern || relPath.rfind(dirPattern + "/", 0) == 0) {
                return true;
            }
            if (relPath.find("/" + dirPattern + "/") != std::string::npos ||
                (relPath.size() >= dirPattern.size() + 1 && relPath.substr(relPath.size() - dirPattern.size() - 1) == "/" + dirPattern)) {
                return true;
            }
        } else if (line.rfind("*.", 0) == 0) {
            std::string ext = line.substr(1);
            if (fname.size() >= ext.size() && fname.substr(fname.size() - ext.size()) == ext) {
                return true;
            }
        } else {
            if (fname == line || relPath == line) {
                return true;
            }
        }
    }
    return false;
}

std::vector<std::string> Repository::workingFiles() const {
    std::vector<std::string> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root_, fs::directory_options::none, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        fs::file_status symStatus = it->symlink_status(ec);
        if (fs::is_symlink(symStatus) || isLinkOrSymlink(it->path())) {
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        std::string rel = it->path().lexically_relative(root_).generic_string();
        if (fs::is_directory(symStatus)) {
            if (isIgnored(it->path(), rel, true)) {
                it.disable_recursion_pending();
                continue;
            }
        } else if (fs::is_regular_file(symStatus)) {
            if (!isIgnored(it->path(), rel, false)) {
                files.push_back(rel);
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::map<std::string, Hash> Repository::headTree() const {
    Hash h = refs_.headCommit();
    if (h.empty()) return {};
    return store_.loadAs<Tree>(store_.loadAs<Commit>(h)->tree())->entries();
}

Hash Repository::hashWorking(const std::string& rel) const { return Blob(readFile(root_ / rel)).hash(); }

void Repository::addFile(const std::string& rel) {
    std::string prefix = rel + "/";
    std::vector<std::string> subGone;
    for (const auto& [p, _] : index_.entries()) {
        if (p.rfind(prefix, 0) == 0) {
            subGone.push_back(p);
        }
    }
    for (const auto& sg : subGone) {
        index_.remove(sg);
    }
    index_.set(rel, store_.save(Blob(readFile(root_ / rel))));
}

void Repository::add(const std::string& pathArg, bool isRepoRelative) {
    if (pathArg == "." || pathArg.empty()) {
        std::vector<std::string> added;
        for (const auto& f : workingFiles()) {
            if (hasIllegalPathChars(f)) {
                std::cerr << "warning: skipping file with illegal characters in path: " << escapePath(f) << "\n";
                continue;
            }
            addFile(f);
            added.push_back(f);
        }
        std::vector<std::string> gone;
        for (const auto& [path, h] : index_.entries())
            if (!fs::exists(root_ / path) || fs::is_directory(root_ / path)) gone.push_back(path);
        for (const auto& g : gone) index_.remove(g);
        removeConflicts(gitDir_, added);
        removeConflicts(gitDir_, gone);
        index_.save();
        return;
    }
    std::string rel = resolveRepoPath(pathArg, isRepoRelative);
    fs::path full = root_ / rel;
    if (fs::is_directory(full)) {
        std::string prefix = rel + "/";
        std::vector<std::string> added;
        for (const auto& f : workingFiles()) {
            if (f.rfind(prefix, 0) == 0) {
                if (hasIllegalPathChars(f)) {
                    std::cerr << "warning: skipping file with illegal characters in path: " << escapePath(f) << "\n";
                    continue;
                }
                addFile(f);
                added.push_back(f);
            }
        }
        std::vector<std::string> gone;
        if (index_.has(rel)) gone.push_back(rel);
        for (const auto& [path, h] : index_.entries())
            if (path.rfind(prefix, 0) == 0 && (!fs::exists(root_ / path) || fs::is_directory(root_ / path))) gone.push_back(path);
        for (const auto& g : gone) index_.remove(g);
        removeConflicts(gitDir_, added);
        removeConflicts(gitDir_, gone);
    } else if (fs::is_regular_file(full)) {
        if (!isIgnored(full, rel, false)) {
            std::string prefix = rel + "/";
            std::vector<std::string> subGone;
            for (const auto& [p, _] : index_.entries()) {
                if (p.rfind(prefix, 0) == 0) {
                    subGone.push_back(p);
                }
            }
            addFile(rel);
            removeConflicts(gitDir_, {rel});
            if (!subGone.empty()) {
                removeConflicts(gitDir_, subGone);
            }
        }
    } else if (index_.has(rel)) {
        index_.remove(rel);  // staging a deletion
        removeConflicts(gitDir_, {rel});
    } else {
        throw FileNotFoundException("file not found: " + pathArg);
    }
    index_.save();
}

void Repository::unstage(const std::string& pathArg, bool isRepoRelative) {
    auto head = headTree();
    if (pathArg.empty() || pathArg == ".") {
        index_.replace(head);
        index_.save();
        return;
    }
    std::string rel = resolveRepoPath(pathArg, isRepoRelative);
    if (head.count(rel)) {
        index_.set(rel, head.at(rel));
    } else {
        index_.remove(rel);
    }
    index_.save();
}

void Repository::discard(const std::string& pathArg, bool isRepoRelative) {
    std::string rel = resolveRepoPath(pathArg, isRepoRelative);
    fs::path full = (root_ / rel).lexically_normal();
    if (full == root_ || rel.empty() || rel == "." || rel.rfind("..", 0) == 0 ||
        rel == ".minigit" || rel.rfind(".minigit/", 0) == 0) {
        throw MiniGitException("cannot discard entire repository or internal files: " + pathArg);
    }
    auto head = headTree();
    if (head.count(rel)) {
        writeFile(full, store_.loadAs<Blob>(head.at(rel))->content());
        index_.set(rel, head.at(rel));
        index_.save();
    } else if (index_.has(rel)) {
        writeFile(full, store_.loadAs<Blob>(index_.get(rel))->content());
    } else if (fs::exists(full)) {
        if (!fs::is_regular_file(full)) {
            throw MiniGitException("discard only works on regular files: " + pathArg);
        }
        fs::remove(full);
    } else {
        throw FileNotFoundException("file not found: " + pathArg);
    }
}

Hash Repository::revert(const Hash& commitHash) {
    if (isMerging()) throw MergeException("merge in progress; commit or abort it first");
    if (isDirty()) throw MiniGitException("you have uncommitted changes; commit them first");

    auto c = store_.loadAs<Commit>(commitHash);
    Hash parentHash = c->parent();

    std::map<std::string, Hash> cTree = store_.loadAs<Tree>(c->tree())->entries();
    std::map<std::string, Hash> pTree;
    if (!parentHash.empty() && store_.exists(parentHash)) {
        pTree = store_.loadAs<Tree>(store_.loadAs<Commit>(parentHash)->tree())->entries();
    }
    auto head = headTree();

    std::vector<std::string> addedInC;
    std::vector<std::string> removedInC;
    std::vector<std::string> modifiedInC;

    for (const auto& [path, h] : cTree) {
        if (!pTree.count(path)) {
            addedInC.push_back(path);
        } else if (pTree.at(path) != h) {
            modifiedInC.push_back(path);
        }
    }
    for (const auto& [path, h] : pTree) {
        if (!cTree.count(path)) {
            removedInC.push_back(path);
        }
    }

    for (const auto& path : addedInC) {
        if (!head.count(path) || head.at(path) != cTree.at(path)) {
            throw MiniGitException("revert conflict in " + path);
        }
    }
    for (const auto& path : removedInC) {
        if (head.count(path)) {
            throw MiniGitException("revert conflict in " + path);
        }
    }
    for (const auto& path : modifiedInC) {
        if (!head.count(path) || head.at(path) != cTree.at(path)) {
            throw MiniGitException("revert conflict in " + path);
        }
    }

    // Fix 10: Cannot revert initial commit
    if (parentHash.empty()) {
        throw MiniGitException("cannot revert the initial commit");
    }

    std::map<std::string, Hash> resultTree = head;
    for (const auto& path : addedInC) resultTree.erase(path);
    for (const auto& path : removedInC) resultTree[path] = pTree.at(path);
    for (const auto& path : modifiedInC) resultTree[path] = pTree.at(path);

    if (resultTree == head || (addedInC.empty() && removedInC.empty() && modifiedInC.empty())) {
        throw MiniGitException("nothing to revert");
    }

    std::map<std::string, Hash> toWriteRevert;
    for (const auto& path : removedInC) toWriteRevert[path] = pTree.at(path);
    for (const auto& path : modifiedInC) toWriteRevert[path] = pTree.at(path);

    checkUntrackedOverwrite(toWriteRevert);

    for (const auto& path : addedInC) {
        if (!isSafeRepoPath(path)) throw MiniGitException("refusing operation with unsafe path: " + path);
        ensureNoSymlinkComponents(root_, path);
        fs::path full = root_ / path;
        std::error_code ec;
        if (fs::exists(full, ec) && fs::is_directory(full, ec) && !fs::is_empty(full, ec)) {
            throw MiniGitException("refusing to remove non-empty directory at tracked path: " + path);
        }
    }
    for (const auto& [path, _] : toWriteRevert) {
        if (!isSafeRepoPath(path)) throw MiniGitException("refusing operation with unsafe path: " + path);
        ensureNoSymlinkComponents(root_, path);
        fs::path full = root_ / path;
        std::error_code ec;
        if (fs::exists(full, ec) && fs::is_directory(full, ec) && !fs::is_empty(full, ec)) {
            throw MiniGitException("cannot restore over non-empty directory: " + path);
        }
    }

    std::map<std::string, std::string> loadedRevertBlobs;
    for (const auto& [path, h] : toWriteRevert) {
        auto blob = store_.loadAs<Blob>(h);
        if (!blob) throw MiniGitException("missing blob object for path: " + path);
        loadedRevertBlobs[path] = blob->content();
    }

    Hash oldHead = refs_.headCommit();
    try {
        for (const auto& path : addedInC) {
            fs::path full = root_ / path;
            std::error_code ec;
            if (fs::exists(full, ec)) {
                if (fs::is_directory(full, ec)) {
                    if (fs::is_empty(full, ec)) fs::remove(full, ec);
                } else {
                    fs::remove(full, ec);
                }
            }
            for (fs::path d = full.parent_path(); d != root_ && !d.empty() && fs::is_directory(d, ec) && fs::is_empty(d, ec); d = d.parent_path()) {
                fs::remove(d, ec);
            }
            index_.remove(path);
        }
        for (const auto& [path, content] : loadedRevertBlobs) {
            writeFile(root_ / path, content);
            index_.set(path, toWriteRevert[path]);
        }
        index_.save();
        return commit("Revert \"" + c->message() + "\"");
    } catch (...) {
        if (!oldHead.empty()) {
            restoreCommit(oldHead);
        }
        throw;
    }
}

Hash Repository::commit(const std::string& message) {
    if (message.empty()) throw MiniGitException("commit message must not be empty");

    Hash parent = refs_.headCommit();
    fs::path mergeHeadFile = gitDir_ / "MERGE_HEAD";
    Hash parent2;
    if (fs::exists(mergeHeadFile)) {
        parent2 = Hash(trim(readFile(mergeHeadFile)));
    }

    if (!parent2.empty()) {
        // Fix 6: Check unresolved conflicts
        auto remainingConflicts = conflictedPaths();
        if (!remainingConflicts.empty()) {
            throw MergeException("cannot commit with unresolved conflicts");
        }
        // Check if staged content of any originally conflicted path (or all staged files) still contains conflict markers
        std::vector<std::string> allConflicted;
        fs::path allConfPath = gitDir_ / "MERGE_CONFLICTS_ALL";
        if (fs::exists(allConfPath)) {
            std::ifstream ifs(allConfPath);
            std::string line;
            while (std::getline(ifs, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                while (!line.empty() && (line.front() == ' ')) line.erase(line.begin());
                if (!line.empty()) allConflicted.push_back(line);
            }
        }
        for (const auto& cp : allConflicted) {
            if (index_.has(cp)) {
                Hash bHash = index_.get(cp);
                if (store_.exists(bHash)) {
                    std::string c = store_.loadAs<Blob>(bHash)->content();
                    if ((c.find("<<<<<<<") != std::string::npos && c.find("=======") != std::string::npos && c.find(">>>>>>>") != std::string::npos) ||
                        c.find("<<<<<<< ours") != std::string::npos) {
                        throw MergeException("cannot commit: staged content of '" + cp + "' still contains conflict markers");
                    }
                }
            }
        }
        for (const auto& [path, h] : index_.entries()) {
            if (store_.exists(h)) {
                std::string c = store_.loadAs<Blob>(h)->content();
                if (c.find("<<<<<<< ours") != std::string::npos) {
                    throw MergeException("cannot commit: staged content of '" + path + "' still contains conflict markers");
                }
            }
        }
    }

    bool headHasFiles = false;
    if (!parent.empty()) {
        auto parentCommit = store_.loadAs<Commit>(parent);
        auto parentTree = store_.loadAs<Tree>(parentCommit->tree());
        headHasFiles = !parentTree->entries().empty();
    }

    // Fix 11: allow empty-tree commit when HEAD's tree is not empty
    if (index_.empty()) {
        if (!headHasFiles) {
            throw NothingToCommitException("nothing to commit (staging area is empty)");
        }
    }

    Tree tree;
    for (const auto& [path, h] : index_.entries()) tree.set(path, h);
    tree.dropPrefixParents();

    std::vector<std::string> indexDrop;
    for (const auto& [path, h] : index_.entries()) {
        if (!tree.entries().count(path)) {
            indexDrop.push_back(path);
        }
    }
    for (const auto& p : indexDrop) {
        index_.remove(p);
    }
    if (!indexDrop.empty()) {
        index_.save();
    }

    if (parent2.empty() && !parent.empty() && store_.loadAs<Commit>(parent)->tree() == tree.hash())
        throw NothingToCommitException("nothing to commit (no changes staged)");

    store_.save(tree);
    Hash h = store_.save(Commit(tree.hash(), parent, author(), message, std::time(nullptr), parent2));
    refs_.updateHead(h);
    if (!parent2.empty()) {
        fs::remove(mergeHeadFile);
        std::error_code ec;
        fs::remove(gitDir_ / "MERGE_CONFLICTS", ec);
        fs::remove(gitDir_ / "MERGE_CONFLICTS_ALL", ec);
        recordReflog("merge", parent, h, message);
    } else {
        std::error_code ec;
        fs::remove(gitDir_ / "MERGE_CONFLICTS", ec);
        fs::remove(gitDir_ / "MERGE_CONFLICTS_ALL", ec);
        recordReflog("commit", parent, h, message);
    }
    return h;
}

std::vector<Commit> Repository::log() const {
    std::vector<Commit> out;
    Hash start = refs_.headCommit();
    if (start.empty()) return out;

    std::queue<Hash> q;
    std::unordered_set<std::string> visited;
    q.push(start);
    visited.insert(start.str());

    while (!q.empty()) {
        Hash h = q.front();
        q.pop();
        if (!store_.exists(h)) continue;
        auto c = store_.loadAs<Commit>(h);
        if (!c) continue;
        out.push_back(*c);

        if (!c->parent().empty() && visited.insert(c->parent().str()).second) {
            q.push(c->parent());
        }
        if (c->hasParent2() && !c->parent2().empty() && visited.insert(c->parent2().str()).second) {
            q.push(c->parent2());
        }
    }

    std::stable_sort(out.begin(), out.end(), [](const Commit& a, const Commit& b) {
        return a.time() > b.time();
    });

    return out;
}

Status Repository::status() const {
    Status s;
    auto head = headTree();
    for (const auto& [path, h] : index_.entries()) {
        auto it = head.find(path);
        if (it == head.end() || it->second != h) s.staged.push_back(path);
        if (!fs::exists(root_ / path) || fs::is_directory(root_ / path)) s.deleted.push_back(path);
        else if (hashWorking(path) != h) s.modified.push_back(path);
    }
    // files in HEAD but removed from the index are staged deletions
    for (const auto& [path, h] : head)
        if (!index_.has(path)) s.staged.push_back(path + " (deleted)");
    for (const auto& f : workingFiles())
        if (!index_.has(f)) s.untracked.push_back(f);
    return s;
}

std::string Repository::diff(const std::string& pathArg, bool isRepoRelative) const {
    std::vector<std::string> targets;
    if (pathArg.empty()) for (const auto& [p, h] : index_.entries()) targets.push_back(p);
    else {
        std::string rel = resolveRepoPath(pathArg, isRepoRelative);
        if (!index_.has(rel)) throw FileNotFoundException("not tracked: " + pathArg);
        targets.push_back(rel);
    }

    std::string out;
    size_t filesChanged = 0;
    size_t totalInsertions = 0;
    size_t totalDeletions = 0;

    for (const auto& path : targets) {
        auto oldBlob = store_.loadAs<Blob>(index_.get(path));
        std::string now = (fs::exists(root_ / path) && !fs::is_directory(root_ / path)) ? readFile(root_ / path) : "";
        if (oldBlob->content() == now) continue;

        filesChanged++;
        auto ops = DiffEngine<std::string>::diff(splitLines(oldBlob->content()), splitLines(now));
        out += "--- a/" + path + "\n+++ b/" + path + "\n";
        size_t i = 0, j = 0;
        for (const auto& op : ops) {
            using K = DiffOp<std::string>::Kind;
            if (op.kind == K::Keep)        { ++i; ++j; }
            else if (op.kind == K::Remove) {
                out += "-" + std::to_string(++i) + ": " + op.value + "\n";
                totalDeletions++;
            } else {
                out += "+" + std::to_string(++j) + ": " + op.value + "\n";
                totalInsertions++;
            }
        }
    }

    if (filesChanged > 0) {
        out += " " + std::to_string(filesChanged) + (filesChanged == 1 ? " file changed" : " files changed");
        if (totalInsertions > 0) {
            out += ", " + std::to_string(totalInsertions) + (totalInsertions == 1 ? " insertion(+)" : " insertions(+)");
        }
        if (totalDeletions > 0) {
            out += ", " + std::to_string(totalDeletions) + (totalDeletions == 1 ? " deletion(-)" : " deletions(-)");
        }
        out += "\n";
    }

    return out;
}

CommitStat Repository::commitStats(const Hash& commitHash) const {
    CommitStat result{ {}, 0, 0, 0 };
    if (commitHash.empty() || !store_.exists(commitHash)) return result;
    auto c = store_.loadAs<Commit>(commitHash);
    if (!c) return result;

    std::map<std::string, Hash> curTree;
    if (store_.exists(c->tree())) {
        curTree = store_.loadAs<Tree>(c->tree())->entries();
    }

    std::map<std::string, Hash> pTree;
    if (!c->parent().empty() && store_.exists(c->parent())) {
        auto pCommit = store_.loadAs<Commit>(c->parent());
        if (pCommit && store_.exists(pCommit->tree())) {
            pTree = store_.loadAs<Tree>(pCommit->tree())->entries();
        }
    }

    std::set<std::string> allPaths;
    for (const auto& [p, _] : curTree) allPaths.insert(p);
    for (const auto& [p, _] : pTree) allPaths.insert(p);

    for (const auto& path : allPaths) {
        Hash oldH = pTree.count(path) ? pTree.at(path) : Hash();
        Hash newH = curTree.count(path) ? curTree.at(path) : Hash();
        if (oldH == newH) continue;

        std::string oldContent = (!oldH.empty() && store_.exists(oldH)) ? store_.loadAs<Blob>(oldH)->content() : "";
        std::string newContent = (!newH.empty() && store_.exists(newH)) ? store_.loadAs<Blob>(newH)->content() : "";

        bool isBin = (oldContent.find('\0') != std::string::npos || newContent.find('\0') != std::string::npos);
        if (isBin) {
            result.files.push_back({path, 0, 0, true});
            result.totalFiles++;
            continue;
        }

        auto ops = DiffEngine<std::string>::diff(splitLines(oldContent), splitLines(newContent));
        size_t added = 0, removed = 0;
        for (const auto& op : ops) {
            if (op.kind == DiffOp<std::string>::Kind::Add) added++;
            else if (op.kind == DiffOp<std::string>::Kind::Remove) removed++;
        }
        result.files.push_back({path, added, removed, false});
        result.totalFiles++;
        result.totalAdded += added;
        result.totalRemoved += removed;
    }
    return result;
}

void Repository::createBranch(const std::string& name, const Hash& at) {
    if (isMerging()) {
        throw MergeException("merge in progress; commit or abort it first");
    }
    Hash tip;
    if (at.empty()) {
        tip = refs_.headCommit();
        if (tip.empty()) throw BranchException("cannot create a branch before the first commit");
    } else {
        tip = store_.resolvePrefix(at.str());
        store_.loadAs<Commit>(tip); // validates tip resolves to a commit
    }
    refs_.createBranch(name, tip);
    recordReflog("branch", tip, tip, "Created branch " + name);
}

bool Repository::isDirty() const {
    Status s = status();
    return !(s.staged.empty() && s.modified.empty() && s.deleted.empty());
}

bool Repository::isAncestor(const Hash& ancestor, const Hash& descendant) const {
    if (ancestor.empty() || descendant.empty()) return false;
    if (ancestor == descendant) return true;
    std::unordered_set<std::string> visited;
    std::queue<Hash> q;
    q.push(descendant);
    visited.insert(descendant.str());
    while (!q.empty()) {
        Hash cur = q.front();
        q.pop();
        if (cur == ancestor) return true;
        if (store_.exists(cur)) {
            auto c = store_.loadAs<Commit>(cur);
            if (!c->parent().empty() && visited.insert(c->parent().str()).second) {
                q.push(c->parent());
            }
            if (c->hasParent2() && !c->parent2().empty() && visited.insert(c->parent2().str()).second) {
                q.push(c->parent2());
            }
        }
    }
    return false;
}

Hash Repository::findMergeBase(const Hash& h1, const Hash& h2) const {
    if (h1.empty() || h2.empty()) return Hash();
    if (h1 == h2) return h1;

    std::unordered_set<std::string> visited1;
    std::queue<Hash> q1;
    q1.push(h1);
    visited1.insert(h1.str());
    while (!q1.empty()) {
        Hash cur = q1.front();
        q1.pop();
        if (store_.exists(cur)) {
            auto commit = store_.loadAs<Commit>(cur);
            if (!commit->parent().empty() && visited1.insert(commit->parent().str()).second) {
                q1.push(commit->parent());
            }
            if (commit->hasParent2() && !commit->parent2().empty() && visited1.insert(commit->parent2().str()).second) {
                q1.push(commit->parent2());
            }
        }
    }

    std::vector<Hash> common;
    std::unordered_set<std::string> visited2;
    std::queue<Hash> q2;
    q2.push(h2);
    visited2.insert(h2.str());
    while (!q2.empty()) {
        Hash cur = q2.front();
        q2.pop();
        if (visited1.count(cur.str())) {
            common.push_back(cur);
        }
        if (store_.exists(cur)) {
            auto commit = store_.loadAs<Commit>(cur);
            if (!commit->parent().empty() && visited2.insert(commit->parent().str()).second) {
                q2.push(commit->parent());
            }
            if (commit->hasParent2() && !commit->parent2().empty() && visited2.insert(commit->parent2().str()).second) {
                q2.push(commit->parent2());
            }
        }
    }

    if (common.empty()) return Hash();
    if (common.size() == 1) return common.front();

    // Drop any candidate that is an ancestor of another candidate
    std::vector<Hash> lcaCandidates;
    for (size_t i = 0; i < common.size(); ++i) {
        bool isOlder = false;
        for (size_t j = 0; j < common.size(); ++j) {
            if (i == j) continue;
            if (isAncestor(common[i], common[j])) {
                isOlder = true;
                break;
            }
        }
        if (!isOlder) {
            lcaCandidates.push_back(common[i]);
        }
    }

    if (lcaCandidates.empty()) return common.front();

    std::sort(lcaCandidates.begin(), lcaCandidates.end(), [&](const Hash& a, const Hash& b) {
        std::time_t ta = store_.loadAs<Commit>(a)->time();
        std::time_t tb = store_.loadAs<Commit>(b)->time();
        return ta > tb;
    });

    return lcaCandidates.front();
}

std::string Repository::mergeAbort() {
    fs::path mergeHead = gitDir_ / "MERGE_HEAD";
    if (!fs::exists(mergeHead)) {
        throw MergeException("no merge in progress to abort");
    }
    fs::remove(mergeHead);
    std::error_code ec;
    fs::remove(gitDir_ / "MERGE_CONFLICTS", ec);
    fs::remove(gitDir_ / "MERGE_CONFLICTS_ALL", ec);
    Hash cur = refs_.headCommit();
    if (!cur.empty()) {
        restoreCommit(cur);
    }
    return "Merge aborted.";
}

bool Repository::isMerging() const {
    return fs::exists(gitDir_ / "MERGE_HEAD");
}

std::vector<std::string> Repository::conflictedPaths() const {
    fs::path p = gitDir_ / "MERGE_CONFLICTS";
    if (!fs::exists(p)) return {};
    std::vector<std::string> out;
    std::ifstream ifs(p);
    std::string line;
    while (std::getline(ifs, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        while (!line.empty() && (line.front() == ' ')) line.erase(line.begin());
        if (!line.empty()) out.push_back(line);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void Repository::checkUntrackedOverwrite(const std::map<std::string, Hash>& targetTree) const {
    Status s = status();
    std::set<std::string> untrackedSet(s.untracked.begin(), s.untracked.end());

    // 1. Direct match with untracked files
    for (const auto& f : s.untracked) {
        if (targetTree.count(f)) {
            throw MiniGitException("untracked file '" + f + "' would be overwritten; move or commit it first");
        }
    }

    // 2. Parent prefix check:
    // If target has d/x, and d is an untracked file or untracked symlink on disk
    for (const auto& [tp, _] : targetTree) {
        fs::path rel(tp);
        fs::path cur;
        for (const auto& part : rel) {
            if (cur.empty()) cur = part;
            else cur /= part;
            std::string curStr = cur.generic_string();
            if (curStr == tp) break; // only check parent prefixes
            fs::path full = root_ / cur;
            std::error_code ec;
            if (isLinkOrSymlink(full)) {
                throw MiniGitException("untracked file '" + curStr + "' would be overwritten; move or commit it first");
            }
            if (fs::exists(full, ec) && !fs::is_directory(full, ec)) {
                if (!index_.has(curStr) || untrackedSet.count(curStr)) {
                    throw MiniGitException("untracked file '" + curStr + "' would be overwritten; move or commit it first");
                }
            }
        }
    }

    // 3. Any untracked item in s.untracked that sits as a parent prefix of target path
    for (const auto& f : s.untracked) {
        for (const auto& [tp, _] : targetTree) {
            if (tp.size() > f.size() && tp[f.size()] == '/' && tp.rfind(f, 0) == 0) {
                throw MiniGitException("untracked file '" + f + "' would be overwritten; move or commit it first");
            }
        }
    }
}

void Repository::restoreCommit(const Hash& c) {
    auto commit = store_.loadAs<Commit>(c);
    auto tree = store_.loadAs<Tree>(commit->tree());
    const auto& target = tree->entries();

    // 1. Pre-load all blobs and validate target paths
    std::map<std::string, std::string> loadedBlobs;
    for (const auto& [path, h] : target) {
        if (!isSafeRepoPath(path)) {
            throw MiniGitException("refusing to restore commit with unsafe path: " + path);
        }
        ensureNoSymlinkComponents(root_, path);
        auto blob = store_.loadAs<Blob>(h);
        if (!blob) {
            throw MiniGitException("missing blob object for path: " + path);
        }
        loadedBlobs[path] = blob->content();
    }

    // 2. Validate index paths: refuse if any tracked path is now a non-empty directory
    for (const auto& [path, h] : index_.entries()) {
        if (!isSafeRepoPath(path)) {
            throw MiniGitException("refusing operation with unsafe path: " + path);
        }
        ensureNoSymlinkComponents(root_, path);
        fs::path full = root_ / path;
        std::error_code ec;
        if (fs::exists(full, ec) && fs::is_directory(full, ec) && !fs::is_empty(full, ec)) {
            throw MiniGitException("tracked path is now a non-empty directory: " + path);
        }
    }

    // 3. Refuse if untracked file sits at path or parent prefix
    checkUntrackedOverwrite(target);

    auto oldIndex = index_.entries();
    std::map<std::string, std::string> backupFiles;
    std::vector<std::string> createdFiles;

    for (const auto& [p, _] : oldIndex) {
        fs::path full = root_ / p;
        if (fs::is_regular_file(full)) {
            backupFiles[p] = readFile(full);
        }
    }
    for (const auto& [p, _] : target) {
        if (!backupFiles.count(p)) {
            fs::path full = root_ / p;
            if (fs::is_regular_file(full)) {
                backupFiles[p] = readFile(full);
            }
        }
    }

    try {
        // 5. Delete absent files (no remove_all on user content)
        for (const auto& [path, h] : index_.entries()) {
            if (target.count(path)) continue;
            fs::path full = root_ / path;
            std::error_code ec;
            if (fs::exists(full, ec)) {
                if (fs::is_directory(full, ec)) {
                    if (fs::is_empty(full, ec)) fs::remove(full, ec);
                } else {
                    fs::remove(full, ec);
                }
            }
            pruneEmptyParents(root_, full);
        }

        // 6. Write target files
        for (const auto& [path, content] : loadedBlobs) {
            fs::path full = root_ / path;
            std::error_code ec;
            if (fs::exists(full, ec) && fs::is_directory(full, ec)) {
                if (fs::is_empty(full, ec)) {
                    fs::remove(full, ec);
                } else {
                    throw MiniGitException("cannot restore over non-empty directory: " + path);
                }
            }
            for (fs::path d = full.parent_path(); d != root_ && !d.empty(); d = d.parent_path()) {
                if (fs::exists(d, ec) && !fs::is_directory(d, ec)) {
                    fs::remove(d, ec);
                }
            }
            if (!fs::exists(full, ec)) {
                createdFiles.push_back(path);
            }
            writeFile(full, content);
        }

        index_.replace(target);
        index_.save();
    } catch (...) {
        // Rollback on exception
        std::error_code ec;
        for (const auto& cPath : createdFiles) {
            fs::path full = root_ / cPath;
            if (fs::exists(full, ec)) fs::remove(full, ec);
            pruneEmptyParents(root_, full);
        }
        for (const auto& [bPath, bContent] : backupFiles) {
            fs::path full = root_ / bPath;
            writeFile(full, bContent);
        }
        index_.replace(oldIndex);
        index_.save();
        throw;
    }
}

void Repository::checkout(const std::string& target) {
    if (isMerging()) throw MergeException("merge in progress; commit or abort it first");
    if (isDirty()) throw CheckoutException("you have uncommitted changes; commit them first");
    Hash oldHead = refs_.headCommit();
    bool isBranch = false;
    try {
        if (RefManager::isValidBranchName(target) && refs_.branchExists(target)) {
            isBranch = true;
        }
    } catch (const BranchException&) {
        isBranch = false;
    }
    Hash dest = isBranch ? refs_.branchTip(target) : store_.resolvePrefix(target);
    auto c = store_.loadAs<Commit>(dest);  // validates that dest is a commit
    auto targetTree = store_.loadAs<Tree>(c->tree())->entries();
    checkUntrackedOverwrite(targetTree);
    restoreCommit(dest);
    if (isBranch) refs_.switchToBranch(target);
    else          refs_.detach(dest);
    recordReflog("checkout", oldHead, dest, "Switched to " + target);
}

std::string Repository::merge(const std::string& branch) {
    if (branch == "--abort") {
        return mergeAbort();
    }
    if (!refs_.onBranch()) throw MergeException("cannot merge while HEAD is detached");
    if (!refs_.branchExists(branch)) throw BranchException("no such branch: " + branch);
    if (isDirty()) throw MergeException("you have uncommitted changes; commit them first");

    fs::path mergeHeadFile = gitDir_ / "MERGE_HEAD";
    if (fs::exists(mergeHeadFile)) {
        throw MergeException("a merge is already in progress; commit or abort it first");
    }

    Hash cur = refs_.headCommit(), tgt = refs_.branchTip(branch);
    if (cur.empty()) throw MergeException("current branch has no commits");
    if (tgt.empty()) throw MergeException("branch '" + branch + "' has no commits");
    if (cur == tgt || isAncestor(tgt, cur)) return "Already up to date.";
    if (isAncestor(cur, tgt)) {
        auto targetTree = store_.loadAs<Tree>(store_.loadAs<Commit>(tgt)->tree())->entries();
        checkUntrackedOverwrite(targetTree);
        restoreCommit(tgt);
        refs_.updateHead(tgt);
        recordReflog("merge", cur, tgt, "Merged branch " + branch);
        return "Fast-forward to " + tgt.shortStr();
    }

    // 3-way merge
    Hash baseCommitHash = findMergeBase(cur, tgt);
    std::map<std::string, Hash> baseEntries;
    if (!baseCommitHash.empty() && store_.exists(baseCommitHash)) {
        baseEntries = store_.loadAs<Tree>(store_.loadAs<Commit>(baseCommitHash)->tree())->entries();
    }
    auto oursEntries = store_.loadAs<Tree>(store_.loadAs<Commit>(cur)->tree())->entries();
    auto theirsEntries = store_.loadAs<Tree>(store_.loadAs<Commit>(tgt)->tree())->entries();

    std::set<std::string> allPaths;
    for (const auto& [p, _] : baseEntries) allPaths.insert(p);
    for (const auto& [p, _] : oursEntries) allPaths.insert(p);
    for (const auto& [p, _] : theirsEntries) allPaths.insert(p);

    struct ConflictItem {
        std::string path;
        std::string oursContent;
        std::string theirsContent;
        bool isBinary;
    };

    std::map<std::string, Hash> cleanEntries;
    std::map<std::string, Hash> filesToWriteTheirs;
    std::vector<std::string> filesToDelete;
    std::vector<ConflictItem> conflicts;
    std::vector<std::string> conflictedPaths;

    for (const auto& path : allPaths) {
        Hash bHash = baseEntries.count(path) ? baseEntries[path] : Hash();
        Hash oHash = oursEntries.count(path) ? oursEntries[path] : Hash();
        Hash tHash = theirsEntries.count(path) ? theirsEntries[path] : Hash();

        if (oHash == tHash) {
            // Same in ours and theirs
            if (!oHash.empty()) {
                cleanEntries[path] = oHash;
            }
        } else if (oHash == bHash && tHash != bHash) {
            // Changed only in theirs
            if (!tHash.empty()) {
                cleanEntries[path] = tHash;
                filesToWriteTheirs[path] = tHash;
            } else {
                filesToDelete.push_back(path);
            }
        } else if (tHash == bHash && oHash != bHash) {
            // Changed only in ours
            if (!oHash.empty()) {
                cleanEntries[path] = oHash;
            }
        } else {
            // Changed differently on both sides: conflict
            std::string oursContent = (!oHash.empty() && store_.exists(oHash)) ? store_.loadAs<Blob>(oHash)->content() : "";
            std::string theirsContent = (!tHash.empty() && store_.exists(tHash)) ? store_.loadAs<Blob>(tHash)->content() : "";
            bool isBinary = (oursContent.find('\0') != std::string::npos || theirsContent.find('\0') != std::string::npos);
            conflicts.push_back({path, std::move(oursContent), std::move(theirsContent), isBinary});
            conflictedPaths.push_back(path);
        }
    }

    // Safety: check untracked overwrite before any modifications to disk
    std::map<std::string, Hash> toWriteMerge;
    for (const auto& [p, h] : filesToWriteTheirs) toWriteMerge[p] = h;
    for (const auto& c : conflicts) toWriteMerge[c.path] = Hash();

    checkUntrackedOverwrite(toWriteMerge);

    for (const auto& p : filesToDelete) {
        if (!isSafeRepoPath(p)) throw MiniGitException("refusing operation with unsafe path: " + p);
        ensureNoSymlinkComponents(root_, p);
        fs::path full = root_ / p;
        std::error_code ec;
        if (fs::exists(full, ec) && fs::is_directory(full, ec) && !fs::is_empty(full, ec)) {
            throw MiniGitException("refusing to remove non-empty directory at tracked path: " + p);
        }
    }
    for (const auto& [p, _] : toWriteMerge) {
        if (!isSafeRepoPath(p)) throw MiniGitException("refusing operation with unsafe path: " + p);
        ensureNoSymlinkComponents(root_, p);
        fs::path full = root_ / p;
        std::error_code ec;
        if (fs::exists(full, ec) && fs::is_directory(full, ec) && !fs::is_empty(full, ec)) {
            throw MiniGitException("cannot restore over non-empty directory: " + p);
        }
    }

    std::map<std::string, std::string> loadedTheirsBlobs;
    for (const auto& [p, h] : filesToWriteTheirs) {
        auto blob = store_.loadAs<Blob>(h);
        if (!blob) throw MiniGitException("missing blob object for path: " + p);
        loadedTheirsBlobs[p] = blob->content();
    }

    if (!conflicts.empty()) {
        auto oldIndex = index_.entries();
        std::map<std::string, std::string> backupFiles;
        std::vector<std::string> createdFiles;

        for (const auto& [p, _] : oldIndex) {
            fs::path full = root_ / p;
            if (fs::is_regular_file(full)) backupFiles[p] = readFile(full);
        }
        for (const auto& [p, _] : toWriteMerge) {
            if (!backupFiles.count(p)) {
                fs::path full = root_ / p;
                if (fs::is_regular_file(full)) backupFiles[p] = readFile(full);
            }
        }

        try {
            // Apply non-conflicted updates
            for (const auto& p : filesToDelete) {
                fs::path full = root_ / p;
                std::error_code ec;
                if (fs::exists(full, ec)) {
                    if (fs::is_directory(full, ec)) {
                        if (fs::is_empty(full, ec)) fs::remove(full, ec);
                    } else {
                        fs::remove(full, ec);
                    }
                }
                pruneEmptyParents(root_, full);
                index_.remove(p);
            }
            for (const auto& [p, content] : loadedTheirsBlobs) {
                fs::path full = root_ / p;
                std::error_code ec;
                if (!fs::exists(full, ec)) createdFiles.push_back(p);
                writeFile(full, content);
                index_.set(p, filesToWriteTheirs[p]);
            }

            // Apply conflict markers or keep ours for binary
            for (const auto& c : conflicts) {
                fs::path full = root_ / c.path;
                std::error_code ec;
                if (!fs::exists(full, ec)) createdFiles.push_back(c.path);
                if (c.isBinary) {
                    if (!c.oursContent.empty()) {
                        writeFile(full, c.oursContent);
                    }
                } else {
                    std::string text = "<<<<<<< ours\n";
                    text += c.oursContent;
                    if (!c.oursContent.empty() && c.oursContent.back() != '\n') text += "\n";
                    text += "=======\n";
                    text += c.theirsContent;
                    if (!c.theirsContent.empty() && c.theirsContent.back() != '\n') text += "\n";
                    text += ">>>>>>> " + branch + "\n";
                    writeFile(full, text);
                }
            }
            index_.save();
            writeFile(mergeHeadFile, tgt.str() + "\n");

            std::sort(conflictedPaths.begin(), conflictedPaths.end());
            std::ostringstream cStream;
            for (const auto& cp : conflictedPaths) cStream << cp << "\n";
            writeFile(gitDir_ / "MERGE_CONFLICTS", cStream.str());
            writeFile(gitDir_ / "MERGE_CONFLICTS_ALL", cStream.str());

            std::string listStr;
            for (size_t i = 0; i < conflictedPaths.size(); ++i) {
                if (i > 0) listStr += ", ";
                listStr += conflictedPaths[i];
            }
            throw MergeException("Automatic merge failed; fix conflicts and then commit the result. Conflicted files: " + listStr);
        } catch (const MergeException&) {
            // Expected MergeException for conflict reporting, rethrow directly
            throw;
        } catch (...) {
            // Rollback on unexpected exception
            std::error_code ec;
            fs::remove(mergeHeadFile, ec);
            fs::remove(gitDir_ / "MERGE_CONFLICTS", ec);
            fs::remove(gitDir_ / "MERGE_CONFLICTS_ALL", ec);
            for (const auto& cPath : createdFiles) {
                fs::path full = root_ / cPath;
                if (fs::exists(full, ec)) fs::remove(full, ec);
                pruneEmptyParents(root_, full);
            }
            for (const auto& [bPath, bContent] : backupFiles) {
                fs::path full = root_ / bPath;
                writeFile(full, bContent);
            }
            index_.replace(oldIndex);
            index_.save();
            throw;
        }
    }

    // No conflicts: write working tree & index, create 2-parent merge commit
    for (const auto& p : filesToDelete) {
        fs::path full = root_ / p;
        std::error_code ec;
        if (fs::exists(full, ec)) {
            if (fs::is_directory(full, ec)) {
                if (fs::is_empty(full, ec)) fs::remove(full, ec);
            } else {
                fs::remove(full, ec);
            }
        }
        for (fs::path d = full.parent_path(); d != root_ && !d.empty() && fs::is_directory(d, ec) && fs::is_empty(d, ec); d = d.parent_path())
            fs::remove(d, ec);
    }
    for (const auto& [p, content] : loadedTheirsBlobs) {
        writeFile(root_ / p, content);
    }

    index_.replace(cleanEntries);
    index_.save();

    Tree resultTree;
    for (const auto& [p, h] : cleanEntries) {
        resultTree.set(p, h);
    }
    store_.save(resultTree);
    Hash mergeCommit = store_.save(Commit(resultTree.hash(), cur, author(), "Merge branch '" + branch + "'", std::time(nullptr), tgt));
    refs_.updateHead(mergeCommit);
    recordReflog("merge", cur, mergeCommit, "Merge branch '" + branch + "'");
    return "Merged " + branch + " (3-way)";
}

Snapshot Repository::snapshot() const {
    Snapshot s;
    s.currentBranch = refs_.currentBranch();
    s.head = refs_.headCommit();
    for (const auto& b : refs_.listBranches()) s.branches[b] = refs_.branchTip(b);
    for (const auto& h : store_.listAll()) s.objects.push_back(store_.load(h));

    std::set<std::string> binFiles;
    for (const auto& f : workingFiles()) {
        std::string content = readFile(root_ / f);
        if (content.find('\0') != std::string::npos) {
            binFiles.insert(f);
            continue;  // skip binary files
        }
        s.files[f] = std::move(content);
    }

    auto head = headTree();
    for (const auto& [path, h] : index_.entries()) {
        if (binFiles.count(path)) continue;
        if (store_.exists(h)) {
            auto b = store_.loadAs<Blob>(h);
            if (b && b->content().find('\0') != std::string::npos) {
                binFiles.insert(path);
            }
        }
    }
    for (const auto& [path, h] : head) {
        if (binFiles.count(path)) continue;
        if (store_.exists(h)) {
            auto b = store_.loadAs<Blob>(h);
            if (b && b->content().find('\0') != std::string::npos) {
                binFiles.insert(path);
            }
        }
    }
    s.binaryFiles.assign(binFiles.begin(), binFiles.end());
    for (const auto& [path, h] : index_.entries()) {
        auto it = head.find(path);
        if (it == head.end() || it->second != h) s.staged[path] = h;
    }
    for (const auto& [path, h] : head)
        if (!index_.has(path)) s.stagedDeleted.push_back(path);
    s.status = status();
    s.detached = !refs_.onBranch();
    s.merging = isMerging();
    s.conflicts = conflictedPaths();

    for (const auto& [path, h] : index_.entries()) {
        auto it = head.find(path);
        if (it == head.end()) {
            s.stagedStatus[path] = 'A';
        } else if (it->second != h) {
            s.stagedStatus[path] = 'M';
        }
    }
    for (const auto& [path, h] : head) {
        if (!index_.has(path)) {
            s.stagedStatus[path] = 'D';
        }
    }

    return s;
}

static std::string escapeReflogField(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '|') out += "\\|";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

static std::string unescapeReflogField(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char next = s[++i];
            if (next == 'n') out += '\n';
            else if (next == 'r') out += '\r';
            else if (next == '|') out += '|';
            else if (next == '\\') out += '\\';
            else { out += '\\'; out += next; }
        } else {
            out += s[i];
        }
    }
    return out;
}

void Repository::recordReflog(const std::string& action, const Hash& oldH, const Hash& newH, const std::string& desc) {
    try {
        std::ofstream ofs(gitDir_ / "reflog", std::ios::app);
        if (ofs.is_open()) {
            ofs << isoTime(std::time(nullptr)) << "|"
                << escapeReflogField(action) << "|"
                << (oldH.empty() ? "0000000" : oldH.str()) << "|"
                << (newH.empty() ? "0000000" : newH.str()) << "|"
                << escapeReflogField(desc) << "\n";
        }
    } catch (...) {}
}

std::vector<ReflogEntry> Repository::reflog() const {
    std::vector<ReflogEntry> entries;
    fs::path p = gitDir_ / "reflog";
    if (fs::exists(p)) {
        std::ifstream ifs(p);
        std::string line;
        while (std::getline(ifs, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (line.empty()) continue;
            std::vector<std::string> parts;
            std::string current;
            for (size_t i = 0; i < line.size(); ++i) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    current += line[i];
                    current += line[++i];
                } else if (line[i] == '|') {
                    parts.push_back(unescapeReflogField(current));
                    current.clear();
                } else {
                    current += line[i];
                }
            }
            parts.push_back(unescapeReflogField(current));

            if (parts.size() >= 5) {
                entries.push_back({parts[0], parts[1], parts[2], parts[3], parts[4]});
            }
        }
    }
    if (entries.empty()) {
        for (const auto& c : log()) {
            entries.push_back({
                isoTime(c.time()),
                "commit",
                (c.parent().empty() ? "0000000" : c.parent().str()),
                c.hash().str(),
                c.message()
            });
        }
    }
    std::reverse(entries.begin(), entries.end());
    return entries;
}

std::string Repository::undo() {
    if (isMerging()) throw MergeException("merge in progress; commit or abort it first");
    Hash cur = refs_.headCommit();
    if (cur.empty()) throw MiniGitException("nothing to undo (repository has no commits)");
    auto c = store_.loadAs<Commit>(cur);
    Hash parent = c->parent();
    if (parent.empty()) {
        throw MiniGitException("cannot undo the initial commit");
    }

    if (refs_.onBranch()) {
        refs_.updateHead(parent);
    } else {
        refs_.detach(parent);
    }

    recordReflog("undo", cur, parent, "Undid commit: " + c->message());
    return "Undid commit [" + cur.shortStr() + "]. Your changes are preserved in the working directory.";
}

bool Repository::hasStash() const {
    return fs::exists(gitDir_ / "stash") && !stashList().empty();
}

std::string Repository::stashSave(const std::string& message, bool includeUntracked) {
    if (isMerging()) throw MergeException("merge in progress; commit or abort it first");
    if (head().empty()) {
        throw MiniGitException("cannot stash before the first commit");
    }
    Status s = status();
    bool hasTrackedChanges = !(s.staged.empty() && s.modified.empty() && s.deleted.empty());
    if (!hasTrackedChanges && (!includeUntracked || s.untracked.empty())) {
        return "no local changes to save";
    }

    Tree t;
    std::set<std::string> trackedFiles;
    for (const auto& [p, h] : index_.entries()) trackedFiles.insert(p);
    for (const auto& [p, h] : headTree()) trackedFiles.insert(p);

    for (const auto& f : trackedFiles) {
        if (fs::exists(root_ / f)) {
            t.set(f, store_.save(Blob(readFile(root_ / f))));
        }
    }
    if (includeUntracked) {
        for (const auto& u : s.untracked) {
            if (fs::exists(root_ / u)) {
                t.set(u, store_.save(Blob(readFile(root_ / u))));
            }
        }
    }
    store_.save(t);

    Tree indexTree;
    for (const auto& [p, h] : index_.entries()) indexTree.set(p, h);
    Hash indexTreeHash = store_.save(indexTree);

    std::string desc = message.empty() ? ("WIP on " + (currentBranch().empty() ? "detached" : currentBranch())) : message;
    Hash h = store_.save(Commit(t.hash(), head(), author(), desc, std::time(nullptr)));

    {
        std::ofstream ofs(gitDir_ / "stash", std::ios::app);
        if (ofs.is_open()) {
            ofs << h.str() << " " << indexTreeHash.str() << " " << escapeReflogField(desc) << "\n";
        }
    }

    restoreCommit(head());
    if (includeUntracked) {
        for (const auto& u : s.untracked) {
            if (fs::exists(root_ / u)) fs::remove(root_ / u);
        }
    }

    recordReflog("stash", head(), h, "Saved stash: " + desc);
    return "Saved stash [" + h.shortStr() + "]: " + desc;
}

std::string Repository::stashPop() {
    if (isMerging()) {
        throw MergeException("merge in progress; commit or abort it first");
    }
    auto list = stashList();
    if (list.empty()) throw MiniGitException("no stash entries found to pop");

    if (isDirty()) {
        throw MiniGitException("cannot pop stash: you have uncommitted changes");
    }

    StashEntry entry = list.front();

    auto c = store_.loadAs<Commit>(entry.commitHash);
    auto t = store_.loadAs<Tree>(c->tree());
    const auto& stashTree = t->entries();

    Hash baseHash = c->parent();
    std::map<std::string, Hash> baseTree;
    if (!baseHash.empty() && store_.exists(baseHash)) {
        baseTree = store_.loadAs<Tree>(store_.loadAs<Commit>(baseHash)->tree())->entries();
    }

    auto curHeadTree = headTree();

    std::set<std::string> allPaths;
    for (const auto& [p, _] : baseTree) allPaths.insert(p);
    for (const auto& [p, _] : stashTree) allPaths.insert(p);
    for (const auto& [p, _] : curHeadTree) allPaths.insert(p);

    std::vector<std::string> stashAdded;
    std::vector<std::string> stashDeleted;
    std::vector<std::string> stashModified;

    for (const auto& p : allPaths) {
        Hash bHash = baseTree.count(p) ? baseTree.at(p) : Hash();
        Hash sHash = stashTree.count(p) ? stashTree.at(p) : Hash();
        Hash cHash = curHeadTree.count(p) ? curHeadTree.at(p) : Hash();

        bool changedInStash = (sHash != bHash);
        bool changedInHead = (cHash != bHash);

        if (changedInStash && changedInHead) {
            throw MiniGitException("conflict: path '" + p + "' changed in both stash and current HEAD");
        }

        if (changedInStash) {
            if (bHash.empty() && !sHash.empty()) {
                stashAdded.push_back(p);
            } else if (!bHash.empty() && sHash.empty()) {
                stashDeleted.push_back(p);
            } else {
                stashModified.push_back(p);
            }
        }
    }

    // Check untracked files before touching disk
    std::map<std::string, Hash> toWriteStash;
    for (const auto& p : stashAdded) toWriteStash[p] = stashTree.at(p);
    for (const auto& p : stashModified) toWriteStash[p] = stashTree.at(p);

    checkUntrackedOverwrite(toWriteStash);

    for (const auto& p : stashDeleted) {
        if (!isSafeRepoPath(p)) throw MiniGitException("refusing operation with unsafe path: " + p);
        ensureNoSymlinkComponents(root_, p);
        fs::path full = root_ / p;
        std::error_code ec;
        if (fs::exists(full, ec) && fs::is_directory(full, ec) && !fs::is_empty(full, ec)) {
            throw MiniGitException("refusing to remove non-empty directory at tracked path: " + p);
        }
    }
    for (const auto& [p, _] : toWriteStash) {
        if (!isSafeRepoPath(p)) throw MiniGitException("refusing operation with unsafe path: " + p);
        ensureNoSymlinkComponents(root_, p);
        fs::path full = root_ / p;
        std::error_code ec;
        if (fs::exists(full, ec) && fs::is_directory(full, ec) && !fs::is_empty(full, ec)) {
            throw MiniGitException("cannot restore over non-empty directory: " + p);
        }
    }

    std::map<std::string, std::string> loadedStashBlobs;
    for (const auto& [p, h] : toWriteStash) {
        auto blob = store_.loadAs<Blob>(h);
        if (!blob) throw MiniGitException("missing blob object for path: " + p);
        loadedStashBlobs[p] = blob->content();
    }

    // Apply stash's delta to working tree on disk
    for (const auto& p : stashDeleted) {
        fs::path full = root_ / p;
        std::error_code ec;
        if (fs::exists(full, ec)) {
            if (fs::is_directory(full, ec)) {
                if (fs::is_empty(full, ec)) fs::remove(full, ec);
            } else {
                fs::remove(full, ec);
            }
        }
        for (fs::path d = full.parent_path(); d != root_ && !d.empty() && fs::is_directory(d, ec) && fs::is_empty(d, ec); d = d.parent_path()) {
            fs::remove(d, ec);
        }
    }
    for (const auto& [p, content] : loadedStashBlobs) {
        writeFile(root_ / p, content);
    }

    // Update index
    if (this->head() == baseHash) {
        if (!entry.indexTreeHash.empty() && store_.exists(entry.indexTreeHash)) {
            auto idxTree = store_.loadAs<Tree>(entry.indexTreeHash);
            index_.replace(idxTree->entries());
        } else {
            index_.replace(curHeadTree);
        }
    } else {
        if (!entry.indexTreeHash.empty() && store_.exists(entry.indexTreeHash)) {
            auto stagedTree = store_.loadAs<Tree>(entry.indexTreeHash)->entries();
            for (const auto& [p, h] : stagedTree) {
                if (!baseTree.count(p) || baseTree.at(p) != h) {
                    index_.set(p, h);
                }
            }
            for (const auto& [p, h] : baseTree) {
                if (!stagedTree.count(p)) {
                    index_.remove(p);
                }
            }
        }
    }
    index_.save();

    std::ifstream ifs(gitDir_ / "stash");
    std::vector<std::string> allLines;
    std::string l;
    while (std::getline(ifs, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty()) allLines.push_back(l);
    }
    ifs.close();

    if (!allLines.empty()) allLines.pop_back();

    if (allLines.empty()) {
        fs::remove(gitDir_ / "stash");
    } else {
        std::ofstream ofs(gitDir_ / "stash", std::ios::trunc);
        for (const auto& line : allLines) ofs << line << "\n";
    }

    recordReflog("stash_pop", entry.commitHash, this->head(), "Popped stash [" + entry.commitHash.shortStr() + "]");
    return "Restored and applied stash [" + entry.commitHash.shortStr() + "]";
}

std::vector<StashEntry> Repository::stashList() const {
    std::vector<StashEntry> entries;
    fs::path p = gitDir_ / "stash";
    if (!fs::exists(p)) return entries;

    std::ifstream ifs(p);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(ifs, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }

    std::reverse(lines.begin(), lines.end());
    for (size_t i = 0; i < lines.size(); ++i) {
        std::stringstream ss(lines[i]);
        std::string hStr, idxHStr, descEsc;
        ss >> hStr >> idxHStr;
        std::getline(ss, descEsc);
        while (!descEsc.empty() && descEsc.front() == ' ') descEsc.erase(descEsc.begin());

        Hash h(hStr);
        Hash idxH(idxHStr);
        std::string desc = unescapeReflogField(descEsc);
        std::string ts;
        if (store_.exists(h)) {
            try {
                auto c = store_.loadAs<Commit>(h);
                ts = isoTime(c->time());
                if (desc.empty()) desc = c->message();
            } catch (...) {}
        }
        entries.push_back({i, h, idxH, desc, ts});
    }
    return entries;
}

std::string Repository::exportBundleJson() const {
    return HtmlExporter::toJson(snapshot(), root_.filename().string(), true);
}

static std::time_t parseIsoTimestamp(const std::string& iso) {
    if (iso.size() < 19) return 0;
    try {
        std::tm tm{};
        tm.tm_year = std::stoi(iso.substr(0, 4)) - 1900;
        tm.tm_mon  = std::stoi(iso.substr(5, 2)) - 1;
        tm.tm_mday = std::stoi(iso.substr(8, 2));
        tm.tm_hour = std::stoi(iso.substr(11, 2));
        tm.tm_min  = std::stoi(iso.substr(14, 2));
        tm.tm_sec  = std::stoi(iso.substr(17, 2));
#if defined(_WIN32)
        return _mkgmtime(&tm);
#else
        return timegm(&tm);
#endif
    } catch (...) {
        return 0;
    }
}

std::string Repository::importBundleJson(const std::string& bundleJson) {
    std::string parseErr;
    JsonValue root = JsonValue::parse(bundleJson, &parseErr);
    if (!root.is_object()) {
        std::map<std::string, JsonValue> errRes;
        errRes["ok"] = JsonValue(false);
        errRes["error"] = JsonValue("Malformed JSON bundle: " + (parseErr.empty() ? "expected JSON object" : parseErr));
        errRes["objectsImported"] = JsonValue(0);
        errRes["objectsPresent"] = JsonValue(0);
        errRes["objectsRejected"] = JsonValue(0);
        return JsonValue(errRes).serialize();
    }

    if (!root.contains("objects") || !root["objects"].is_object()) {
        std::map<std::string, JsonValue> errRes;
        errRes["ok"] = JsonValue(false);
        errRes["error"] = JsonValue("Invalid bundle: missing 'objects' dictionary");
        errRes["objectsImported"] = JsonValue(0);
        errRes["objectsPresent"] = JsonValue(0);
        errRes["objectsRejected"] = JsonValue(0);
        return JsonValue(errRes).serialize();
    }

    struct BlobItem { Hash hash; std::string content; };
    struct TreeItem { Hash hash; Tree tree; };
    struct CommitItem { Hash hash; Commit commit; };

    std::vector<BlobItem> verifiedBlobs;
    std::vector<TreeItem> verifiedTrees;
    std::vector<CommitItem> verifiedCommits;

    int objectsRejected = 0;
    const auto& objs = root["objects"].as_object();
    for (const auto& [hashStr, objVal] : objs) {
        if (hashStr.size() != 40 || !objVal.is_object()) {
            objectsRejected++;
            continue;
        }
        Hash expectedHash(hashStr);
        std::string type = objVal["type"].as_string();

        if (type == "blob") {
            std::string content;
            if (objVal.contains("encoding") && objVal["encoding"].as_string() == "base64") {
                std::string decErr;
                if (!Base64::decode(objVal["content"].as_string(), content, decErr)) {
                    objectsRejected++;
                    continue;
                }
            } else {
                content = objVal["content"].as_string();
            }
            try {
                Blob b(content);
                if (b.hash() != expectedHash) {
                    objectsRejected++;
                } else {
                    verifiedBlobs.push_back({expectedHash, std::move(content)});
                }
            } catch (...) {
                objectsRejected++;
            }
        } else if (type == "tree") {
            Tree t;
            if (objVal.contains("data") && objVal["data"].is_object()) {
                for (const auto& [p, hVal] : objVal["data"].as_object()) {
                    if (!isSafeRepoPath(p)) {
                        std::map<std::string, JsonValue> errRes;
                        errRes["ok"] = JsonValue(false);
                        errRes["error"] = JsonValue("Unsafe path in bundle tree: " + p);
                        errRes["objectsImported"] = JsonValue(0);
                        errRes["objectsPresent"] = JsonValue(0);
                        errRes["objectsRejected"] = JsonValue(0);
                        return JsonValue(errRes).serialize();
                    }
                    t.set(p, Hash(hVal.as_string()));
                }
            }
            try {
                Tree parsedTree = Tree::parse(t.serialize());
                if (parsedTree.hash() != expectedHash) {
                    objectsRejected++;
                } else {
                    verifiedTrees.push_back({expectedHash, std::move(parsedTree)});
                }
            } catch (...) {
                objectsRejected++;
            }
        } else if (type == "commit") {
            if (objVal.contains("data") && objVal["data"].is_object()) {
                const auto& d = objVal["data"];
                std::string treeStr = d["tree"].as_string();
                std::string parentStr = (d.contains("parent") && !d["parent"].is_null()) ? d["parent"].as_string() : "";
                std::string authorStr = d["author"].as_string();
                std::string messageStr = d["message"].as_string();
                bool badAuthor = false;
                for (char ch : authorStr) {
                    unsigned char uc = static_cast<unsigned char>(ch);
                    if (uc < 32 || uc == 127) {
                        badAuthor = true;
                        break;
                    }
                }
                if (badAuthor) {
                    objectsRejected++;
                    continue;
                }
                std::time_t t = 0;
                if (d.contains("time") && d["time"].is_number()) {
                    t = static_cast<std::time_t>(d["time"].as_int64());
                } else if (d.contains("timestamp") && d["timestamp"].is_string()) {
                    t = parseIsoTimestamp(d["timestamp"].as_string());
                }
                std::string parent2Str = (d.contains("parent2") && !d["parent2"].is_null()) ? d["parent2"].as_string() : "";
                try {
                    Commit c(Hash(treeStr), Hash(parentStr), authorStr, messageStr, t, Hash(parent2Str));
                    Commit parsedCommit = Commit::parse(c.serialize());
                    if (parsedCommit.hash() != expectedHash) {
                        objectsRejected++;
                    } else {
                        verifiedCommits.push_back({expectedHash, std::move(parsedCommit)});
                    }
                } catch (...) {
                    objectsRejected++;
                }
            } else {
                objectsRejected++;
            }
        } else {
            objectsRejected++;
        }
    }

    if (objectsRejected > 0) {
        std::map<std::string, JsonValue> errRes;
        errRes["ok"] = JsonValue(false);
        errRes["error"] = JsonValue("Hash verification failed: " + std::to_string(objectsRejected) + " object(s) rejected; refs untouched.");
        errRes["objectsImported"] = JsonValue(0);
        errRes["objectsPresent"] = JsonValue(0);
        errRes["objectsRejected"] = JsonValue(objectsRejected);
        return JsonValue(errRes).serialize();
    }

    // Save in dependency order: Blobs, Trees, Commits
    int objectsImported = 0;
    int objectsPresent = 0;

    for (const auto& item : verifiedBlobs) {
        if (store_.exists(item.hash)) {
            objectsPresent++;
        } else {
            store_.save(Blob(item.content));
            objectsImported++;
        }
    }

    for (const auto& item : verifiedTrees) {
        if (store_.exists(item.hash)) {
            objectsPresent++;
        } else {
            store_.save(item.tree);
            objectsImported++;
        }
    }

    for (const auto& item : verifiedCommits) {
        if (store_.exists(item.hash)) {
            objectsPresent++;
        } else {
            store_.save(item.commit);
            objectsImported++;
        }
    }

    // Before creating or fast-forwarding any branch, verify the tip commit, its tree, all blobs and parents exist; otherwise reject the whole bundle
    bool bundleInvalid = false;
    std::string bundleInvalidReason;
    if (root.contains("branches") && root["branches"].is_object()) {
        for (const auto& [bName, tipVal] : root["branches"].as_object()) {
            if (!RefManager::isValidBranchName(bName) || bName.rfind("peer-", 0) == 0) {
                continue;
            }
            if (tipVal.is_null() || !tipVal.is_string() || tipVal.as_string().size() != 40) {
                continue;
            }
            Hash target(tipVal.as_string());
            if (!store_.exists(target)) {
                bundleInvalid = true;
                bundleInvalidReason = "Missing tip commit " + target.shortStr() + " for branch '" + bName + "'";
                break;
            }

            try {
                auto commit = store_.loadAs<Commit>(target);
                std::set<Hash> visited;
                auto verifyCommitTree = [&](auto self, const Hash& ch) -> bool {
                    if (ch.empty() || ch.str() == "-") return true;
                    if (visited.count(ch)) return true;
                    visited.insert(ch);
                    if (!store_.exists(ch)) return false;
                    try {
                        auto c = store_.loadAs<Commit>(ch);
                        if (!store_.exists(c->tree())) return false;
                        auto tree = store_.loadAs<Tree>(c->tree());
                        for (const auto& [path, bHash] : tree->entries()) {
                            if (!store_.exists(bHash)) return false;
                        }
                        if (!c->parent().empty() && c->parent().str() != "-") {
                            if (!self(self, c->parent())) return false;
                        }
                        if (c->hasParent2() && !c->parent2().empty()) {
                            if (!self(self, c->parent2())) return false;
                        }
                    } catch (...) {
                        return false;
                    }
                    return true;
                };

                if (!verifyCommitTree(verifyCommitTree, target)) {
                    bundleInvalid = true;
                    bundleInvalidReason = "Missing object (commit, tree, blob, or parent) for branch '" + bName + "' with tip " + target.shortStr();
                    break;
                }
            } catch (const InvalidObjectException&) {
                // Non-commit tip (handled by branchesSkippedInvalid with warning in branch loop)
                continue;
            } catch (...) {
                continue;
            }
        }
    }

    if (bundleInvalid) {
        std::map<std::string, JsonValue> errRes;
        errRes["ok"] = JsonValue(false);
        errRes["error"] = JsonValue("Bundle rejected: " + bundleInvalidReason + "; refs untouched.");
        errRes["objectsImported"] = JsonValue(objectsImported);
        errRes["objectsPresent"] = JsonValue(objectsPresent);
        errRes["objectsRejected"] = JsonValue(objectsRejected);
        return JsonValue(errRes).serialize();
    }

    // Process Branches
    std::vector<std::string> warnings;
    std::vector<std::string> branchesCreated;
    std::vector<std::string> branchesFastForwarded;
    std::vector<std::string> branchesSkippedDiverged;
    std::vector<std::string> branchesUpToDate;
    std::vector<std::string> branchesSkippedInvalid;
    std::vector<std::string> peerRefs;
    bool activeBranchDirtyRefused = false;
    std::string refuseReason;

    if (root.contains("branches") && root["branches"].is_object()) {
        for (const auto& [bName, tipVal] : root["branches"].as_object()) {
            if (!RefManager::isValidBranchName(bName) || bName.rfind("peer-", 0) == 0) {
                branchesSkippedInvalid.push_back(bName);
                continue;
            }
            if (tipVal.is_null() || !tipVal.is_string()) continue;
            std::string hStr = tipVal.as_string();
            if (hStr.size() != 40) continue;
            Hash target(hStr);
            if (!store_.exists(target)) continue;

            try {
                auto cCheck = store_.loadAs<Commit>(target);
            } catch (const InvalidObjectException& e) {
                std::string warnMsg = "Branch '" + bName + "' tip " + target.shortStr() + " does not point to a commit (" + e.what() + "); skipped.";
                warnings.push_back(warnMsg);
                branchesSkippedInvalid.push_back(bName);
                continue;
            } catch (...) {
                std::string warnMsg = "Branch '" + bName + "' tip " + target.shortStr() + " is invalid; skipped.";
                warnings.push_back(warnMsg);
                branchesSkippedInvalid.push_back(bName);
                continue;
            }

            try {
                if (!refs_.branchExists(bName)) {
                    if (refs_.currentBranch() == bName) {
                        auto targetTree = store_.loadAs<Tree>(store_.loadAs<Commit>(target)->tree())->entries();
                        checkUntrackedOverwrite(targetTree);
                        restoreCommit(target);
                    }
                    refs_.createBranch(bName, target);
                    branchesCreated.push_back(bName);
                    recordReflog("p2p_sync", Hash(), target, "P2P LAN Sync: created branch " + bName + " at " + target.shortStr());
                } else {
                    Hash curTip = refs_.branchTip(bName);
                    if (curTip == target) {
                        branchesUpToDate.push_back(bName);
                    } else if (isAncestor(target, curTip)) {
                        branchesUpToDate.push_back(bName + " (local ahead)");
                    } else if (isAncestor(curTip, target)) {
                        // Fast-forward candidate
                        if (refs_.currentBranch() == bName) {
                            if (isDirty()) {
                                activeBranchDirtyRefused = true;
                                refuseReason = "Cannot fast-forward active branch '" + bName + "': working tree or index has uncommitted changes.";
                                continue;
                            }
                            auto targetTree = store_.loadAs<Tree>(store_.loadAs<Commit>(target)->tree())->entries();
                            checkUntrackedOverwrite(targetTree);
                            restoreCommit(target);
                            refs_.updateHead(target);
                            recordReflog("p2p_sync", curTip, target, "P2P LAN Sync: fast-forward " + bName + " to " + target.shortStr());
                            branchesFastForwarded.push_back(bName);
                        } else {
                            refs_.setBranch(bName, target);
                            recordReflog("p2p_sync", curTip, target, "P2P LAN Sync: fast-forward " + bName + " to " + target.shortStr());
                            branchesFastForwarded.push_back(bName);
                        }
                    } else {
                        // Diverged
                        std::string peerRef = "peer-" + bName;
                        refs_.setBranch(peerRef, target);
                        recordReflog("p2p_sync", Hash(), target, "P2P LAN Sync: saved diverged peer tip for " + bName + " as " + peerRef);
                        peerRefs.push_back(peerRef);
                        branchesSkippedDiverged.push_back(bName);
                    }
                }
            } catch (const MiniGitException& e) {
                if (refs_.currentBranch() == bName) {
                    activeBranchDirtyRefused = true;
                    refuseReason = e.what();
                }
            }
        }
    }

    bool ok = true;
    std::string message;

    if (activeBranchDirtyRefused) {
        ok = false;
        message = refuseReason;
    } else if (!branchesSkippedDiverged.empty() && branchesCreated.empty() && branchesFastForwarded.empty()) {
        ok = false;
        if (branchesSkippedDiverged.size() == 1) {
            std::string b = branchesSkippedDiverged[0];
            message = b + " diverged; your tip is unchanged. Peer's commits saved as peer-" + b + ". To combine them, run: minigit merge peer-" + b;
        } else {
            std::string divList;
            for (size_t i = 0; i < branchesSkippedDiverged.size(); ++i) {
                if (i > 0) divList += ", ";
                divList += branchesSkippedDiverged[i];
            }
            message = "Branch(es) diverged (" + divList + "), merge needed; tip unchanged.";
        }
    } else if (objectsImported == 0 && objectsPresent == 0 && branchesCreated.empty() && branchesFastForwarded.empty() && branchesUpToDate.empty()) {
        ok = false;
        message = "No objects or branches could be imported from bundle.";
    } else {
        ok = true;
        message = "P2P LAN Sync complete: " + std::to_string(objectsImported) + " object(s) imported, " +
                  std::to_string(objectsPresent) + " already present.";
        if (!branchesCreated.empty()) {
            message += " Created branch: ";
            for (size_t i = 0; i < branchesCreated.size(); ++i) {
                if (i > 0) message += ", ";
                message += branchesCreated[i];
            }
            message += ".";
        }
        if (!branchesFastForwarded.empty()) {
            message += " Fast-forwarded: ";
            for (size_t i = 0; i < branchesFastForwarded.size(); ++i) {
                if (i > 0) message += ", ";
                message += branchesFastForwarded[i];
            }
            message += ".";
        }
        if (!branchesSkippedDiverged.empty()) {
            message += " Skipped diverged: ";
            for (size_t i = 0; i < branchesSkippedDiverged.size(); ++i) {
                if (i > 0) message += ", ";
                message += branchesSkippedDiverged[i];
            }
            message += " (merge needed).";
        }
    }

    std::map<std::string, JsonValue> resObj;
    resObj["ok"] = JsonValue(ok);
    resObj["message"] = JsonValue(message);
    if (!ok) resObj["error"] = JsonValue(message);
    resObj["objectsImported"] = JsonValue(objectsImported);
    resObj["objectsPresent"] = JsonValue(objectsPresent);
    resObj["objectsRejected"] = JsonValue(objectsRejected);

    auto makeJsonArr = [](const std::vector<std::string>& vec) {
        std::vector<JsonValue> arr;
        for (const auto& s : vec) arr.push_back(JsonValue(s));
        return JsonValue(std::move(arr));
    };

    resObj["branchesCreated"] = makeJsonArr(branchesCreated);
    resObj["branchesFastForwarded"] = makeJsonArr(branchesFastForwarded);
    resObj["branchesDiverged"] = makeJsonArr(branchesSkippedDiverged);
    resObj["branchesUpToDate"] = makeJsonArr(branchesUpToDate);
    resObj["branchesSkippedInvalid"] = makeJsonArr(branchesSkippedInvalid);
    resObj["warnings"] = makeJsonArr(warnings);
    resObj["peerRefs"] = makeJsonArr(peerRefs);

    return JsonValue(resObj).serialize();
}

std::string Repository::gitStatus() const {
    bool hasGit = fs::exists(root_ / ".git");
    return hasGit ? "{\"gitExists\":true}" : "{\"gitExists\":false}";
}

std::string Repository::exportToGit() {
    int retCheck = runProcess({"git", "--version"});
    if (retCheck != 0) {
        throw MiniGitException("git is not installed or not available in PATH");
    }

    int retIdent = runProcess({"git", "var", "GIT_AUTHOR_IDENT"});
    if (retIdent != 0) {
        throw MiniGitException("git user.name and user.email are not configured");
    }

    std::string headMsg = "MiniGit snapshot sync";
    if (!head().empty()) {
        try {
            headMsg = store_.loadAs<Commit>(head())->message();
        } catch (...) {}
    }

    std::error_code ec;
    fs::path tempDir = fs::temp_directory_path(ec);
    if (ec) tempDir = root_;
    fs::path tmpMsgFile;
#ifndef _WIN32
    std::string tmpl = (tempDir / "mg_exp_XXXXXX").string();
    std::vector<char> tBuf(tmpl.begin(), tmpl.end());
    tBuf.push_back('\0');
    int fd = mkstemp(tBuf.data());
    if (fd >= 0) {
        tmpMsgFile = tBuf.data();
        close(fd);
    } else {
        tmpMsgFile = tempDir / ("mg_exp_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(std::rand()) + ".txt");
    }
#else
    char tBuf[MAX_PATH];
    char fBuf[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tBuf) && GetTempFileNameA(tBuf, "mg_", 0, fBuf)) {
        tmpMsgFile = fBuf;
    } else {
        tmpMsgFile = tempDir / ("mg_exp_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(std::rand()) + ".txt");
    }
#endif
    writeFile(tmpMsgFile, headMsg);

    if (!fs::exists(root_ / ".git")) {
        int retInit = runProcess({"git", "-C", root_.string(), "init"});
        if (retInit != 0) {
            fs::remove(tmpMsgFile, ec);
            throw MiniGitException("git init failed with exit code " + std::to_string(retInit));
        }
    }

    // Exclude .minigit via .git/info/exclude so no .gitignore is created or tracked
    if (fs::exists(root_ / ".git")) {
        std::error_code ecExc;
        fs::path gitInfo = root_ / ".git" / "info";
        fs::create_directories(gitInfo, ecExc);
        fs::path gitExclude = gitInfo / "exclude";
        std::string excContent;
        if (fs::exists(gitExclude)) {
            try { excContent = readFile(gitExclude); } catch (...) {}
        }
        if (excContent.find(".minigit") == std::string::npos) {
            if (!excContent.empty() && excContent.back() != '\n') excContent += "\n";
            excContent += ".minigit/\n";
            writeFile(gitExclude, excContent);
        }
    }

    int retAdd = runProcess({"git", "-C", root_.string(), "add", "-A"});
    if (retAdd != 0) {
        fs::remove(tmpMsgFile, ec);
        throw MiniGitException("git add failed with exit code " + std::to_string(retAdd));
    }

    std::string commitOutput;
    int commitExitCode = runProcess({"git", "-C", root_.string(), "commit", "-F", tmpMsgFile.string()}, &commitOutput);
    fs::remove(tmpMsgFile, ec);

    if (commitExitCode != 0) {
        if (commitOutput.find("nothing to commit") != std::string::npos ||
            commitOutput.find("working tree clean") != std::string::npos) {
            return "already up to date";
        }
        if (commitOutput.find("Please tell me who you are") != std::string::npos ||
            commitOutput.find("user.name") != std::string::npos ||
            commitOutput.find("user.email") != std::string::npos ||
            commitOutput.find("Author identity unknown") != std::string::npos) {
            throw MiniGitException("git user.name and user.email are not configured");
        }
        throw MiniGitException("git commit failed with exit code " + std::to_string(commitExitCode) + (commitOutput.empty() ? "" : ": " + trim(commitOutput)));
    }

    recordReflog("git_export", head(), head(), "Exported to standard Git (.git)");
    return "Exported to standard Git repository successfully.";
}

std::string Repository::importFromGit() {
    if (!fs::exists(root_ / ".git")) {
        throw MiniGitException("no .git repository found in current directory");
    }
    add(".");
    Hash h;
    try {
        h = commit("Imported snapshot from standard Git repository (.git)");
    } catch (const NothingToCommitException&) {
        return "already up to date";
    }
    recordReflog("git_import", Hash(), h, "Imported from standard Git");
    return "Successfully imported current working tree as commit [" + h.shortStr() + "].";
}
