#include "mi_utils.hpp"

#include "../common/string_utils.hpp"

#include <sstream>

std::string mi_quote(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

std::string unescape_mi_string(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c != '\\' || i + 1 >= s.size()) {
            out.push_back(c);
            continue;
        }
        char n = s[++i];
        switch (n) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            default: out.push_back(n); break;
        }
    }
    return out;
}

std::optional<std::string> mi_stream_payload(const std::string &line) {
    if (line.size() < 3) {
        return std::nullopt;
    }
    char kind = line[0];
    if (kind != '~' && kind != '@' && kind != '&') {
        return std::nullopt;
    }
    auto first = line.find('"');
    auto last = line.rfind('"');
    if (first == std::string::npos || last == std::string::npos || first == last) {
        return std::nullopt;
    }
    return unescape_mi_string(std::string_view(line).substr(first + 1, last - first - 1));
}

std::string field_value(const std::string &line, const std::string &name) {
    std::string needle = name + "=\"";
    auto pos = line.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    pos += needle.size();
    std::string out;
    bool escaped = false;
    for (; pos < line.size(); ++pos) {
        char c = line[pos];
        if (escaped) {
            out.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            break;
        }
        out.push_back(c);
    }
    return out;
}

std::string joined_raw(const std::vector<std::string> &lines) {
    std::ostringstream out;
    for (const auto &line : lines) {
        out << line << '\n';
    }
    return out.str();
}

std::string decoded_streams(const std::vector<std::string> &lines) {
    std::ostringstream out;
    for (const auto &line : lines) {
        if (auto payload = mi_stream_payload(line)) {
            out << *payload;
            if (!payload->empty() && payload->back() != '\n') {
                out << '\n';
            }
        }
    }
    std::string decoded = out.str();
    return decoded.empty() ? joined_raw(lines) : decoded;
}

