#include "mi_utils.hpp"

#include <cctype>
#include <sstream>
#include <string>

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

class MiValueParser {
public:
    explicit MiValueParser(std::string_view text) : text_(text) {}

    std::optional<MiValue> parse() {
        skip_space();
        auto value = parse_value();
        skip_space();
        if (!value || pos_ != text_.size()) {
            return std::nullopt;
        }
        return value;
    }

private:
    std::optional<MiValue> parse_value() {
        skip_space();
        if (pos_ >= text_.size()) {
            return std::nullopt;
        }
        if (text_[pos_] == '"') {
            return parse_string();
        }
        if (text_[pos_] == '{') {
            return parse_tuple();
        }
        if (text_[pos_] == '[') {
            return parse_list();
        }
        return parse_bare();
    }

    std::optional<MiValue> parse_string() {
        ++pos_;
        std::string raw;
        bool escaped = false;
        for (; pos_ < text_.size(); ++pos_) {
            char c = text_[pos_];
            if (escaped) {
                raw.push_back('\\');
                raw.push_back(c);
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"') {
                ++pos_;
                MiValue value;
                value.kind = MiValue::Kind::String;
                value.value = unescape_mi_string(raw);
                return value;
            }
            raw.push_back(c);
        }
        return std::nullopt;
    }

