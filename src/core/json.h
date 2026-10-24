// Just enough JSON for the messages exchanged with the web UI: a parser for
// flat objects of strings, and a string escaper for the replies.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace vitool {

using JsonObject = std::map<std::string, std::string>;

// Parses {"key": "value", ...}. Non-string values are rejected.
std::optional<JsonObject> ParseFlatObject(std::string_view json);

// Returns the text as a quoted JSON string.
std::string JsonQuote(std::string_view text);

}  // namespace vitool
