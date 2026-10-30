#include "Index.h"

void Index::load() {
    entries_.clear();
    if (!fs::exists(file_)) return;
    for (const auto& line : splitLines(readFile(file_))) {
        if (line.size() >= 42) {
            std::string path = line.substr(41);
            if (hasIllegalPathChars(path)) {
                std::cerr << "warning: skipping index entry with illegal characters in path: " << path << "\n";
                continue;
            }
            entries_[path] = Hash(line.substr(0, 40));
        }
    }
}

void Index::save() const {
    std::string out;
    for (const auto& [path, h] : entries_) out += h.str() + " " + path + "\n";
    writeFile(file_, out);
}
