#include "Object.h"
#include <iterator>
#include <sstream>
#include "Exceptions.h"
#include "FileUtil.h"
#include "Hasher.h"
#include <vector>
#include "PathSafety.h"

Hash Object::hash() const { return Hash(Hasher::sha1(raw())); }

std::unique_ptr<Object> Object::deserialize(const std::string& raw) {
    size_t nl = raw.find('\n');
    if (nl == std::string::npos) throw InvalidObjectException("corrupt object (no header)");
    std::string type = raw.substr(0, nl), body = raw.substr(nl + 1);
    if (type == "blob")   return std::make_unique<Blob>(body);
    if (type == "tree")   return std::make_unique<Tree>(Tree::parse(body));
    if (type == "commit") return std::make_unique<Commit>(Commit::parse(body));
    throw InvalidObjectException("unknown object type: " + type);
}

void Tree::dropPrefixParents() {
    std::vector<std::string> toDrop;
    for (const auto& [p, _] : entries_) {
        std::string prefix = p + "/";
        for (const auto& [other, _] : entries_) {
            if (other.size() > prefix.size() && other.rfind(prefix, 0) == 0) {
                toDrop.push_back(p);
                break;
            }
        }
    }
    for (const auto& p : toDrop) {
        entries_.erase(p);
    }
}

std::string Tree::serialize() const {
    const_cast<Tree*>(this)->dropPrefixParents();
    std::string out;
    for (const auto& [path, h] : entries_) out += h.str() + " " + path + "\n";
    return out;
}

Tree Tree::parse(const std::string& body) {
    Tree t;
    for (const auto& line : splitLines(body)) {
        if (line.size() < 42) continue;
        std::string hStr = line.substr(0, 40);
        validateHash(hStr);
        if (line[40] != ' ') throw InvalidObjectException("invalid tree line format");
        std::string p = line.substr(41);
        if (!isSafeRepoPath(p)) throw InvalidObjectException("unsafe path in tree: " + p);
        t.set(p, Hash(hStr));
    }
    return t;
}

std::string Commit::serialize() const {
    std::ostringstream os;
    os << "tree " << tree_ << "\nparent " << (parent_.empty() ? "-" : parent_.str());
    if (!parent2_.empty()) {
        os << "\nparent2 " << parent2_.str();
    }
    os << "\nauthor " << author_ << "\ntime " << time_ << "\n\n" << message_;
    return os.str();
}

Commit Commit::parse(const std::string& body) {
    std::istringstream in(body);
    std::string line, tree, parent, parent2, author;
    std::time_t t = 0;
    while (std::getline(in, line) && !line.empty()) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("tree ", 0) == 0) {
            tree = line.substr(5);
            validateHash(tree);
        } else if (line.rfind("parent2 ", 0) == 0) {
            parent2 = line.substr(8);
            validateHash(parent2);
        } else if (line.rfind("parent ", 0) == 0) {
            parent = line.substr(7);
            if (!parent.empty() && parent != "-") validateHash(parent);
        }
        else if (line.rfind("author ", 0) == 0) {
            author = line.substr(7);
            for (char ch : author) {
                unsigned char uc = static_cast<unsigned char>(ch);
                if (uc < 32 || uc == 127) {
                    throw InvalidObjectException("invalid author in commit header");
                }
            }
        }
        else if (line.rfind("time ", 0) == 0) {
            try {
                t = static_cast<std::time_t>(std::stoll(line.substr(5)));
            } catch (...) {
                throw InvalidObjectException("invalid commit time in header");
            }
        }
    }
    std::string message((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (tree.empty()) throw InvalidObjectException("corrupt commit (no tree)");
    return Commit(Hash(tree), Hash(parent == "-" ? "" : parent), author, message, t, Hash(parent2));
}

std::ostream& operator<<(std::ostream& os, const Commit& c) {
    char buf[64];
    std::time_t tmVal = c.time_;
    std::tm* tm = std::localtime(&tmVal);
    if (tm) {
        std::strftime(buf, sizeof buf, "%a %b %d %H:%M:%S %Y", tm);
    } else {
        std::snprintf(buf, sizeof buf, "Unknown");
    }
    os << "commit " << c.hash() << "\n";
    if (c.hasParent2()) {
        os << "Merge: " << c.parent_.shortStr() << " " << c.parent2_.shortStr() << "\n";
    }
    return os << "Author: " << c.author_ << "\nDate:   " << buf
              << "\n\n    " << c.message_ << "\n";
}
