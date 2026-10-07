#pragma once
#include <string>
#include <vector>
#include "Repository.h"

using Args = std::vector<std::string>;

// Command pattern: each CLI verb is a polymorphic object (frontend layer).
class Command {
public:
    virtual ~Command() = default;
    virtual std::string name() const = 0;
    virtual std::string usage() const = 0;
    virtual bool needsRepo() const { return true; }
    virtual void execute(Repository* repo, const Args& args) = 0;
};

// Each expansion is a concrete Command subclass.
#define MG_COMMAND(Cls, Name, Usage, NeedsRepo)                                  \
    class Cls : public Command {                                                 \
    public:                                                                      \
        std::string name() const override { return Name; }                       \
        std::string usage() const override { return Usage; }                     \
        bool needsRepo() const override { return NeedsRepo; }                    \
        void execute(Repository* repo, const Args& args) override;               \
    };

MG_COMMAND(InitCmd,     "init",     "minigit init", false)
MG_COMMAND(AddCmd,      "add",      "minigit add <file|dir|.>...", true)
MG_COMMAND(CommitCmd,   "commit",   "minigit commit -m \"message\"", true)
MG_COMMAND(LogCmd,      "log",      "minigit log [--oneline]", true)
MG_COMMAND(StatusCmd,   "status",   "minigit status", true)
MG_COMMAND(DiffCmd,     "diff",     "minigit diff [file]", true)
MG_COMMAND(BranchCmd,   "branch",   "minigit branch [name]", true)
MG_COMMAND(CheckoutCmd, "checkout", "minigit checkout <branch|hash>", true)
MG_COMMAND(MergeCmd,    "merge",    "minigit merge <branch>", true)
