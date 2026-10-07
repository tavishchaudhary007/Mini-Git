#pragma once
#include <stdexcept>
#include <string>

// Base of the exception hierarchy. category() is virtual so the CLI can report
// which subsystem failed; what() is noexcept (inherited from std::runtime_error).
class MiniGitException : public std::runtime_error {
public:
    explicit MiniGitException(const std::string& msg) : std::runtime_error(msg) {}
    virtual const char* category() const noexcept { return "general"; }
};

// Each macro expansion is a full derived class (see UML).
#define MG_DEFINE_EXCEPTION(Name, Cat)                                          \
    class Name : public MiniGitException {                                      \
    public:                                                                     \
        explicit Name(const std::string& m) : MiniGitException(m) {}            \
        const char* category() const noexcept override { return Cat; }          \
    };

MG_DEFINE_EXCEPTION(NotARepoException,       "repository")
MG_DEFINE_EXCEPTION(FileNotFoundException,   "file")
MG_DEFINE_EXCEPTION(InvalidObjectException,  "object-store")
MG_DEFINE_EXCEPTION(BranchException,         "branch")
MG_DEFINE_EXCEPTION(NothingToCommitException,"commit")
MG_DEFINE_EXCEPTION(CheckoutException,       "checkout")
MG_DEFINE_EXCEPTION(MergeException,          "merge")
