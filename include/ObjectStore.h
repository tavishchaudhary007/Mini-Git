#pragma once
#include <memory>
#include "FileUtil.h"
#include "Object.h"

// Persistent, content-addressed object database: .minigit/objects/<sha1>
class ObjectStore {
    fs::path dir_;
public:
    explicit ObjectStore(fs::path dir) : dir_(std::move(dir)) {}

    Hash save(const Object& obj) const;
    std::unique_ptr<Object> load(const Hash& h) const;
    bool exists(const Hash& h) const { return fs::exists(dir_ / h.str()); }
    Hash resolvePrefix(const std::string& prefix) const;  // short hash -> full hash

    // Typed load; throws if the stored object is a different type.
    template <typename T>
    std::unique_ptr<T> loadAs(const Hash& h) const {
        std::unique_ptr<Object> base = load(h);
        T* typed = dynamic_cast<T*>(base.get());
        if (!typed) throw InvalidObjectException("object " + h.shortStr() + " is a " + base->type());
        base.release();
        return std::unique_ptr<T>(typed);
    }
};
