#include "Json.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

const JsonValue& JsonValue::operator[](const std::string& key) const {
    static const JsonValue nullVal;
    if (type_ != Type::Object) return nullVal;
    auto it = objVal_.find(key);
    if (it != objVal_.end()) return it->second;
    return nullVal;
}

const JsonValue& JsonValue::operator[](size_t idx) const {
    static const JsonValue nullVal;
    if (type_ != Type::Array || idx >= arrVal_.size()) return nullVal;
    return arrVal_[idx];
}

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
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    os << buf;
                } else {
                    os << static_cast<char>(c);
                }
                break;
        }
    }
    os << '"';
}

std::string jsonString(const std::string& s) {
    std::ostringstream oss;
    jsonString(oss, s);
    return oss.str();
}

static void escapeJsonString(std::ostream& os, const std::string& s) {
    jsonString(os, s);
}

std::string JsonValue::serialize() const {
    std::ostringstream os;
    switch (type_) {
        case Type::Null:
            os << "null";
            break;
        case Type::Bool:
            os << (boolVal_ ? "true" : "false");
            break;
        case Type::Number:
            if (std::floor(numVal_) == numVal_ && !std::isinf(numVal_) && numVal_ >= -9e18 && numVal_ <= 9e18) {
                os << intVal_;
            } else {
                os << numVal_;
            }
            break;
        case Type::String:
            escapeJsonString(os, strVal_);
            break;
        case Type::Array: {
            os << '[';
            for (size_t i = 0; i < arrVal_.size(); ++i) {
                if (i > 0) os << ',';
                os << arrVal_[i].serialize();
            }
            os << ']';
            break;
        }
        case Type::Object: {
            os << '{';
            bool first = true;
            for (const auto& [k, v] : objVal_) {
                if (!first) os << ',';
                first = false;
                escapeJsonString(os, k);
                os << ':' << v.serialize();
            }
            os << '}';
            break;
        }
    }
    return os.str();
}

namespace {

class JsonParser {
    const std::string& src_;
    size_t pos_{0};
    std::string err_;
    int depth_{0};
    static constexpr int MAX_DEPTH = 64;

    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& depth) : d(depth) { ++d; }
        ~DepthGuard() { --d; }
    };

    void setError(const std::string& msg) {
        if (err_.empty()) {
            err_ = "at pos " + std::to_string(pos_) + ": " + msg;
        }
    }

    void skipWhitespace() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                pos_++;
            } else {
                break;
            }
        }
    }

    char peek() {
        skipWhitespace();
        if (pos_ >= src_.size()) return '\0';
        return src_[pos_];
    }

    char get() {
        skipWhitespace();
        if (pos_ >= src_.size()) return '\0';
        return src_[pos_++];
    }

    static void encodeUtf8(uint32_t cp, std::string& out) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0x10FFFF) {
            out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseHex4(uint32_t& val) {
        val = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ >= src_.size()) return false;
            char c = src_[pos_++];
            val <<= 4;
            if (c >= '0' && c <= '9') val |= (c - '0');
            else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool parseStringRaw(std::string& out) {
        out.clear();
        if (pos_ >= src_.size() || src_[pos_] != '"') {
            setError("expected string opening quote");
            return false;
        }
        pos_++; // skip opening '"'

        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                setError("unescaped control character in string");
                return false;
            }
            if (c == '\\') {
                if (pos_ >= src_.size()) {
                    setError("unexpected end of string escape");
                    return false;
                }
                char esc = src_[pos_++];
                switch (esc) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!parseHex4(cp)) {
                            setError("invalid \\u hex escape");
                            return false;
                        }
                        // Check for surrogate pairs: 0xD800 - 0xDBFF
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (pos_ + 2 <= src_.size() && src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                                pos_ += 2;
                                uint32_t low = 0;
                                if (parseHex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                                    cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                                } else {
                                    // Invalid low surrogate; output high surrogate directly
                                }
                            }
                        }
                        encodeUtf8(cp, out);
                        break;
                    }
                    default:
                        setError(std::string("unknown escape \\") + esc);
                        return false;
                }
            } else {
                out.push_back(c);
            }
        }

        setError("unterminated string");
        return false;
    }

    bool parseNumber(JsonValue& out) {
        size_t start = pos_;
        if (pos_ < src_.size() && src_[pos_] == '-') pos_++;

        bool hasDigits = false;
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
            pos_++;
            hasDigits = true;
        }
        if (!hasDigits) {
            setError("expected digits in number");
            return false;
        }

        bool isFloat = false;
        if (pos_ < src_.size() && src_[pos_] == '.') {
            isFloat = true;
            pos_++;
            if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                setError("expected digits after decimal point");
                return false;
            }
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                pos_++;
            }
        }

        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            isFloat = true;
            pos_++;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) pos_++;
            if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                setError("expected digits in exponent");
                return false;
            }
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                pos_++;
            }
        }

        std::string numStr = src_.substr(start, pos_ - start);
        try {
            if (isFloat) {
                out = JsonValue(std::stod(numStr));
            } else {
                out = JsonValue(static_cast<int64_t>(std::stoll(numStr)));
            }
            return true;
        } catch (...) {
            setError("number out of range: " + numStr);
            return false;
        }
    }

    bool parseArray(JsonValue& out) {
        if (depth_ >= MAX_DEPTH) {
            setError("maximum nesting depth exceeded (max 64)");
            return false;
        }
        DepthGuard guard(depth_);
        pos_++; // skip '['
        std::vector<JsonValue> arr;
        skipWhitespace();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            pos_++;
            out = JsonValue(std::move(arr));
            return true;
        }

        while (pos_ < src_.size()) {
            JsonValue elem;
            if (!parseValue(elem)) return false;
            arr.push_back(std::move(elem));

            skipWhitespace();
            if (pos_ >= src_.size()) break;
            if (src_[pos_] == ']') {
                pos_++;
                out = JsonValue(std::move(arr));
                return true;
            }
            if (src_[pos_] == ',') {
                pos_++;
                continue;
            }
            setError("expected ',' or ']' in array");
            return false;
        }

        setError("unterminated array");
        return false;
    }

    bool parseObject(JsonValue& out) {
        if (depth_ >= MAX_DEPTH) {
            setError("maximum nesting depth exceeded (max 64)");
            return false;
        }
        DepthGuard guard(depth_);
        pos_++; // skip '{'
        std::map<std::string, JsonValue> obj;
        skipWhitespace();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            pos_++;
            out = JsonValue(std::move(obj));
            return true;
        }

        while (pos_ < src_.size()) {
            skipWhitespace();
            if (pos_ >= src_.size() || src_[pos_] != '"') {
                setError("expected string key in object");
                return false;
            }
            std::string key;
            if (!parseStringRaw(key)) return false;

            skipWhitespace();
            if (pos_ >= src_.size() || src_[pos_] != ':') {
                setError("expected ':' after key in object");
                return false;
            }
            pos_++; // skip ':'

            JsonValue val;
            if (!parseValue(val)) return false;
            obj[std::move(key)] = std::move(val);

            skipWhitespace();
            if (pos_ >= src_.size()) break;
            if (src_[pos_] == '}') {
                pos_++;
                out = JsonValue(std::move(obj));
                return true;
            }
            if (src_[pos_] == ',') {
                pos_++;
                continue;
            }
            setError("expected ',' or '}' in object");
            return false;
        }

        setError("unterminated object");
        return false;
    }

