#include "Repository.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include "DiffEngine.h"

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
    bool isBranch = refs_.branchExists(target);
    Hash dest = isBranch ? refs_.branchTip(target) : store_.resolvePrefix(target);
    store_.loadAs<Commit>(dest);  // validates that dest is a commit
    restoreCommit(dest);
    if (isBranch) refs_.switchToBranch(target);
    else          refs_.detach(dest);
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
