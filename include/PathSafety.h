#pragma once
#include <string>
#include <cctype>
#include <filesystem>
#include <vector>
#include <cstdio>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#endif

inline std::string escapePath(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c < 0x20 || c == 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned int>(c));
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

inline bool isLinkOrSymlink(const std::filesystem::path& p) {
    std::error_code ec;
    if (std::filesystem::is_symlink(p, ec)) return true;
#ifdef _WIN32
    DWORD attr = GetFileAttributesW(p.wstring().c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return true;
    }
#endif
    return false;
}

inline void ensureNoSymlinkComponents(const std::filesystem::path& root, const std::filesystem::path& relPath) {
    std::filesystem::path curr = root;
    for (const auto& part : relPath) {
        curr /= part;
        if (isLinkOrSymlink(curr)) {
            throw MiniGitException("path component is a symlink: " + relPath.generic_string());
        }
    }
}

inline bool isSafeRepoPath(const std::string& path) {
    // Rejects empty paths
    if (path.empty()) return false;

    // Rejects NUL and control characters
    for (char c : path) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc == 127) return false;
    }

    // Rejects backslashes
    if (path.find('\\') != std::string::npos) return false;

    // Rejects drive letters (e.g. C:, c:, or contains :)
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') {
        return false;
    }
    if (path.find(':') != std::string::npos) {
        return false;
    }

    // Rejects absolute paths (leading slash or filesystem is_absolute / root)
    if (path.front() == '/') return false;
    std::filesystem::path fsPath(path);
    if (fsPath.is_absolute() || fsPath.has_root_name() || fsPath.has_root_directory()) {
        return false;
    }

    // Split into segments by '/'
    // Rejects empty segments, "." and ".." segments, and any segment equal to .minigit (case-insensitive)
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        std::string seg;
        if (slash == std::string::npos) {
            seg = path.substr(start);
            start = path.size() + 1;
        } else {
            seg = path.substr(start, slash - start);
            start = slash + 1;
        }

        if (seg.empty()) return false;
        if (seg == "." || seg == "..") return false;

        std::string lowerSeg = seg;
        for (char& ch : lowerSeg) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (lowerSeg == ".minigit") return false;
    }

    return true;
}

inline void pruneEmptyParents(const std::filesystem::path& root, const std::filesystem::path& p) {
    std::error_code ec;
    for (std::filesystem::path d = p.parent_path(); d != root && !d.empty() && std::filesystem::is_directory(d, ec) && std::filesystem::is_empty(d, ec); d = d.parent_path()) {
        std::filesystem::remove(d, ec);
    }
}
