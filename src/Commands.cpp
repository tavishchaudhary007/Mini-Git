#include "Commands.h"
#include <iostream>

using std::cout;

static void usageError(const Command& c) { throw MiniGitException("usage: " + c.usage()); }

void InitCmd::execute(Repository*, const Args&) {
    fs::path cwd = fs::current_path();
    Repository::init(cwd);
    cout << "Initialized empty Mini-Git repository in " << (cwd / ".minigit").generic_string() << "\n";
}

void AddCmd::execute(Repository* repo, const Args& args) {
    if (args.empty()) usageError(*this);
    for (const auto& a : args) repo->add(a);
}

void CommitCmd::execute(Repository* repo, const Args& args) {
    std::string msg;
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "-m") msg = args[i + 1];
    if (msg.empty()) usageError(*this);
    Hash h = repo->commit(msg);
    std::string b = repo->currentBranch();
    cout << "[" << (b.empty() ? "detached" : b) << " " << h.shortStr() << "] " << msg << "\n";
}

void LogCmd::execute(Repository* repo, const Args& args) {
    bool oneline = !args.empty() && args[0] == "--oneline";
    auto commits = repo->log();
    if (commits.empty()) { cout << "No commits yet.\n"; return; }
    for (const auto& c : commits) {
        if (oneline) cout << c.hash().shortStr() << " " << c.message().substr(0, c.message().find('\n')) << "\n";
        else         cout << c << "\n";
    }
}

void StatusCmd::execute(Repository* repo, const Args&) {
    std::string b = repo->currentBranch();
    cout << (b.empty() ? "HEAD detached at " + repo->head().shortStr() : "On branch " + b) << "\n";
    Status s = repo->status();
    auto section = [](const char* title, const std::vector<std::string>& v) {
        if (v.empty()) return;
        cout << title << "\n";
        for (const auto& f : v) cout << "    " << f << "\n";
    };
    section("Staged (will be committed):", s.staged);
    section("Modified (not staged):", s.modified);
    section("Deleted (not staged):", s.deleted);
    section("Untracked:", s.untracked);
    if (s.clean()) cout << "Nothing to report, working tree clean.\n";
}

void DiffCmd::execute(Repository* repo, const Args& args) {
    std::string d = repo->diff(args.empty() ? "" : args[0]);
    cout << (d.empty() ? "No differences.\n" : d);
}

void BranchCmd::execute(Repository* repo, const Args& args) {
    if (args.empty()) {
        std::string cur = repo->currentBranch();
        for (const auto& b : repo->branches()) cout << (b == cur ? "* " : "  ") << b << "\n";
        return;
    }
    repo->createBranch(args[0]);
    cout << "Created branch " << args[0] << "\n";
}

void CheckoutCmd::execute(Repository* repo, const Args& args) {
    if (args.size() != 1) usageError(*this);
    repo->checkout(args[0]);
    std::string b = repo->currentBranch();
    cout << (b.empty() ? "HEAD is now at " + repo->head().shortStr() : "Switched to branch '" + b + "'") << "\n";
}

void MergeCmd::execute(Repository* repo, const Args& args) {
    if (args.size() != 1) usageError(*this);
    cout << repo->merge(args[0]) << "\n";
}
