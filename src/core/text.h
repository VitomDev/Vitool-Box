#pragma once

#include <string>
#include <string_view>

namespace vitool {

std::wstring Utf8ToWide(std::string_view text);
std::string WideToUtf8(std::wstring_view text);
std::wstring ToLower(std::wstring_view text);
bool StartsWith(std::wstring_view text, std::wstring_view prefix);

}  // namespace vitool
