#include "Commands.h"
#include <cstdlib>
#include <iostream>
#include "HtmlExporter.h"
#include "HttpServer.h"

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
    bool oneline = false;
    for (const auto& a : args) {
        if (a == "--oneline") oneline = true;
        else usageError(*this);
    }
    for (const auto& c : repo->log()) {
        if (oneline) {
            std::string first = c.message();
            auto nl = first.find('\n');
            if (nl != std::string::npos) first = first.substr(0, nl);
            cout << c.hash().shortStr() << " " << first << "\n";
        } else {
            cout << "commit " << c.hash().str() << "\n";
            if (c.hasParent2()) {
                cout << "Merge: " << c.parent().shortStr() << " " << c.parent2().shortStr() << "\n";
            }
            cout << "Author: " << c.author() << "\nDate:   " << c.time() << "\n\n    "
                 << c.message() << "\n\n";
        }
    }
}

void StatusCmd::execute(Repository* repo, const Args& args) {
    if (!args.empty()) usageError(*this);
    Status s = repo->status();
    std::string b = repo->currentBranch();
    cout << "On branch " << (b.empty() ? "(detached)" : b) << "\n";
    if (s.clean()) { cout << "nothing to commit, working tree clean\n"; return; }
    if (!s.staged.empty()) {
        cout << "\nChanges to be committed:\n";
        for (const auto& f : s.staged) cout << "  staged:    " << f << "\n";
    }
    if (!s.modified.empty() || !s.deleted.empty()) {
        cout << "\nChanges not staged for commit:\n";
        for (const auto& f : s.modified) cout << "  modified:  " << f << "\n";
        for (const auto& f : s.deleted)  cout << "  deleted:   " << f << "\n";
    }
    if (!s.untracked.empty()) {
        cout << "\nUntracked files:\n";
        for (const auto& f : s.untracked) cout << "  " << f << "\n";
    }
}

void DiffCmd::execute(Repository* repo, const Args& args) {
    if (args.size() > 1) usageError(*this);
    std::string out = repo->diff(args.empty() ? "" : args[0]);
    if (!out.empty()) cout << out;
}

void BranchCmd::execute(Repository* repo, const Args& args) {
    if (args.empty()) {
        std::string cur = repo->currentBranch();
        for (const auto& b : repo->branches()) {
            if (b == cur) cout << "* " << b << "\n";
            else          cout << "  " << b << "\n";
        }
        return;
    }
    if (args.size() == 1) {
        repo->createBranch(args[0]);
        cout << "Created branch " << args[0] << "\n";
        return;
    }
    usageError(*this);
}

void CheckoutCmd::execute(Repository* repo, const Args& args) {
    if (args.size() != 1) usageError(*this);
    repo->checkout(args[0]);
    std::string b = repo->currentBranch();
    if (!b.empty()) cout << "Switched to branch '" << b << "'\n";
    else           cout << "Note: switching to '" << args[0] << "'. HEAD is now detached.\n";
}

void MergeCmd::execute(Repository* repo, const Args& args) {
    if (args.empty() || args.size() > 1) usageError(*this);
    if (args[0] == "--abort") {
        cout << repo->mergeAbort() << "\n";
        return;
    }
    cout << repo->merge(args[0]) << "\n";
}

static bool openInBrowser(const fs::path& file) {
#if defined(_WIN32)
    std::string cmd = "start \"\" \"" + file.string() + "\"";
#elif defined(__APPLE__)
    std::string cmd = "open \"" + file.string() + "\"";
#else
    std::string cmd = "xdg-open \"" + file.string() + "\" >/dev/null 2>&1";
#endif
    return std::system(cmd.c_str()) == 0;
}

void HtmlCmd::execute(Repository* repo, const Args& args) {
    bool open = true;
    for (const auto& a : args) {
        if (a == "--no-open") open = false;
        else usageError(*this);
    }
    // Written inside .minigit/ so the report never shows up as an untracked file.
    fs::path out = repo->root() / ".minigit" / "report.html";
    writeFile(out, HtmlExporter::render(repo->snapshot(), repo->root().filename().string()));
    cout << "Report written to " << out.generic_string() << "\n";
    if (open) {
        if (openInBrowser(out)) cout << "Opened in your default browser.\n";
        else cout << "Could not launch a browser; open the file above manually.\n";
    }
}

void UiCmd::execute(Repository* repo, const Args& args) {
    int port = 8080;
    bool open = true;
    bool lan = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--no-open") {
            open = false;
        } else if (args[i] == "--lan") {
            lan = true;
        } else if (args[i] == "--port" && i + 1 < args.size()) {
            port = std::stoi(args[++i]);
        } else {
            usageError(*this);
        }
    }
    HttpServer::run(repo, port, open, lan);
}

void ServeCmd::execute(Repository* repo, const Args& args) {
    UiCmd().execute(repo, args);
}
