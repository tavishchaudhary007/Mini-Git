#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

private:
    Type type_{Type::Null};
    bool boolVal_{false};
    double numVal_{0.0};
    int64_t intVal_{0};
    std::string strVal_;
    std::vector<JsonValue> arrVal_;
    std::map<std::string, JsonValue> objVal_;

public:
    JsonValue() : type_(Type::Null) {}
    JsonValue(std::nullptr_t) : type_(Type::Null) {}
    JsonValue(bool b) : type_(Type::Bool), boolVal_(b) {}
    JsonValue(int n) : type_(Type::Number), numVal_(n), intVal_(n) {}
    JsonValue(int64_t n) : type_(Type::Number), numVal_(static_cast<double>(n)), intVal_(n) {}
    JsonValue(double d) : type_(Type::Number), numVal_(d), intVal_(static_cast<int64_t>(d)) {}
    JsonValue(const char* s) : type_(Type::String), strVal_(s ? s : "") {}
    JsonValue(std::string s) : type_(Type::String), strVal_(std::move(s)) {}
    JsonValue(std::vector<JsonValue> arr) : type_(Type::Array), arrVal_(std::move(arr)) {}
    JsonValue(std::map<std::string, JsonValue> obj) : type_(Type::Object), objVal_(std::move(obj)) {}

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool() const { return boolVal_; }
    double as_double() const { return numVal_; }
    int64_t as_int64() const { return intVal_; }
    const std::string& as_string() const { return strVal_; }
    const std::vector<JsonValue>& as_array() const { return arrVal_; }
    const std::map<std::string, JsonValue>& as_object() const { return objVal_; }

    std::vector<JsonValue>& as_array_mut() { return arrVal_; }
    std::map<std::string, JsonValue>& as_object_mut() { return objVal_; }

    bool contains(const std::string& key) const {
        if (type_ != Type::Object) return false;
        return objVal_.find(key) != objVal_.end();
    }

    const JsonValue& operator[](const std::string& key) const;
    const JsonValue& operator[](size_t idx) const;

    std::string serialize() const;
    static JsonValue parse(const std::string& src, std::string* errOut = nullptr);
};
