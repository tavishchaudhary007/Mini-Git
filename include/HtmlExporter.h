#pragma once
#include <string>
#include "Repository.h"

// Second frontend: turns a Snapshot into a self-contained HTML page by injecting
// the repository data (as JSON) into the embedded web UI template.
class HtmlExporter {
public:
    static std::string toJson(const Snapshot& snap, const std::string& repoName);
    static std::string render(const Snapshot& snap, const std::string& repoName);
};
