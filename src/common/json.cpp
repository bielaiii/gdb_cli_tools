#include "json.hpp"

#include <cctype>
#include <sstream>

namespace {

class Parser {
public:
    explicit Parser(const std::string &text) : text_(text) {}

    Json parse() {
        skip_ws();
        Json value = parse_value();
        skip_ws();
        if (pos_ != text_.size()) {
            throw std::runtime_error("unexpected trailing JSON input");
        }
        return value;
    }

private:
    Json parse_value() {
        skip_ws();
        if (pos_ >= text_.size()) {
            throw std::runtime_error("unexpected end of JSON input");
        }
        char c = text_[pos_];
        if (c == '"') {
            Json json;
            json.type = Json::Type::String;
            json.string_value = parse_string();
            return json;
        }
        if (c == '{') {
            return parse_object();
        }
        if (c == '[') {
            return parse_array();
        }
        if (c == 't' || c == 'f') {
            return parse_bool();
        }
        if (c == 'n') {
            return parse_null();
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return parse_number();
        }
        throw std::runtime_error("invalid JSON value");
    }

    Json parse_object() {
        expect('{');
        Json json;
        json.type = Json::Type::Object;
        skip_ws();
        if (peek('}')) {
            ++pos_;
            return json;
        }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            expect(':');
            json.object_value.emplace(std::move(key), parse_value());
            skip_ws();
            if (peek('}')) {
                ++pos_;
                return json;
            }
            expect(',');
        }
    }

    Json parse_array() {
        expect('[');
        Json json;
        json.type = Json::Type::Array;
        skip_ws();
        if (peek(']')) {
            ++pos_;
            return json;
        }
        while (true) {
            json.array_value.push_back(parse_value());
            skip_ws();
            if (peek(']')) {
                ++pos_;
                return json;
            }
            expect(',');
        }
    }

    Json parse_bool() {
        Json json;
        json.type = Json::Type::Bool;
        if (text_.compare(pos_, 4, "true") == 0) {
            json.bool_value = true;
            pos_ += 4;
            return json;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            json.bool_value = false;
            pos_ += 5;
            return json;
        }
        throw std::runtime_error("invalid JSON bool");
    }

    Json parse_null() {
        if (text_.compare(pos_, 4, "null") != 0) {
            throw std::runtime_error("invalid JSON null");
        }
        pos_ += 4;
        return {};
    }

    Json parse_number() {
        size_t start = pos_;
        if (peek('-')) {
            ++pos_;
        }
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
        if (peek('.')) {
            ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            }
        }
        Json json;
        json.type = Json::Type::Number;
        json.number_value = std::stod(text_.substr(start, pos_ - start));
        return json;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) {
                throw std::runtime_error("unterminated JSON escape");
            }
            char e = text_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: throw std::runtime_error("unsupported JSON escape");
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }

    void skip_ws() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    bool peek(char c) const {
        return pos_ < text_.size() && text_[pos_] == c;
    }

    void expect(char c) {
        skip_ws();
        if (!peek(c)) {
            throw std::runtime_error("unexpected JSON character");
        }
        ++pos_;
    }

    const std::string &text_;
    size_t pos_ = 0;
};

static std::string escape_string(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

} // namespace

const Json *Json::find(const std::string &key) const {
    if (!is_object()) {
        return nullptr;
    }
    auto it = object_value.find(key);
    if (it == object_value.end()) {
        return nullptr;
    }
    return &it->second;
}

std::string Json::string_or(const std::string &key, const std::string &fallback) const {
    const Json *value = find(key);
    if (value == nullptr || !value->is_string()) {
        return fallback;
    }
    return value->string_value;
}

int Json::int_or(const std::string &key, int fallback) const {
    const Json *value = find(key);
    if (value == nullptr || !value->is_number()) {
        return fallback;
    }
    return static_cast<int>(value->number_value);
}

bool Json::bool_or(const std::string &key, bool fallback) const {
    const Json *value = find(key);
    if (value == nullptr || !value->is_bool()) {
        return fallback;
    }
    return value->bool_value;
}

Json parse_json(const std::string &text) {
    return Parser(text).parse();
}

std::string dump_json(const Json &json) {
    switch (json.type) {
        case Json::Type::Null:
            return "null";
        case Json::Type::Bool:
            return json.bool_value ? "true" : "false";
        case Json::Type::Number: {
            std::ostringstream out;
            out << json.number_value;
            return out.str();
        }
        case Json::Type::String:
            return escape_string(json.string_value);
        case Json::Type::Array: {
            std::string out = "[";
            for (size_t i = 0; i < json.array_value.size(); ++i) {
                if (i != 0) {
                    out += ",";
                }
                out += dump_json(json.array_value[i]);
            }
            out += "]";
            return out;
        }
        case Json::Type::Object: {
            std::string out = "{";
            bool first = true;
            for (const auto &[key, value] : json.object_value) {
                if (!first) {
                    out += ",";
                }
                first = false;
                out += escape_string(key);
                out += ":";
                out += dump_json(value);
            }
            out += "}";
            return out;
        }
    }
    return "null";
}

