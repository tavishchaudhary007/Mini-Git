#include "CLI.h"
#include <iostream>

CLI::CLI() {
    registerCommand(std::make_unique<InitCmd>());
    registerCommand(std::make_unique<AddCmd>());
    registerCommand(std::make_unique<CommitCmd>());
    registerCommand(std::make_unique<LogCmd>());
    registerCommand(std::make_unique<StatusCmd>());
    registerCommand(std::make_unique<DiffCmd>());
    registerCommand(std::make_unique<BranchCmd>());
    registerCommand(std::make_unique<CheckoutCmd>());
    registerCommand(std::make_unique<MergeCmd>());
}

void CLI::printUsage() const {
    std::cerr << "Mini-Git - usage:\n";
    for (const auto& [name, cmd] : commands_) std::cerr << "  " << cmd->usage() << "\n";
}

int CLI::run(int argc, char** argv) {
    if (argc < 2) { printUsage(); return 1; }
    auto it = commands_.find(argv[1]);
    if (it == commands_.end()) {
        std::cerr << "error: unknown command '" << argv[1] << "'\n";
        printUsage();
        return 1;
    }
    Args args(argv + 2, argv + argc);
    try {
        std::unique_ptr<Repository> repo;
        if (it->second->needsRepo()) repo = std::make_unique<Repository>(Repository::findRoot(fs::current_path()));
        it->second->execute(repo.get(), args);
        return 0;
    } catch (const MiniGitException& e) {
        std::cerr << "error [" << e.category() << "]: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 2;
    }
}
