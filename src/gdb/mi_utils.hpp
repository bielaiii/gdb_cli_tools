#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

std::string mi_quote(std::string_view s);
std::string unescape_mi_string(std::string_view s);
std::optional<std::string> mi_stream_payload(std::string_view line);
std::string field_value(std::string_view line, std::string_view name);
std::string joined_raw(const std::vector<std::string> &lines);
std::string decoded_streams(const std::vector<std::string> &lines);
