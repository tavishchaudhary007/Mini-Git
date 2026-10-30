#pragma once
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "Exceptions.h"
#include "PathSafety.h"
#include <atomic>
#include <chrono>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace fs = std::filesystem;

inline int processExitCode(int status) {
#if defined(_WIN32)
    return status;
#else
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return status;
#endif
}

inline std::string isoTime(std::time_t t) {
    char buf[32];
    std::tm* tm = std::gmtime(&t);
    if (!tm) return "1970-01-01T00:00:00Z";
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", tm);
    return buf;
}

inline std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw FileNotFoundException("file not found: " + p.generic_string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline void writeFile(const fs::path& p, const std::string& data) {
    if (isLinkOrSymlink(p)) {
        throw MiniGitException("refusing to write through symlink at destination: " + p.generic_string());
    }
    fs::path parent = p.has_parent_path() ? p.parent_path() : fs::current_path();
    if (p.has_parent_path()) {
        fs::create_directories(parent);
    }
    if (isLinkOrSymlink(parent)) {
        throw MiniGitException("refusing to write into symlinked directory: " + parent.generic_string());
    }

    static std::atomic<uint64_t> tmpCounter{0};
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::string tmpName = p.filename().string() + ".tmp." + std::to_string(now) + "." + std::to_string(++tmpCounter);
    fs::path tmpPath = parent / tmpName;

    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw MiniGitException("cannot write file: " + p.generic_string());
        }
        if (!data.empty()) {
            out.write(data.data(), data.size());
        }
        out.flush();
        if (!out.good()) {
            out.close();
            std::error_code ec;
            fs::remove(tmpPath, ec);
            throw MiniGitException("stream error while writing file: " + p.generic_string());
        }
    }

    if (isLinkOrSymlink(p)) {
        std::error_code ec;
        fs::remove(tmpPath, ec);
        throw MiniGitException("refusing to overwrite symlink at destination: " + p.generic_string());
    }

#if defined(_WIN32)
    if (!MoveFileExW(p.wstring().c_str(), nullptr, 0)) {
        // Destination might not exist yet, which is fine
    }
    if (!MoveFileExW(tmpPath.wstring().c_str(), p.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        fs::remove(tmpPath, ec);
        throw MiniGitException("cannot rename temp file to: " + p.generic_string());
    }
#else
    std::error_code ec;
    fs::rename(tmpPath, p, ec);
    if (ec) {
        fs::remove(tmpPath, ec);
        throw MiniGitException("cannot rename temp file to: " + p.generic_string() + ": " + ec.message());
    }
#endif
}

inline std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

inline std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

inline bool hasIllegalPathChars(const std::string& path) {
    for (char c : path) {
        if (static_cast<unsigned char>(c) < 32 || c == 127 ||
            c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            return true;
        }
    }
    return false;
}

inline int runProcess(const std::vector<std::string>& args, std::string* output = nullptr) {
    if (args.empty()) return -1;
#if defined(_WIN32)
    std::string cmdLine;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) cmdLine += " ";
        const std::string& a = args[i];
        bool needQuote = a.empty() || a.find_first_of(" \t\n\v\"") != std::string::npos;
        if (!needQuote) {
            cmdLine += a;
        } else {
            cmdLine += "\"";
            for (size_t j = 0; j < a.size(); ++j) {
                if (a[j] == '\"') cmdLine += "\\\"";
                else if (a[j] == '\\') {
                    size_t k = j;
                    while (k < a.size() && a[k] == '\\') ++k;
                    if (k == a.size() || a[k] == '\"') {
                        cmdLine.append((k - j) * 2, '\\');
                    } else {
                        cmdLine.append(k - j, '\\');
                    }
                    j = k - 1;
                } else {
                    cmdLine += a[j];
                }
            }
            cmdLine += "\"";
        }
    }

    HANDLE hReadPipe = NULL;
    HANDLE hWritePipe = NULL;
    HANDLE hNull = NULL;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (output) {
        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return -1;
        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);
    } else {
        hNull = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, NULL);
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    if (output) {
        si.hStdOutput = hWritePipe;
        si.hStdError = hWritePipe;
    } else {
        si.hStdOutput = (hNull != INVALID_HANDLE_VALUE) ? hNull : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = (hNull != INVALID_HANDLE_VALUE) ? hNull : GetStdHandle(STD_ERROR_HANDLE);
    }
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    BOOL ok = CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, TRUE,
                             0, NULL, NULL, &si, &pi);
    if (hNull && hNull != INVALID_HANDLE_VALUE) {
        CloseHandle(hNull);
    }
    if (output) {
        CloseHandle(hWritePipe);
    }

    if (!ok) {
        if (output) CloseHandle(hReadPipe);
        return -1;
    }

    if (output) {
        char buf[4096];
        DWORD bytesRead = 0;
        while (ReadFile(hReadPipe, buf, sizeof(buf), &bytesRead, NULL) && bytesRead > 0) {
            output->append(buf, bytesRead);
        }
        CloseHandle(hReadPipe);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);

#else
    int pipefd[2] = {-1, -1};
    if (output) {
        if (pipe(pipefd) != 0) return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (output) { close(pipefd[0]); close(pipefd[1]); }
        return -1;
    }

    if (pid == 0) {
        if (output) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
        } else {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        std::vector<char*> argv;
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    if (output) {
        close(pipefd[1]);
        char buf[4096];
        ssize_t n = 0;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            output->append(buf, n);
        }
        close(pipefd[0]);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

inline bool openInBrowser(const std::string& target) {
#if defined(_WIN32)
    HINSTANCE h = ShellExecuteA(NULL, "open", target.c_str(), NULL, NULL, SW_SHOWNORMAL);
    return (reinterpret_cast<INT_PTR>(h) > 32);
#elif defined(__APPLE__)
    return runProcess({"open", target}) == 0;
#else
    return runProcess({"xdg-open", target}) == 0;
#endif
}
