#include "ObjectStore.h"

Hash ObjectStore::save(const Object& obj) const {
    Hash h = obj.hash();
    if (!exists(h)) writeFile(dir_ / h.str(), obj.raw());
    return h;
}

std::unique_ptr<Object> ObjectStore::load(const Hash& h) const {
    if (h.empty() || !exists(h)) throw InvalidObjectException("object not found: " + h.str());
    return Object::deserialize(readFile(dir_ / h.str()));
}

Hash ObjectStore::resolvePrefix(const std::string& prefix) const {
    if (prefix.size() < 4) throw InvalidObjectException("unknown revision: " + prefix);
    Hash found;
    int matches = 0;
    if (fs::exists(dir_))
        for (const auto& e : fs::directory_iterator(dir_)) {
            std::string name = e.path().filename().string();
            if (name.rfind(prefix, 0) == 0) { found = Hash(name); ++matches; }
        }
    if (matches != 1) throw InvalidObjectException("unknown or ambiguous revision: " + prefix);
    return found;
}
