#pragma once
#include <string>

namespace Base64 {

std::string encode(const std::string& in);
bool decode(const std::string& in, std::string& out, std::string& err);

} // namespace Base64
