#include "Object.h"
#include <iterator>
#include <sstream>
#include "Exceptions.h"
#include "FileUtil.h"
#include "Hasher.h"

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

std::string Tree::serialize() const {
    std::string out;
    for (const auto& [path, h] : entries_) out += h.str() + " " + path + "\n";
    return out;
}

Tree Tree::parse(const std::string& body) {
    Tree t;
    for (const auto& line : splitLines(body)) {
        if (line.size() < 42) continue;
        t.set(line.substr(41), Hash(line.substr(0, 40)));
    }
    return t;
}

std::string Commit::serialize() const {
    std::ostringstream os;
    os << "tree " << tree_ << "\nparent " << (parent_.empty() ? "-" : parent_.str())
       << "\nauthor " << author_ << "\ntime " << time_ << "\n\n" << message_;
    return os.str();
}

Commit Commit::parse(const std::string& body) {
    std::istringstream in(body);
    std::string line, tree, parent, author;
    std::time_t t = 0;
    while (std::getline(in, line) && !line.empty()) {
        if (line.rfind("tree ", 0) == 0)        tree = line.substr(5);
        else if (line.rfind("parent ", 0) == 0) parent = line.substr(7);
        else if (line.rfind("author ", 0) == 0) author = line.substr(7);
        else if (line.rfind("time ", 0) == 0)   t = static_cast<std::time_t>(std::stoll(line.substr(5)));
    }
    std::string message((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (tree.empty()) throw InvalidObjectException("corrupt commit (no tree)");
    return Commit(Hash(tree), Hash(parent == "-" ? "" : parent), author, message, t);
}

std::ostream& operator<<(std::ostream& os, const Commit& c) {
    char buf[64];
    std::tm* tm = std::localtime(&c.time_);
    std::strftime(buf, sizeof buf, "%a %b %d %H:%M:%S %Y", tm);
    return os << "commit " << c.hash() << "\nAuthor: " << c.author_ << "\nDate:   " << buf
              << "\n\n    " << c.message_ << "\n";
}
