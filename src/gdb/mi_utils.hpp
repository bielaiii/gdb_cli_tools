#pragma once

#include <optional>
#include <string>
#include <vector>

std::string mi_quote(const std::string &s);
std::string unescape_mi_string(std::string_view s);
std::optional<std::string> mi_stream_payload(const std::string &line);
std::string field_value(const std::string &line, const std::string &name);
std::string joined_raw(const std::vector<std::string> &lines);
std::string decoded_streams(const std::vector<std::string> &lines);

