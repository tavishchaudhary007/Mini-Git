#pragma once
#include <map>
#include <memory>
#include <string>
#include "Commands.h"

// Frontend: parses argv, dispatches to a Command, converts exceptions to exit codes.
class CLI {
    std::map<std::string, std::unique_ptr<Command>> commands_;
    void registerCommand(std::unique_ptr<Command> c) { commands_[c->name()] = std::move(c); }
    void printUsage() const;
public:
    CLI();
    int run(int argc, char** argv);
};
