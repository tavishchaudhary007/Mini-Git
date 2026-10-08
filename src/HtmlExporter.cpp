#include "HtmlExporter.h"
#include <cstdio>
#include <ctime>
#include <sstream>
#include "FrontendTemplate.h"

namespace {

// JSON string literal. '<', '>' and '&' are escaped so the data can never
// close the surrounding <script> tag.
void jsonString(std::ostream& os, const std::string& s) {
    os << '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\n': os << "\\n";  break;
            case '\r': os << "\\r";  break;
            case '\t': os << "\\t";  break;
            case '<':  os << "\\u003c"; break;
            case '>':  os << "\\u003e"; break;
            case '&':  os << "\\u0026"; break;
            default:
                if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); os << buf; }
                else os << static_cast<char>(c);
        }
    }
    os << '"';
}

std::string isoTime(std::time_t t) {
    char buf[32];
    std::tm* tm = std::gmtime(&t);
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", tm);
    return buf;
}

void jsonHashOrNull(std::ostream& os, const Hash& h) {
    if (h.empty()) os << "null"; else jsonString(os, h.str());
}

}  // namespace

std::string HtmlExporter::toJson(const Snapshot& snap, const std::string& repoName) {
    std::ostringstream o;

    // The page needs a "current branch" entry even before the first commit or when detached.
    std::string current = snap.currentBranch;
    std::map<std::string, Hash> branches = snap.branches;
    if (current.empty()) { current = "(detached " + snap.head.shortStr() + ")"; branches[current] = snap.head; }
    else if (!branches.count(current)) branches[current] = Hash();

    o << "{\"repoName\":";      jsonString(o, repoName);
    o << ",\"generated\":";     jsonString(o, isoTime(std::time(nullptr)));
    o << ",\"currentBranch\":"; jsonString(o, current);

    o << ",\"branches\":{";
    bool first = true;
    for (const auto& [name, tip] : branches) {
        if (!first) o << ',';
        first = false;
        jsonString(o, name); o << ':'; jsonHashOrNull(o, tip);
    }

    o << "},\"objects\":{";
    first = true;
    for (const auto& obj : snap.objects) {
        if (!first) o << ',';
        first = false;
        jsonString(o, obj->hash().str());
        o << ":{\"type\":"; jsonString(o, obj->type());
        if (const auto* b = dynamic_cast<const Blob*>(obj.get())) {
            o << ",\"content\":";
            if (b->content().find('\0') != std::string::npos) {
                jsonString(o, "[binary file]");
            } else {
                jsonString(o, b->content());
            }
        } else if (const auto* t = dynamic_cast<const Tree*>(obj.get())) {
            o << ",\"data\":{";
            bool f2 = true;
            for (const auto& [path, h] : t->entries()) {
                if (!f2) o << ',';
                f2 = false;
                jsonString(o, path); o << ':'; jsonString(o, h.str());
            }
            o << '}';
        } else if (const auto* c = dynamic_cast<const Commit*>(obj.get())) {
            o << ",\"data\":{\"tree\":";   jsonString(o, c->tree().str());
            o << ",\"parent\":";           jsonHashOrNull(o, c->parent());
            o << ",\"message\":";          jsonString(o, c->message());
            o << ",\"timestamp\":";        jsonString(o, isoTime(c->time()));
            o << ",\"author\":";           jsonString(o, c->author());
            o << '}';
        }
        o << '}';
    }

    o << "},\"files\":{";
    first = true;
    for (const auto& [path, content] : snap.files) {
        if (!first) o << ',';
        first = false;
        jsonString(o, path); o << ':'; jsonString(o, content);
    }

    o << "},\"staged\":{";
    first = true;
    for (const auto& [path, h] : snap.staged) {
        if (!first) o << ',';
        first = false;
        jsonString(o, path); o << ':'; jsonString(o, h.str());
    }

    o << "},\"stagedDeleted\":[";
    first = true;
    for (const auto& path : snap.stagedDeleted) {
        if (!first) o << ',';
        first = false;
        jsonString(o, path);
    }
    o << "]}";
    return o.str();
}

std::string HtmlExporter::render(const Snapshot& snap, const std::string& repoName) {
    std::string html = kFrontendTemplate;
    const std::string marker = "/*__MINIGIT_DATA__*/null";
    size_t pos = html.find(marker);
    if (pos == std::string::npos) throw MiniGitException("frontend template is missing its data placeholder");
    html.replace(pos, marker.size(), toJson(snap, repoName));
    return html;
}
