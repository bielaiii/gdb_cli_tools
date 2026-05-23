#pragma once

#include <filesystem>
#include <string>
#include <string_view>

std::string_view trim_view(std::string_view s);
std::string trim(std::string s);
std::string lower(std::string s);
bool starts_with(std::string_view s, std::string_view prefix);
std::string shell_quote_for_report(std::string_view s);
void replace_all(std::string &s, std::string_view from, std::string_view to);
std::string sanitize_output(std::string s, const std::filesystem::path &working_directory);
std::string slugify(std::string_view s);
void write_text_file(const std::filesystem::path &path, std::string_view text);
