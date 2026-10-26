#include "Repository.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include "DiffEngine.h"
#include "HtmlExporter.h"
#include "Base64.h"
#include "Json.h"
#include <set>

Repository::Repository(const fs::path& root)
    : root_(root), gitDir_(root / ".minigit"), store_(gitDir_ / "objects"),
      index_(gitDir_ / "index"), refs_(gitDir_) {}

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
    if (pathArg.empty() || pathArg == "." || pathArg == "..") {
        throw MiniGitException("invalid path: cannot be empty, '.', or '..'");
    }
    fs::path inPath(pathArg);
    if (inPath.is_absolute()) {
        throw MiniGitException("invalid path: absolute paths are not allowed: " + pathArg);
    }
    fs::path full;
    if (isRepoRelative) {
        full = (root_ / inPath).lexically_normal();
    } else {
        full = fs::weakly_canonical(fs::absolute(inPath));
    }
    fs::path normRoot = fs::weakly_canonical(root_);
    fs::path rel = full.lexically_relative(normRoot);
    std::string relStr = rel.generic_string();
    if (relStr.empty() || relStr == ".") {
        throw MiniGitException("invalid path: refers to repository root");
    }
    if (relStr.rfind("..", 0) == 0) {
        throw MiniGitException("path is outside the repository: " + pathArg);
    }
    if (relStr == ".minigit" || relStr.rfind(".minigit/", 0) == 0) {
        throw MiniGitException("invalid path: cannot access .minigit: " + pathArg);
    }
    return relStr;
}

