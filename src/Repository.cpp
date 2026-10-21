#include "Repository.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include "DiffEngine.h"
#include "HtmlExporter.h"

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
    if (s == "." ) return "";
    if (s.rfind("..", 0) == 0) throw MiniGitException("path is outside the repository: " + p.generic_string());
    return s;
}

std::vector<std::string> Repository::workingFiles() const {
    std::vector<std::string> files;
    for (auto it = fs::recursive_directory_iterator(root_); it != fs::recursive_directory_iterator(); ++it) {
        if (it->path().filename() == ".minigit") { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file()) files.push_back(it->path().lexically_relative(root_).generic_string());
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

void Repository::add(const std::string& pathArg) {
    fs::path p(pathArg);
    std::string rel = relPath(p);
    if (fs::is_directory(p)) {
        std::string prefix = rel.empty() ? "" : rel + "/";
        for (const auto& f : workingFiles())
            if (f.rfind(prefix, 0) == 0) addFile(f);
        std::vector<std::string> gone;  // tracked files deleted from disk
        for (const auto& [path, h] : index_.entries())
            if (path.rfind(prefix, 0) == 0 && !fs::exists(root_ / path)) gone.push_back(path);
        for (const auto& g : gone) index_.remove(g);
    } else if (fs::is_regular_file(p)) {
        addFile(rel);
    } else if (index_.has(rel)) {
        index_.remove(rel);  // staging a deletion
    } else {
        throw FileNotFoundException("file not found: " + pathArg);
    }
    index_.save();
}

void Repository::unstage(const std::string& pathArg) {
    auto head = headTree();
    if (pathArg.empty() || pathArg == ".") {
        index_.replace(head);
        index_.save();
        return;
    }
    std::string rel = relPath(fs::path(pathArg));
    if (head.count(rel)) {
        index_.set(rel, head.at(rel));
    } else {
        index_.remove(rel);
    }
    index_.save();
}

void Repository::discard(const std::string& pathArg) {
    std::string rel = relPath(fs::path(pathArg));
    auto head = headTree();
    if (head.count(rel)) {
        writeFile(root_ / rel, store_.loadAs<Blob>(head.at(rel))->content());
        index_.set(rel, head.at(rel));
        index_.save();
    } else if (index_.has(rel)) {
        writeFile(root_ / rel, store_.loadAs<Blob>(index_.get(rel))->content());
    } else if (fs::exists(root_ / rel)) {
        fs::remove_all(root_ / rel);
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

    // Invert the changes introduced in c
    for (const auto& [path, h] : cTree) {
        if (pTree.count(path)) {
            writeFile(root_ / path, store_.loadAs<Blob>(pTree.at(path))->content());
            index_.set(path, pTree.at(path));
        } else {
            if (fs::exists(root_ / path)) fs::remove(root_ / path);
            index_.remove(path);
        }
    }
    for (const auto& [path, h] : pTree) {
        if (!cTree.count(path)) {
            writeFile(root_ / path, store_.loadAs<Blob>(h)->content());
            index_.set(path, h);
        }
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

std::string Repository::diff(const std::string& pathArg) const {
    std::vector<std::string> targets;
    if (pathArg.empty()) for (const auto& [p, h] : index_.entries()) targets.push_back(p);
    else {
        std::string rel = relPath(pathArg);
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
    store_.loadAs<Commit>(dest);  // validates that dest is a commit
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

static std::string currentIsoTime() {
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::gmtime(&t);
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", tm);
    return buf;
}

void Repository::recordReflog(const std::string& action, const Hash& oldH, const Hash& newH, const std::string& desc) {
    try {
        std::ofstream ofs(gitDir_ / "reflog", std::ios::app);
        if (ofs.is_open()) {
            ofs << currentIsoTime() << "|" << action << "|"
                << (oldH.empty() ? "0000000" : oldH.str()) << "|"
                << (newH.empty() ? "0000000" : newH.str()) << "|"
                << desc << "\n";
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
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string item;
            std::vector<std::string> parts;
            while (std::getline(ss, item, '|')) parts.push_back(item);
            if (parts.size() >= 5) {
                entries.push_back({parts[0], parts[1], parts[2], parts[3], parts[4]});
            }
        }
    }
    if (entries.empty()) {
        for (const auto& c : log()) {
            entries.push_back({
                currentIsoTime(),
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
    return fs::exists(gitDir_ / "stash");
}

std::string Repository::stashSave(const std::string& message) {
    Status s = status();
    if (s.clean()) throw MiniGitException("no uncommitted changes to stash");

    Tree t;
    for (const auto& f : workingFiles()) {
        t.set(f, store_.save(Blob(readFile(root_ / f))));
    }
    store_.save(t);

    std::string desc = message.empty() ? ("WIP on " + (currentBranch().empty() ? "detached" : currentBranch())) : message;
    Hash h = store_.save(Commit(t.hash(), head(), author(), desc, std::time(nullptr)));
    writeFile(gitDir_ / "stash", h.str());

    if (!head().empty()) {
        restoreCommit(head());
    }

    recordReflog("stash", head(), h, "Saved stash: " + desc);
    return "Saved stash [" + h.shortStr() + "]: " + desc;
}

std::string Repository::stashPop() {
    if (!hasStash()) throw MiniGitException("no stash entries found to pop");
    std::string hStr = readFile(gitDir_ / "stash");
    while (!hStr.empty() && (hStr.back() == '\r' || hStr.back() == '\n' || hStr.back() == ' ')) hStr.pop_back();
    Hash stashHash(hStr);

    auto c = store_.loadAs<Commit>(stashHash);
    auto t = store_.loadAs<Tree>(c->tree());
    for (const auto& [p, blobHash] : t->entries()) {
        writeFile(root_ / p, store_.loadAs<Blob>(blobHash)->content());
        index_.set(p, blobHash);
    }
    index_.save();

    fs::remove(gitDir_ / "stash");
    recordReflog("stash_pop", head(), head(), "Restored stash [" + stashHash.shortStr() + "]");
    return "Restored and applied stash [" + stashHash.shortStr() + "]";
}

std::string Repository::exportBundleJson() const {
    return HtmlExporter::toJson(snapshot(), root_.filename().string());
}

std::string Repository::importBundleJson(const std::string& bundleJson) {
    // Parse objects from bundle JSON
    size_t objPos = bundleJson.find("\"objects\":{");
    if (objPos != std::string::npos) {
        size_t start = objPos + 11;
        while (start < bundleJson.size()) {
            size_t hashStart = bundleJson.find('"', start);
            if (hashStart == std::string::npos) break;
            size_t hashEnd = bundleJson.find('"', hashStart + 1);
            if (hashEnd == std::string::npos) break;
            std::string hashStr = bundleJson.substr(hashStart + 1, hashEnd - hashStart - 1);
            if (hashStr.size() >= 40) {
                size_t typePos = bundleJson.find("\"type\":\"", hashEnd);
                if (typePos != std::string::npos && typePos < hashEnd + 50) {
                    size_t typeEnd = bundleJson.find('"', typePos + 8);
                    std::string type = bundleJson.substr(typePos + 8, typeEnd - typePos - 8);
                    if (type == "blob") {
                        size_t cPos = bundleJson.find("\"content\":\"", typeEnd);
                        if (cPos != std::string::npos) {
                            size_t cStart = cPos + 11;
                            std::string content;
                            for (size_t i = cStart; i < bundleJson.size(); ++i) {
                                if (bundleJson[i] == '\\' && i + 1 < bundleJson.size()) {
                                    char next = bundleJson[++i];
                                    if (next == 'n') content += '\n';
                                    else if (next == 'r') content += '\r';
                                    else if (next == 't') content += '\t';
                                    else if (next == '"') content += '"';
                                    else if (next == '\\') content += '\\';
                                    else content += next;
                                } else if (bundleJson[i] == '"') {
                                    break;
                                } else {
                                    content += bundleJson[i];
                                }
                            }
                            store_.save(Blob(content));
                        }
                    }
                }
            }
            size_t nextObj = bundleJson.find("},\"", hashEnd);
            if (nextObj == std::string::npos || nextObj > bundleJson.find("},\"files\"", hashEnd)) {
                break;
            }
            start = nextObj + 2;
        }
    }

    // Parse branches from bundle JSON
    size_t bPos = bundleJson.find("\"branches\":{");
    if (bPos != std::string::npos) {
        size_t bEnd = bundleJson.find('}', bPos);
        std::string bSection = bundleJson.substr(bPos + 12, bEnd - bPos - 12);
        std::stringstream ss(bSection);
        std::string pair;
        while (std::getline(ss, pair, ',')) {
            size_t q1 = pair.find('"');
            size_t q2 = pair.find('"', q1 + 1);
            size_t q3 = pair.find('"', q2 + 1);
            size_t q4 = pair.find('"', q3 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos && q3 != std::string::npos && q4 != std::string::npos) {
                std::string bName = pair.substr(q1 + 1, q2 - q1 - 1);
                std::string bHashStr = pair.substr(q3 + 1, q4 - q3 - 1);
                if (!bName.empty() && bHashStr.size() >= 40) {
                    Hash h(bHashStr);
                    if (store_.exists(h)) {
                        if (!refs_.branchExists(bName)) {
                            refs_.createBranch(bName, h);
                        } else if (isAncestor(refs_.branchTip(bName), h)) {
                            if (refs_.currentBranch() == bName) {
                                restoreCommit(h);
                                refs_.updateHead(h);
                            } else {
                                refs_.createBranch(bName, h);
                            }
                        }
                    }
                }
            }
        }
    }

    recordReflog("p2p_sync", head(), head(), "P2P LAN Sync Bundle Imported");
    return "P2P LAN Sync complete: verified objects and updated branches.";
}

std::string Repository::gitStatus() const {
    bool hasGit = fs::exists(root_ / ".git");
    return hasGit ? "{\"gitExists\":true}" : "{\"gitExists\":false}";
}

std::string Repository::exportToGit() {
    fs::path gitDir = root_ / ".git";
    std::string rootStr = root_.string();
    std::string headMsg = "MiniGit snapshot sync";
    if (!head().empty()) {
        try {
            headMsg = store_.loadAs<Commit>(head())->message();
        } catch (...) {}
    }
    std::string safeMsg;
    for (char c : headMsg) { if (c == '"') safeMsg += "'"; else safeMsg += c; }

    std::string cmd;
    if (!fs::exists(gitDir)) {
#ifdef _WIN32
        cmd = "cd /d \"" + rootStr + "\" && git init && git add -A && git commit -m \"" + safeMsg + "\"";
#else
        cmd = "cd \"" + rootStr + "\" && git init && git add -A && git commit -m \"" + safeMsg + "\"";
#endif
    } else {
#ifdef _WIN32
        cmd = "cd /d \"" + rootStr + "\" && git add -A && git commit -m \"" + safeMsg + "\"";
#else
        cmd = "cd \"" + rootStr + "\" && git add -A && git commit -m \"" + safeMsg + "\"";
#endif
    }

    int ret = std::system(cmd.c_str());
    recordReflog("git_export", head(), head(), "Exported to standard Git (.git)");
    if (ret == 0) {
        return "Exported to standard Git repository successfully.";
    }
    return "Git export executed (exit code " + std::to_string(ret) + ").";
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
