#include "core/json.h"

#include <cstdio>

namespace vitool {
namespace {

class Parser {
public:
    explicit Parser(std::string_view json) : json_(json) {}

    std::optional<JsonObject> Object() {
        JsonObject result;
        if (!Consume('{')) return std::nullopt;
        if (Consume('}')) return End(result);
        do {
            auto key = String();
            if (!key || !Consume(':')) return std::nullopt;
            auto value = String();
            if (!value) return std::nullopt;
            result[*key] = *value;
        } while (Consume(','));
        if (!Consume('}')) return std::nullopt;
        return End(result);
    }

private:
    std::optional<JsonObject> End(JsonObject& result) {
        SkipSpace();
        if (pos_ != json_.size()) return std::nullopt;
        return std::move(result);
    }

    void SkipSpace() {
        while (pos_ < json_.size() && (json_[pos_] == ' ' || json_[pos_] == '\t' || json_[pos_] == '\n' || json_[pos_] == '\r')) ++pos_;
    }

    bool Consume(char c) {
        SkipSpace();
        if (pos_ < json_.size() && json_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    static void AppendUtf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    std::optional<unsigned> Hex4() {
        if (pos_ + 4 > json_.size()) return std::nullopt;
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = json_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
            else return std::nullopt;
        }
        return value;
    }

    std::optional<std::string> String() {
        if (!Consume('"')) return std::nullopt;
        std::string out;
        while (pos_ < json_.size()) {
            const char c = json_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= json_.size()) return std::nullopt;
            switch (json_[pos_++]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    auto code = Hex4();
                    if (!code) return std::nullopt;
                    // Surrogate pairs come as two escapes.
                    if (*code >= 0xD800 && *code < 0xDC00 && pos_ + 2 <= json_.size() && json_[pos_] == '\\' && json_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        auto low = Hex4();
                        if (!low || *low < 0xDC00 || *low >= 0xE000) return std::nullopt;
                        *code = 0x10000 + ((*code - 0xD800) << 10) + (*low - 0xDC00);
                    }
                    AppendUtf8(out, *code);
                    break;
                }
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::string_view json_;
    size_t pos_ = 0;
};

}  // namespace

std::optional<JsonObject> ParseFlatObject(std::string_view json) {
    return Parser(json).Object();
}

std::string JsonQuote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
                    out += escaped;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

}  // namespace vitool
