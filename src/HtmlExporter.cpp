#include "HtmlExporter.h"
#include <cstdio>
#include <ctime>
#include <sstream>
#include "Base64.h"
#include "FrontendTemplate.h"
#include "Hasher.h"
#include "Json.h"

namespace {

void jsonHashOrNull(std::ostream& os, const Hash& h) {
    if (h.empty()) os << "null"; else jsonString(os, h.str());
}

}  // namespace

std::string HtmlExporter::computeVersion(const Snapshot& snap) {
    std::ostringstream ss;
    ss << "branch:" << snap.currentBranch << "\n";
    ss << "head:" << snap.head.str() << "\n";
    ss << "detached:" << (snap.detached ? "1" : "0") << "\n";
    ss << "merging:" << (snap.merging ? "1" : "0") << "\n";
    for (const auto& [b, tip] : snap.branches) {
        ss << "b:" << b << "=" << tip.str() << "\n";
    }
    for (const auto& [p, tip] : snap.staged) {
        ss << "s:" << p << "=" << tip.str() << "\n";
    }
    for (const auto& p : snap.stagedDeleted) {
        ss << "sd:" << p << "\n";
    }
    for (const auto& [p, st] : snap.stagedStatus) {
        ss << "ss:" << p << "=" << st << "\n";
    }
    for (const auto& c : snap.conflicts) {
        ss << "c:" << c << "\n";
    }
    for (const auto& f : snap.status.modified) {
        ss << "m:" << f << "\n";
    }
    for (const auto& f : snap.status.deleted) {
        ss << "d:" << f << "\n";
    }
    for (const auto& f : snap.status.untracked) {
        ss << "u:" << f << "\n";
    }
    for (const auto& [p, content] : snap.files) {
        ss << "f:" << p << "=" << content << "\n";
    }
    for (const auto& bf : snap.binaryFiles) {
        ss << "bf:" << bf << "\n";
    }
    return Hasher::sha1(ss.str());
}

std::string HtmlExporter::toJson(const Snapshot& snap, const std::string& repoName, bool isSyncBundle) {
    std::ostringstream o;

    std::string current = snap.currentBranch;
    std::map<std::string, Hash> branches = snap.branches;
    if (!current.empty() && !branches.count(current)) {
        branches[current] = Hash();
    }

    std::string version = computeVersion(snap);

    o << "{\"repoName\":";      jsonString(o, repoName);
    o << ",\"generated\":";     jsonString(o, isoTime(std::time(nullptr)));
    o << ",\"currentBranch\":"; jsonString(o, current);
    o << ",\"head\":";          jsonHashOrNull(o, snap.head);
    o << ",\"detached\":" <<    (snap.detached ? "true" : "false");
    o << ",\"merging\":" <<     (snap.merging ? "true" : "false");

    o << ",\"conflicts\":[";
    bool firstConflict = true;
    for (const auto& c : snap.conflicts) {
        if (!firstConflict) o << ',';
        firstConflict = false;
        jsonString(o, c);
    }
    o << "],\"stagedStatus\":{";
    bool firstStatus = true;
    for (const auto& [path, st] : snap.stagedStatus) {
        if (!firstStatus) o << ',';
        firstStatus = false;
        jsonString(o, path);
        o << ":\"" << st << "\"";
    }
    o << "},\"version\":"; jsonString(o, version);
    o << ",\"lan\":" << (snap.lan ? "true" : "false");
    o << ",\"lanUrl\":"; jsonString(o, snap.lanUrl);
    o << ",\"binaryFiles\":[";
    bool firstBin = true;
    for (const auto& bf : snap.binaryFiles) {
        if (!firstBin) o << ',';
        firstBin = false;
        jsonString(o, bf);
    }
    o << "]";

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
            if (isSyncBundle && b->content().find('\0') != std::string::npos) {
                o << ",\"encoding\":"; jsonString(o, "base64");
                o << ",\"content\":";  jsonString(o, Base64::encode(b->content()));
            } else {
                o << ",\"content\":";
                if (b->content().find('\0') != std::string::npos) {
                    jsonString(o, "[binary file]");
                } else {
                    jsonString(o, b->content());
                }
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
            if (c->hasParent2()) {
                o << ",\"parent2\":";      jsonHashOrNull(o, c->parent2());
            }
            o << ",\"message\":";          jsonString(o, c->message());
            o << ",\"timestamp\":";        jsonString(o, isoTime(c->time()));
            o << ",\"time\":";             o << static_cast<long long>(c->time());
            o << ",\"author\":";           jsonString(o, c->author());
            o << '}';
        }
        o << '}';
    }

    o << "}";

    if (!isSyncBundle) {
        o << ",\"files\":{";
        first = true;
        for (const auto& [path, content] : snap.files) {
            if (!first) o << ',';
            first = false;
            jsonString(o, path); o << ':'; jsonString(o, content);
        }
        o << "}";
    }

    o << ",\"staged\":{";
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
    o << "]";

    if (!isSyncBundle) {
        o << ",\"status\":{";
        o << "\"staged\":[";
        bool fst = true;
        for (const auto& f : snap.status.staged) {
            if (!fst) o << ',';
            fst = false;
            jsonString(o, f);
        }
        o << "],\"modified\":[";
        fst = true;
        for (const auto& f : snap.status.modified) {
            if (!fst) o << ',';
            fst = false;
            jsonString(o, f);
        }
        o << "],\"deleted\":[";
        fst = true;
        for (const auto& f : snap.status.deleted) {
            if (!fst) o << ',';
            fst = false;
            jsonString(o, f);
        }
        o << "],\"untracked\":[";
        fst = true;
        for (const auto& f : snap.status.untracked) {
            if (!fst) o << ',';
            fst = false;
            jsonString(o, f);
        }
        o << "]}";
    }

    o << "}";
    return o.str();
}

std::string HtmlExporter::render(const Snapshot& snap, const std::string& repoName, bool isStaticExport) {
    std::string html = kFrontendTemplate;
    if (isStaticExport) {
        const std::string scriptTag = "<script>";
        size_t sPos = html.find(scriptTag);
        if (sPos != std::string::npos) {
            html.insert(sPos + scriptTag.size(), "\nwindow.__staticExport = true;\n");
        }
    }
    const std::string marker = "/*__MINIGIT_DATA__*/null";
    size_t pos = html.find(marker);
    if (pos == std::string::npos) throw MiniGitException("frontend template is missing its data placeholder");
    html.replace(pos, marker.size(), toJson(snap, repoName));
    return html;
}