bool Repository::isIgnored(const fs::path& fullPath, const std::string& relPath, bool isDir) const {
    std::string fname = fullPath.filename().generic_string();
    if (fname == ".git" || fname == ".minigit" || fname == "node_modules" ||
        fname == "minigit.exe" || fname == "minigit") {
        return true;
    }
    if (relPath == ".git" || relPath.rfind(".git/", 0) == 0 ||
        relPath == ".minigit" || relPath.rfind(".minigit/", 0) == 0 ||
        relPath == "node_modules" || relPath.rfind("node_modules/", 0) == 0 ||
        relPath == "minigit.exe" || relPath == "minigit") {
        return true;
    }

    fs::path ignPath = root_ / ".minigitignore";
    if (fs::exists(ignPath)) {
        std::ifstream ifs(ignPath);
        std::string line;
        while (std::getline(ifs, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
            if (line.empty() || line.front() == '#') continue;

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
    }
    return false;
}

std::vector<std::string> Repository::workingFiles() const {
    std::vector<std::string> files;
    for (auto it = fs::recursive_directory_iterator(root_); it != fs::recursive_directory_iterator(); ++it) {
        std::string rel = it->path().lexically_relative(root_).generic_string();
        if (it->is_directory()) {
            if (isIgnored(it->path(), rel, true)) {
                it.disable_recursion_pending();
                continue;
            }
        } else if (it->is_regular_file()) {
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
    index_.set(rel, store_.save(Blob(readFile(root_ / rel))));
}

void Repository::add(const std::string& pathArg, bool isRepoRelative) {
    if (pathArg == "." || pathArg.empty()) {
        for (const auto& f : workingFiles()) addFile(f);
        std::vector<std::string> gone;
        for (const auto& [path, h] : index_.entries())
            if (!fs::exists(root_ / path)) gone.push_back(path);
        for (const auto& g : gone) index_.remove(g);
        index_.save();
        return;
    }
    std::string rel = resolveRepoPath(pathArg, isRepoRelative);
    fs::path full = root_ / rel;
    if (fs::is_directory(full)) {
        std::string prefix = rel + "/";
        for (const auto& f : workingFiles())
            if (f.rfind(prefix, 0) == 0) addFile(f);
        std::vector<std::string> gone;
        for (const auto& [path, h] : index_.entries())
            if (path.rfind(prefix, 0) == 0 && !fs::exists(root_ / path)) gone.push_back(path);
        for (const auto& g : gone) index_.remove(g);
    } else if (fs::is_regular_file(full)) {
        if (!isIgnored(full, rel, false)) {
            addFile(rel);
        }
    } else if (index_.has(rel)) {
        index_.remove(rel);  // staging a deletion
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
    if (isDirty()) throw MiniGitException("you have uncommitted changes; commit them first");
    auto c = store_.loadAs<Commit>(commitHash);
    Hash parentHash = c->parent();

    std::map<std::string, Hash> cTree = store_.loadAs<Tree>(c->tree())->entries();
    std::map<std::string, Hash> pTree;
    if (!parentHash.empty()) {
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

    for (const auto& path : addedInC) {
        if (fs::exists(root_ / path)) fs::remove(root_ / path);
        for (fs::path d = (root_ / path).parent_path(); d != root_ && fs::is_empty(d); d = d.parent_path()) {
            fs::remove(d);
        }
        index_.remove(path);
    }
    for (const auto& path : removedInC) {
        writeFile(root_ / path, store_.loadAs<Blob>(pTree.at(path))->content());
        index_.set(path, pTree.at(path));
    }
    for (const auto& path : modifiedInC) {
        writeFile(root_ / path, store_.loadAs<Blob>(pTree.at(path))->content());
        index_.set(path, pTree.at(path));
    }
    index_.save();
    return commit("Revert \"" + c->message() + "\"");
}

Hash Repository::commit(const std::string& message) {
    if (message.empty()) throw MiniGitException("commit message must not be empty");
    if (index_.empty()) throw NothingToCommitException("nothing to commit (staging area is empty)");

    Tree tree;
    for (const auto& [path, h] : index_.entries()) tree.set(path, h);

    Hash parent = refs_.headCommit();
    if (!parent.empty() && store_.loadAs<Commit>(parent)->tree() == tree.hash())
        throw NothingToCommitException("nothing to commit (no changes staged)");

    store_.save(tree);
    Hash h = store_.save(Commit(tree.hash(), parent, author(), message, std::time(nullptr)));
    refs_.updateHead(h);
    recordReflog("commit", parent, h, message);
    return h;
}

std::vector<Commit> Repository::log() const {
    std::vector<Commit> out;
    for (Hash h = refs_.headCommit(); !h.empty();) {
        auto c = store_.loadAs<Commit>(h);
        out.push_back(*c);
        h = c->parent();
    }
    return out;
}

Status Repository::status() const {
    Status s;
    auto head = headTree();
    for (const auto& [path, h] : index_.entries()) {
        auto it = head.find(path);
        if (it == head.end() || it->second != h) s.staged.push_back(path);
        if (!fs::exists(root_ / path)) s.deleted.push_back(path);
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
    for (const auto& path : targets) {
        auto oldBlob = store_.loadAs<Blob>(index_.get(path));
        std::string now = fs::exists(root_ / path) ? readFile(root_ / path) : "";
        if (oldBlob->content() == now) continue;

        auto ops = DiffEngine<std::string>::diff(splitLines(oldBlob->content()), splitLines(now));
        out += "--- a/" + path + "\n+++ b/" + path + "\n";
        size_t i = 0, j = 0;
        for (const auto& op : ops) {
            using K = DiffOp<std::string>::Kind;
            if (op.kind == K::Keep)        { ++i; ++j; }
            else if (op.kind == K::Remove) out += "-" + std::to_string(++i) + ": " + op.value + "\n";
            else                           out += "+" + std::to_string(++j) + ": " + op.value + "\n";
        }
    }
    return out;
}

void Repository::createBranch(const std::string& name) {
    Hash tip = refs_.headCommit();
    if (tip.empty()) throw BranchException("cannot create a branch before the first commit");
    refs_.createBranch(name, tip);
    recordReflog("branch", tip, tip, "Created branch " + name);
}

bool Repository::isDirty() const {
    Status s = status();
    return !(s.staged.empty() && s.modified.empty() && s.deleted.empty());
}

bool Repository::isAncestor(const Hash& ancestor, const Hash& descendant) const {
    for (Hash h = descendant; !h.empty(); h = store_.loadAs<Commit>(h)->parent())
        if (h == ancestor) return true;
    return false;
}

void Repository::checkUntrackedOverwrite(const std::map<std::string, Hash>& targetTree) const {
    Status s = status();
    for (const auto& f : s.untracked) {
        if (targetTree.count(f)) {
            throw MiniGitException("untracked file '" + f + "' would be overwritten; move or commit it first");
        }
    }
}

void Repository::restoreCommit(const Hash& c) {
    auto tree = store_.loadAs<Tree>(store_.loadAs<Commit>(c)->tree());
    const auto& target = tree->entries();

    for (const auto& [path, h] : index_.entries()) {       // remove files absent in target
        if (target.count(path)) continue;
        fs::remove(root_ / path);
        for (fs::path d = (root_ / path).parent_path(); d != root_ && fs::is_empty(d); d = d.parent_path())
            fs::remove(d);
    }
    for (const auto& [path, h] : target)                   // write files from target
        writeFile(root_ / path, store_.loadAs<Blob>(h)->content());

    index_.replace(target);
    index_.save();
}

void Repository::checkout(const std::string& target) {
    if (isDirty()) throw CheckoutException("you have uncommitted changes; commit them first");
    Hash oldHead = refs_.headCommit();
    bool isBranch = refs_.branchExists(target);
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
    if (!refs_.onBranch()) throw MergeException("cannot merge while HEAD is detached");
    if (!refs_.branchExists(branch)) throw BranchException("no such branch: " + branch);
    if (isDirty()) throw MergeException("you have uncommitted changes; commit them first");

    Hash cur = refs_.headCommit(), tgt = refs_.branchTip(branch);
    if (cur.empty()) throw MergeException("current branch has no commits");
    if (cur == tgt || isAncestor(tgt, cur)) return "Already up to date.";
    if (isAncestor(cur, tgt)) {
        auto targetTree = store_.loadAs<Tree>(store_.loadAs<Commit>(tgt)->tree())->entries();
        checkUntrackedOverwrite(targetTree);
        restoreCommit(tgt);
        refs_.updateHead(tgt);
        recordReflog("merge", cur, tgt, "Merged branch " + branch);
        return "Fast-forward to " + tgt.shortStr();
    }
    throw MergeException("branches have diverged; only fast-forward merges are supported");
}

Snapshot Repository::snapshot() const {
    Snapshot s;
    s.currentBranch = refs_.currentBranch();
    s.head = refs_.headCommit();
    for (const auto& b : refs_.listBranches()) s.branches[b] = refs_.branchTip(b);
    for (const auto& h : store_.listAll()) s.objects.push_back(store_.load(h));

    for (const auto& f : workingFiles()) {
        std::string content = readFile(root_ / f);
        if (content.find('\0') != std::string::npos) continue;  // skip binary files
        s.files[f] = std::move(content);
    }

    auto head = headTree();
    for (const auto& [path, h] : index_.entries()) {
        auto it = head.find(path);
        if (it == head.end() || it->second != h) s.staged[path] = h;
    }
    for (const auto& [path, h] : head)
        if (!index_.has(path)) s.stagedDeleted.push_back(path);
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
    if (head().empty()) {
        throw MiniGitException("cannot stash before the first commit");
    }
    Status s = status();
    bool hasTrackedChanges = !(s.staged.empty() && s.modified.empty() && s.deleted.empty());
    if (!hasTrackedChanges && (!includeUntracked || s.untracked.empty())) {
        throw MiniGitException("no uncommitted changes to stash");
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
    auto list = stashList();
    if (list.empty()) throw MiniGitException("no stash entries found to pop");

    if (isDirty()) {
        throw MiniGitException("cannot pop stash: you have uncommitted changes");
    }

    StashEntry entry = list.front();

    auto c = store_.loadAs<Commit>(entry.commitHash);
    auto t = store_.loadAs<Tree>(c->tree());
    const auto& workEntries = t->entries();

    checkUntrackedOverwrite(workEntries);

    auto curHeadTree = headTree();
    for (const auto& [p, h] : curHeadTree) {
        if (!workEntries.count(p) && fs::exists(root_ / p)) {
            fs::remove(root_ / p);
        }
    }
    for (const auto& [p, blobHash] : workEntries) {
        writeFile(root_ / p, store_.loadAs<Blob>(blobHash)->content());
    }

    if (!entry.indexTreeHash.empty() && store_.exists(entry.indexTreeHash)) {
        auto idxTree = store_.loadAs<Tree>(entry.indexTreeHash);
        index_.replace(idxTree->entries());
    } else {
        index_.replace(curHeadTree);
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
            Blob b(content);
            if (b.hash() != expectedHash) {
                objectsRejected++;
            } else {
                verifiedBlobs.push_back({expectedHash, std::move(content)});
            }
        } else if (type == "tree") {
            Tree t;
            if (objVal.contains("data") && objVal["data"].is_object()) {
                for (const auto& [p, hVal] : objVal["data"].as_object()) {
                    t.set(p, Hash(hVal.as_string()));
                }
            }
            if (t.hash() != expectedHash) {
                objectsRejected++;
            } else {
                verifiedTrees.push_back({expectedHash, std::move(t)});
            }
        } else if (type == "commit") {
            if (objVal.contains("data") && objVal["data"].is_object()) {
                const auto& d = objVal["data"];
                std::string treeStr = d["tree"].as_string();
                std::string parentStr = (d.contains("parent") && !d["parent"].is_null()) ? d["parent"].as_string() : "";
                std::string authorStr = d["author"].as_string();
                std::string messageStr = d["message"].as_string();
                std::time_t t = 0;
                if (d.contains("time") && d["time"].is_number()) {
                    t = static_cast<std::time_t>(d["time"].as_int64());
                } else if (d.contains("timestamp") && d["timestamp"].is_string()) {
                    t = parseIsoTimestamp(d["timestamp"].as_string());
                }
                Commit c(Hash(treeStr), Hash(parentStr), authorStr, messageStr, t);
                if (c.hash() != expectedHash) {
                    objectsRejected++;
                } else {
                    verifiedCommits.push_back({expectedHash, std::move(c)});
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

    // Process Branches
    std::vector<std::string> branchesCreated;
    std::vector<std::string> branchesFastForwarded;
    std::vector<std::string> branchesSkippedDiverged;
    std::vector<std::string> branchesUpToDate;
    bool activeBranchDirtyRefused = false;
    std::string refuseReason;

    if (root.contains("branches") && root["branches"].is_object()) {
        for (const auto& [bName, tipVal] : root["branches"].as_object()) {
            if (tipVal.is_null() || !tipVal.is_string()) continue;
            std::string hStr = tipVal.as_string();
            if (hStr.size() != 40) continue;
            Hash target(hStr);
            if (!store_.exists(target)) continue;

            if (!refs_.branchExists(bName)) {
                refs_.createBranch(bName, target);
                if (refs_.currentBranch() == bName) {
                    restoreCommit(target);
                }
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
                        try {
                            checkUntrackedOverwrite(targetTree);
                        } catch (const MiniGitException& e) {
                            activeBranchDirtyRefused = true;
                            refuseReason = e.what();
                            continue;
                        }
                        restoreCommit(target);
                        refs_.updateHead(target);
                        recordReflog("p2p_sync", curTip, target, "P2P LAN Sync: fast-forward " + bName + " to " + target.shortStr());
                        branchesFastForwarded.push_back(bName);
                    } else {
                        refs_.createBranch(bName, target);
                        recordReflog("p2p_sync", curTip, target, "P2P LAN Sync: fast-forward " + bName + " to " + target.shortStr());
                        branchesFastForwarded.push_back(bName);
                    }
                } else {
                    // Diverged
                    branchesSkippedDiverged.push_back(bName);
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
        std::string divList;
        for (size_t i = 0; i < branchesSkippedDiverged.size(); ++i) {
            if (i > 0) divList += ", ";
            divList += branchesSkippedDiverged[i];
        }
        message = "Branch(es) diverged (" + divList + "), merge needed; tip unchanged.";
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

    return JsonValue(resObj).serialize();
}

std::string Repository::gitStatus() const {
    bool hasGit = fs::exists(root_ / ".git");
    return hasGit ? "{\"gitExists\":true}" : "{\"gitExists\":false}";
}

std::string Repository::exportToGit() {
    std::string headMsg = "MiniGit snapshot sync";
    if (!head().empty()) {
        try {
            headMsg = store_.loadAs<Commit>(head())->message();
        } catch (...) {}
    }

    std::error_code ec;
    fs::path tempDir = fs::temp_directory_path(ec);
    if (ec) tempDir = root_;
    fs::path tmpMsgFile = tempDir / ("mg_exp_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(std::rand()) + ".txt");
    writeFile(tmpMsgFile, headMsg);

    fs::path gitIgnore = root_ / ".gitignore";
    std::string giContent;
    if (fs::exists(gitIgnore)) {
        try { giContent = readFile(gitIgnore); } catch (...) {}
    }
    if (giContent.find(".minigit") == std::string::npos) {
        if (!giContent.empty() && giContent.back() != '\n') giContent += "\n";
        giContent += ".minigit/\n";
        writeFile(gitIgnore, giContent);
    }

    std::string rootArg = "\"" + root_.string() + "\"";
    std::string msgFileArg = "\"" + tmpMsgFile.string() + "\"";

    int retCheck = std::system("git --version >nul 2>&1");
#ifndef _WIN32
    retCheck = std::system("git --version >/dev/null 2>&1");
#endif
    if (retCheck != 0) {
        fs::remove(tmpMsgFile, ec);
        throw MiniGitException("git is not installed or not available in PATH");
    }

    if (!fs::exists(root_ / ".git")) {
        std::string cmdInit = "git -C " + rootArg + " init";
        int retInit = std::system(cmdInit.c_str());
        if (retInit != 0) {
            fs::remove(tmpMsgFile, ec);
            throw MiniGitException("git init failed with exit code " + std::to_string(retInit));
        }
    }

    std::string cmdAdd = "git -C " + rootArg + " add -A";
    int retAdd = std::system(cmdAdd.c_str());
    if (retAdd != 0) {
        fs::remove(tmpMsgFile, ec);
        throw MiniGitException("git add failed with exit code " + std::to_string(retAdd));
    }

    std::string cmdCommit = "git -C " + rootArg + " commit -F " + msgFileArg;
    int retCommit = std::system(cmdCommit.c_str());
    fs::remove(tmpMsgFile, ec);
    if (retCommit != 0) {
        throw MiniGitException("git commit failed with exit code " + std::to_string(retCommit));
    }

    recordReflog("git_export", head(), head(), "Exported to standard Git (.git)");
    return "Exported to standard Git repository successfully.";
}

std::string Repository::importFromGit() {
    if (!fs::exists(root_ / ".git")) {
        throw MiniGitException("no .git repository found in current directory");
    }
    add(".");
    Hash h = commit("Imported snapshot from standard Git repository (.git)");
    recordReflog("git_import", Hash(), h, "Imported from standard Git");
    return "Successfully imported Git repository as commit [" + h.shortStr() + "].";
}