    std::optional<MiValue> parse_tuple() {
        ++pos_;
        MiValue tuple;
        tuple.kind = MiValue::Kind::Tuple;
        skip_space();
        if (consume('}')) {
            return tuple;
        }
        while (pos_ < text_.size()) {
            auto key = parse_identifier();
            if (key.empty() || !consume('=')) {
                return std::nullopt;
            }
            auto value = parse_value();
            if (!value) {
                return std::nullopt;
            }
            tuple.fields.push_back({std::move(key), std::move(*value)});
            skip_space();
            if (consume('}')) {
                return tuple;
            }
            if (!consume(',')) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<MiValue> parse_list() {
        ++pos_;
        MiValue list;
        list.kind = MiValue::Kind::List;
        skip_space();
        if (consume(']')) {
            return list;
        }
        while (pos_ < text_.size()) {
            size_t saved = pos_;
            std::string key = parse_identifier();
            skip_space();
            if (!key.empty() && consume('=')) {
                MiValue tuple_item;
                tuple_item.kind = MiValue::Kind::Tuple;
                auto value = parse_value();
                if (!value) {
                    return std::nullopt;
                }
                tuple_item.fields.push_back({std::move(key), std::move(*value)});
                list.items.push_back(std::move(tuple_item));
            } else {
                pos_ = saved;
                auto value = parse_value();
                if (!value) {
                    return std::nullopt;
                }
                list.items.push_back(std::move(*value));
            }
            skip_space();
            if (consume(']')) {
                return list;
            }
            if (!consume(',')) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<MiValue> parse_bare() {
        size_t start = pos_;
        while (pos_ < text_.size() && text_[pos_] != ',' && text_[pos_] != ']' && text_[pos_] != '}') {
            ++pos_;
        }
        if (pos_ == start) {
            return std::nullopt;
        }
        MiValue value;
        value.kind = MiValue::Kind::Bare;
        value.value = std::string(text_.substr(start, pos_ - start));
        return value;
    }

    std::string parse_identifier() {
        skip_space();
        size_t start = pos_;
        while (pos_ < text_.size()) {
            unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (!(std::isalnum(c) || c == '_' || c == '-')) {
                break;
            }
            ++pos_;
        }
        return std::string(text_.substr(start, pos_ - start));
    }

    void skip_space() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    bool consume(char c) {
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    std::string_view text_;
    size_t pos_ = 0;
};

std::string value_summary(const MiValue &value, int depth = 0) {
    if (depth > 3) {
        return "...";
    }
    std::ostringstream out;
    if (value.kind == MiValue::Kind::String || value.kind == MiValue::Kind::Bare) {
        return value.value;
    }
    if (value.kind == MiValue::Kind::Tuple) {
        out << "{";
        for (size_t i = 0; i < value.fields.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << value.fields[i].first << "=" << value_summary(value.fields[i].second, depth + 1);
            if (i >= 5 && i + 1 < value.fields.size()) {
                out << ", ...";
                break;
            }
        }
        out << "}";
        return out.str();
    }
    out << "[";
    for (size_t i = 0; i < value.items.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << value_summary(value.items[i], depth + 1);
        if (i >= 5 && i + 1 < value.items.size()) {
            out << ", ...";
            break;
        }
    }
    out << "]";
    return out.str();
}

std::string limit_summary(std::string text, size_t max_size = 160) {
    if (text.size() <= max_size) {
        return text;
    }
    if (max_size <= 4) {
        return text.substr(0, max_size);
    }
    text.resize(max_size - 4);
    text += " ...";
    return text;
}

std::optional<size_t> record_marker(std::string_view line, std::string_view markers) {
    size_t marker = line.find_first_of(markers);
    if (marker == std::string_view::npos) {
        return std::nullopt;
    }
    for (size_t i = 0; i < marker; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(line[i]))) {
            return std::nullopt;
        }
    }
    return marker;
}

std::string token_prefix(std::string_view line, size_t marker) {
    return marker == 0 ? "" : std::string(line.substr(0, marker));
}

std::string result_payload(std::string_view line, size_t marker) {
    auto comma = line.find(',', marker + 1);
    if (comma == std::string_view::npos || comma + 1 >= line.size()) {
        return {};
    }
    return std::string(line.substr(comma + 1));
}

const MiValue *tuple_field(const MiValue &value, std::string_view name) {
    if (value.kind != MiValue::Kind::Tuple) {
        return nullptr;
    }
    for (const auto &field : value.fields) {
        if (field.first == name) {
            return &field.second;
        }
    }
    return nullptr;
}

std::string scalar_summary(const MiValue &value) {
    if (value.kind == MiValue::Kind::String || value.kind == MiValue::Kind::Bare) {
        return value.value;
    }
    return value_summary(value);
}

void append_named_field(std::ostringstream &out,
                        const MiValue &tuple,
                        std::string_view name,
                        std::string_view label = {}) {
    const MiValue *field = tuple_field(tuple, name);
    if (!field) {
        return;
    }
    out << " " << (label.empty() ? name : label) << "="
        << limit_summary(scalar_summary(*field));
}

std::string frame_summary(const MiValue &frame) {
    const MiValue *func = tuple_field(frame, "func");
    const MiValue *file = tuple_field(frame, "file");
    const MiValue *fullname = tuple_field(frame, "fullname");
    const MiValue *line = tuple_field(frame, "line");
    std::ostringstream out;
    if (func) {
        out << " frame=" << limit_summary(scalar_summary(*func), 80);
    }
    const MiValue *path = file ? file : fullname;
    if (path) {
        out << " at " << limit_summary(scalar_summary(*path), 120);
        if (line) {
            out << ":" << scalar_summary(*line);
        }
    }
    if (!func && !path) {
        out << " frame=" << limit_summary(value_summary(frame));
    }
    return out.str();
}

void append_payload_summary(std::ostringstream &out, const MiValue &payload) {
    append_named_field(out, payload, "msg");
    append_named_field(out, payload, "reason");
    append_named_field(out, payload, "thread-id");
    append_named_field(out, payload, "stopped-threads");
    append_named_field(out, payload, "value");
    if (const MiValue *frame = tuple_field(payload, "frame")) {
        out << frame_summary(*frame);
    }
    if (const MiValue *bkpt = tuple_field(payload, "bkpt")) {
        out << " bkpt=" << limit_summary(value_summary(*bkpt));
    }
    if (const MiValue *wpt = tuple_field(payload, "wpt")) {
        out << " wpt=" << limit_summary(value_summary(*wpt));
    }
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

std::optional<MiValue> parse_mi_value(std::string_view text) {
    return MiValueParser(text).parse();
}

std::vector<MiRecordAudit> audit_mi_records(const std::vector<std::string> &lines,
                                            const std::vector<unsigned long long> &sequences) {
    std::vector<MiRecordAudit> records;
    records.reserve(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string_view line(lines[i]);
        MiRecordAudit audit;
        audit.sequence = i < sequences.size() ? sequences[i] : 0;
        if (line == "(gdb)") {
            audit.record_kind = "prompt";
            records.push_back(std::move(audit));
            continue;
        }
        auto marker_opt = record_marker(line, "^*=~@&");
        if (!marker_opt) {
            audit.record_kind = "unknown";
            records.push_back(std::move(audit));
            continue;
        }
        size_t marker = *marker_opt;
        audit.token = token_prefix(line, marker);
        char kind = line[marker];
        if (kind == '^') {
            audit.record_kind = "result";
        } else if (kind == '*' || kind == '=') {
            audit.record_kind = "async";
        } else if (kind == '~' || kind == '@' || kind == '&') {
            audit.record_kind = "stream";
            audit.stream_type = kind == '~' ? "console" : (kind == '@' ? "target" : "log");
        } else {
            audit.record_kind = "unknown";
        }
        if (kind == '^' || kind == '*' || kind == '=') {
            size_t class_start = marker + 1;
            size_t class_end = line.find(',', class_start);
            audit.record_class = std::string(line.substr(class_start,
                                                         class_end == std::string_view::npos
                                                             ? std::string_view::npos
                                                             : class_end - class_start));
        }
        records.push_back(std::move(audit));
    }
    return records;
}

std::string summarize_mi_records(const std::vector<std::string> &lines) {
    std::ostringstream out;
    for (const auto &raw : lines) {
        std::string_view line(raw);
        auto marker_opt = record_marker(line, "^*=");
        if (!marker_opt) {
            continue;
        }
        size_t marker = *marker_opt;
        char kind = line[marker];
        if (kind != '^' && kind != '*' && kind != '=') {
            continue;
        }
        size_t class_start = marker + 1;
        size_t class_end = line.find(',', class_start);
        std::string klass(line.substr(class_start,
                                      class_end == std::string_view::npos
                                          ? std::string_view::npos
                                          : class_end - class_start));
        out << (kind == '^' ? "result" : "async") << ":" << klass;
        std::string payload = result_payload(line, marker);
        if (!payload.empty()) {
            auto parsed = parse_mi_value("{" + payload + "}");
            if (parsed) {
                append_payload_summary(out, *parsed);
            }
        }
        out << "\n";
    }
    return out.str();
}
