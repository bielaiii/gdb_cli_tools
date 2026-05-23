#pragma once

#include <filesystem>
#include <string>
#include <string_view>

std::string trim(std::string s);
std::string lower(std::string s);
bool starts_with(std::string_view s, std::string_view prefix);
std::string shell_quote_for_report(const std::string &s);
void replace_all(std::string &s, const std::string &from, const std::string &to);
std::string sanitize_output(std::string s, const std::filesystem::path &working_directory);
std::string slugify(std::string s);
void write_text_file(const std::filesystem::path &path, const std::string &text);

