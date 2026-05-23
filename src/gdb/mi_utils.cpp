#include "mi_utils.hpp"

namespace {

void append_unescaped_mi_string(std::string_view s, std::string &out) {
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
}

std::optional<std::string_view> mi_stream_payload_view(std::string_view line) {
    if (line.size() < 3) {
        return std::nullopt;
    }
    char kind = line[0];
    if (kind != '~' && kind != '@' && kind != '&') {
        return std::nullopt;
    }
    auto first = line.find('"');
    auto last = line.rfind('"');
    if (first == std::string_view::npos || last == std::string_view::npos || first == last) {
        return std::nullopt;
    }
    return line.substr(first + 1, last - first - 1);
}

} // namespace

std::string mi_quote(std::string_view s) {
    std::string out = "\"";
    out.reserve(s.size() + 2);
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
    append_unescaped_mi_string(s, out);
    return out;
}

std::optional<std::string> mi_stream_payload(std::string_view line) {
    auto payload = mi_stream_payload_view(line);
    if (!payload) {
        return std::nullopt;
    }
    return unescape_mi_string(*payload);
}

std::string field_value(std::string_view line, std::string_view name) {
    std::string needle;
    needle.reserve(name.size() + 2);
    needle.append(name);
    needle.append("=\"");
    auto pos = line.find(needle);
    if (pos == std::string_view::npos) {
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
    size_t total_size = lines.size();
    for (const auto &line : lines) {
        total_size += line.size();
    }

    std::string out;
    out.reserve(total_size);
    for (const auto &line : lines) {
        out.append(line);
        out.push_back('\n');
    }
    return out;
}

std::string decoded_streams(const std::vector<std::string> &lines) {
    size_t reserve_size = 0;
    for (const auto &line : lines) {
        reserve_size += line.size();
    }

    std::string out;
    out.reserve(reserve_size);
    for (const auto &line : lines) {
        auto payload = mi_stream_payload_view(line);
        if (!payload) {
            continue;
        }
        size_t before = out.size();
        append_unescaped_mi_string(*payload, out);
        if (out.size() != before && out.back() != '\n') {
            out.push_back('\n');
        }
    }
    return out.empty() ? joined_raw(lines) : out;
}
