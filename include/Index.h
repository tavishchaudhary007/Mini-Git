#pragma once
#include <map>
#include "FileUtil.h"
#include "Hash.h"

// Staging area: .minigit/index  (one "<hash> <path>" per line)
class Index {
    fs::path file_;
    std::map<std::string, Hash> entries_;
    void load();
public:
    explicit Index(fs::path file) : file_(std::move(file)) { load(); }
    void save() const;

    void set(const std::string& path, const Hash& h) { entries_[path] = h; }
    void remove(const std::string& path) { entries_.erase(path); }
    bool has(const std::string& path) const { return entries_.count(path) > 0; }
    const Hash& get(const std::string& path) const { return entries_.at(path); }
    bool empty() const { return entries_.empty(); }
    const std::map<std::string, Hash>& entries() const { return entries_; }
    void replace(const std::map<std::string, Hash>& e) { entries_ = e; }
};