public:
    explicit JsonParser(const std::string& src) : src_(src) {}

    bool parseValue(JsonValue& out) {
        skipWhitespace();
        if (pos_ >= src_.size()) {
            setError("unexpected end of input");
            return false;
        }

        char c = src_[pos_];
        if (c == 'n') {
            if (src_.compare(pos_, 4, "null") == 0) {
                pos_ += 4;
                out = JsonValue(nullptr);
                return true;
            }
            setError("invalid literal (expected null)");
            return false;
        }
        if (c == 't') {
            if (src_.compare(pos_, 4, "true") == 0) {
                pos_ += 4;
                out = JsonValue(true);
                return true;
            }
            setError("invalid literal (expected true)");
            return false;
        }
        if (c == 'f') {
            if (src_.compare(pos_, 5, "false") == 0) {
                pos_ += 5;
                out = JsonValue(false);
                return true;
            }
            setError("invalid literal (expected false)");
            return false;
        }
        if (c == '"') {
            std::string s;
            if (!parseStringRaw(s)) return false;
            out = JsonValue(std::move(s));
            return true;
        }
        if (c == '[') {
            return parseArray(out);
        }
        if (c == '{') {
            return parseObject(out);
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return parseNumber(out);
        }

        setError(std::string("unexpected character '") + c + "'");
        return false;
    }

    const std::string& error() const { return err_; }
    size_t pos() const { return pos_; }
};

} // namespace

JsonValue JsonValue::parse(const std::string& src, std::string* errOut) {
    JsonParser parser(src);
    JsonValue out;
    if (!parser.parseValue(out)) {
        if (errOut) *errOut = parser.error();
        return JsonValue();
    }
    // Check trailing characters
    size_t p = parser.pos();
    while (p < src.size()) {
        char c = src[p];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            p++;
        } else {
            if (errOut) *errOut = "trailing non-whitespace at pos " + std::to_string(p);
            return JsonValue();
        }
    }
    if (errOut) errOut->clear();
    return out;
}
