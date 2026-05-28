#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct MiValue {
    enum class Kind {
        String,
        Tuple,
        List,
        Bare
    };

    Kind kind = Kind::Bare;
    std::string value;
    std::vector<std::pair<std::string, MiValue>> fields;
    std::vector<MiValue> items;
};

struct MiRecordAudit {
    unsigned long long sequence = 0;
    std::string token;
    std::string record_kind;
    std::string record_class;
    std::string stream_type;
};

std::string mi_quote(std::string_view s);
std::string unescape_mi_string(std::string_view s);
std::optional<std::string> mi_stream_payload(std::string_view line);
std::string field_value(std::string_view line, std::string_view name);
std::string joined_raw(const std::vector<std::string> &lines);
std::string decoded_streams(const std::vector<std::string> &lines);
std::optional<MiValue> parse_mi_value(std::string_view text);
std::vector<MiRecordAudit> audit_mi_records(const std::vector<std::string> &lines,
                                            const std::vector<unsigned long long> &sequences);
std::string summarize_mi_records(const std::vector<std::string> &lines);
