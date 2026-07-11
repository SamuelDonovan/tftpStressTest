#include "metrics/json_writer.hpp"

#include <cmath>
#include <cstdio>

namespace tftp_test_harness::metrics {

JsonValue JsonValue::boolean(bool value) {
    JsonValue v;
    v.type_ = Type::Boolean;
    v.boolean_ = value;
    return v;
}

JsonValue JsonValue::number(double value) {
    JsonValue v;
    v.type_ = Type::Number;
    v.number_ = value;
    v.number_is_integer_ = false;
    return v;
}

JsonValue JsonValue::integer(long long value) {
    JsonValue v;
    v.type_ = Type::Number;
    v.number_ = static_cast<double>(value);
    v.number_is_integer_ = true;
    return v;
}

JsonValue JsonValue::string(std::string value) {
    JsonValue v;
    v.type_ = Type::String;
    v.string_ = std::move(value);
    return v;
}

JsonValue JsonValue::array() {
    JsonValue v;
    v.type_ = Type::Array;
    return v;
}

JsonValue JsonValue::object() {
    JsonValue v;
    v.type_ = Type::Object;
    return v;
}

JsonValue& JsonValue::set(const std::string& key, JsonValue value) {
    type_ = Type::Object;
    for (auto& member : object_) {
        if (member.first == key) {
            member.second = std::move(value);
            return *this;
        }
    }
    object_.emplace_back(key, std::move(value));
    return *this;
}

JsonValue& JsonValue::append(JsonValue value) {
    type_ = Type::Array;
    array_.push_back(std::move(value));
    return *this;
}

bool JsonValue::has(const std::string& key) const {
    for (const auto& member : object_) {
        if (member.first == key) return true;
    }
    return false;
}

const JsonValue& JsonValue::at(const std::string& key) const {
    static const JsonValue null_value;
    for (const auto& member : object_) {
        if (member.first == key) return member.second;
    }
    return null_value;
}

std::string escape_json_string(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 2);
    out.push_back('"');
    for (char raw_char : raw) {
        const unsigned char c = static_cast<unsigned char>(raw_char);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

namespace {
std::string format_number(double value, bool as_integer) {
    if (as_integer && std::floor(value) == value &&
        std::abs(value) < 9.007199254740992e15) {
        return std::to_string(static_cast<long long>(value));
    }
    if (std::floor(value) == value && std::abs(value) < 1e15) {
        return std::to_string(static_cast<long long>(value));
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}
} // namespace

void JsonValue::serialize_into(std::string& out, bool pretty, int depth) const {
    const auto indent = [&](int d) {
        if (pretty) {
            out.push_back('\n');
            out.append(static_cast<std::size_t>(d) * 2, ' ');
        }
    };
    switch (type_) {
        case Type::Null:
            out += "null";
            break;
        case Type::Boolean:
            out += boolean_ ? "true" : "false";
            break;
        case Type::Number:
            out += format_number(number_, number_is_integer_);
            break;
        case Type::String:
            out += escape_json_string(string_);
            break;
        case Type::Array: {
            if (array_.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (std::size_t i = 0; i < array_.size(); ++i) {
                if (i != 0) out.push_back(',');
                indent(depth + 1);
                array_[i].serialize_into(out, pretty, depth + 1);
            }
            indent(depth);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            if (object_.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            for (std::size_t i = 0; i < object_.size(); ++i) {
                if (i != 0) out.push_back(',');
                indent(depth + 1);
                out += escape_json_string(object_[i].first);
                out += pretty ? ": " : ":";
                object_[i].second.serialize_into(out, pretty, depth + 1);
            }
            indent(depth);
            out.push_back('}');
            break;
        }
    }
}

std::string JsonValue::serialize(bool pretty) const {
    std::string out;
    serialize_into(out, pretty, 0);
    return out;
}

// ---------------------------------------------------------------------------
// Minimal recursive-descent JSON parser (reads what this module writes).
// ---------------------------------------------------------------------------
namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    bool parse(JsonValue& out) {
        skip_whitespace();
        if (!parse_value(out)) return false;
        skip_whitespace();
        return position_ >= text_.size(); // trailing garbage is an error
    }

private:
    void skip_whitespace() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++position_;
            } else {
                break;
            }
        }
    }

    bool at_end() const { return position_ >= text_.size(); }
    char peek() const { return text_[position_]; }

    bool parse_value(JsonValue& out) {
        skip_whitespace();
        if (at_end()) return false;
        const char c = peek();
        switch (c) {
            case '{': return parse_object(out);
            case '[': return parse_array(out);
            case '"': {
                std::string s;
                if (!parse_string(s)) return false;
                out = JsonValue::string(std::move(s));
                return true;
            }
            case 't':
            case 'f': return parse_bool(out);
            case 'n': return parse_null(out);
            default: return parse_number(out);
        }
    }

    bool parse_object(JsonValue& out) {
        out = JsonValue::object();
        ++position_; // consume '{'
        skip_whitespace();
        if (!at_end() && peek() == '}') {
            ++position_;
            return true;
        }
        for (;;) {
            skip_whitespace();
            if (at_end() || peek() != '"') return false;
            std::string key;
            if (!parse_string(key)) return false;
            skip_whitespace();
            if (at_end() || peek() != ':') return false;
            ++position_;
            JsonValue value;
            if (!parse_value(value)) return false;
            out.set(key, std::move(value));
            skip_whitespace();
            if (at_end()) return false;
            if (peek() == ',') {
                ++position_;
                continue;
            }
            if (peek() == '}') {
                ++position_;
                return true;
            }
            return false;
        }
    }

    bool parse_array(JsonValue& out) {
        out = JsonValue::array();
        ++position_; // consume '['
        skip_whitespace();
        if (!at_end() && peek() == ']') {
            ++position_;
            return true;
        }
        for (;;) {
            JsonValue value;
            if (!parse_value(value)) return false;
            out.append(std::move(value));
            skip_whitespace();
            if (at_end()) return false;
            if (peek() == ',') {
                ++position_;
                continue;
            }
            if (peek() == ']') {
                ++position_;
                return true;
            }
            return false;
        }
    }

    bool parse_string(std::string& out) {
        out.clear();
        if (at_end() || peek() != '"') return false;
        ++position_; // consume opening quote
        while (!at_end()) {
            const char c = text_[position_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (at_end()) return false;
                const char escaped = text_[position_++];
                switch (escaped) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (position_ + 4 > text_.size()) return false;
                        // Decode a basic-multilingual-plane code point to UTF-8.
                        unsigned code = 0;
                        for (int i = 0; i < 4; ++i) {
                            code <<= 4;
                            const char h = text_[position_++];
                            if (h >= '0' && h <= '9') code |= unsigned(h - '0');
                            else if (h >= 'a' && h <= 'f') code |= unsigned(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= unsigned(h - 'A' + 10);
                            else return false;
                        }
                        if (code < 0x80) {
                            out.push_back(static_cast<char>(code));
                        } else if (code < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default: return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false; // unterminated string
    }

    bool parse_bool(JsonValue& out) {
        if (text_.compare(position_, 4, "true") == 0) {
            position_ += 4;
            out = JsonValue::boolean(true);
            return true;
        }
        if (text_.compare(position_, 5, "false") == 0) {
            position_ += 5;
            out = JsonValue::boolean(false);
            return true;
        }
        return false;
    }

    bool parse_null(JsonValue& out) {
        if (text_.compare(position_, 4, "null") == 0) {
            position_ += 4;
            out = JsonValue();
            return true;
        }
        return false;
    }

    bool parse_number(JsonValue& out) {
        const std::size_t start = position_;
        while (!at_end()) {
            const char c = peek();
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
                c == 'e' || c == 'E') {
                ++position_;
            } else {
                break;
            }
        }
        if (position_ == start) return false;
        const std::string token = text_.substr(start, position_ - start);
        try {
            const double value = std::stod(token);
            if (token.find('.') == std::string::npos &&
                token.find('e') == std::string::npos &&
                token.find('E') == std::string::npos) {
                out = JsonValue::integer(static_cast<long long>(value));
            } else {
                out = JsonValue::number(value);
            }
        } catch (...) {
            return false;
        }
        return true;
    }

    const std::string& text_;
    std::size_t position_ = 0;
};

} // namespace

bool parse_json(const std::string& text, JsonValue& out) {
    Parser parser(text);
    return parser.parse(out);
}

} // namespace tftp_test_harness::metrics
