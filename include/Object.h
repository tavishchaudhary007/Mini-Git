#pragma once
#include <ctime>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include "Hash.h"

// Abstract base of everything stored in the object database.
class Object {
public:
    virtual ~Object() = default;
    virtual std::string type() const = 0;       // "blob" | "tree" | "commit"
    virtual std::string serialize() const = 0;  // body text

    std::string raw() const { return type() + "\n" + serialize(); }
    Hash hash() const;                          // SHA-1 of raw()
    static std::unique_ptr<Object> deserialize(const std::string& raw);  // factory
};

class Blob : public Object {
    std::string content_;
public:
    explicit Blob(std::string content) : content_(std::move(content)) {}
    std::string type() const override { return "blob"; }
    std::string serialize() const override { return content_; }
    const std::string& content() const { return content_; }
};

class Tree : public Object {
    std::map<std::string, Hash> entries_;  // path -> blob hash
public:
    void set(const std::string& path, const Hash& h) { entries_[path] = h; }
    const std::map<std::string, Hash>& entries() const { return entries_; }
    std::string type() const override { return "tree"; }
    std::string serialize() const override;
    static Tree parse(const std::string& body);
};

class Commit : public Object {
    Hash tree_, parent_;
    std::string author_, message_;
    std::time_t time_;
public:
    Commit(Hash tree, Hash parent, std::string author, std::string message, std::time_t t)
        : tree_(std::move(tree)), parent_(std::move(parent)), author_(std::move(author)),
          message_(std::move(message)), time_(t) {}

    const Hash& tree() const { return tree_; }
    const Hash& parent() const { return parent_; }
    const std::string& author() const { return author_; }
    const std::string& message() const { return message_; }
    std::time_t time() const { return time_; }

    std::string type() const override { return "commit"; }
    std::string serialize() const override;
    static Commit parse(const std::string& body);

    bool operator==(const Commit& o) const { return hash() == o.hash(); }
    friend std::ostream& operator<<(std::ostream& os, const Commit& c);
};
