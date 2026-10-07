#pragma once
#include <string>

// Hand-written SHA-1 (no external libraries).
class Hasher {
public:
    static std::string sha1(const std::string& data);
};
