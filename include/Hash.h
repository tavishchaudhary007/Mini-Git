#pragma once
#include <functional>
#include <ostream>
#include <string>
#include <utility>

// Value type wrapping a hex digest. Demonstrates operator overloading.
inline bool isValidHash(const std::string& s) {
    if (s.size() != 40) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

class Hash {
    std::string value_;
public:
    Hash() = default;
    explicit Hash(std::string v) : value_(std::move(v)) {}

    const std::string& str() const { return value_; }
    std::string shortStr(size_t n = 7) const { return value_.substr(0, n); }
    bool empty() const { return value_.empty(); }

    bool operator==(const Hash& o) const { return value_ == o.value_; }
    bool operator!=(const Hash& o) const { return !(*this == o); }
    bool operator<(const Hash& o)  const { return value_ < o.value_; }
    friend std::ostream& operator<<(std::ostream& os, const Hash& h) { return os << h.value_; }
};

namespace std {
template <>
struct hash<Hash> {
    size_t operator()(const Hash& h) const noexcept {
        return hash<std::string>()(h.str());
    }
};
}
