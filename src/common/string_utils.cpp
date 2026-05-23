#include "string_utils.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

std::string trim(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](char c) { return !is_space(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [&](char c) { return !is_space(c); }).base(), s.end());
    return s;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

std::string shell_quote_for_report(const std::string &s) {
    if (s.find_first_of(" \t\n\"'\\") == std::string::npos) {
        return s;
    }
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

void replace_all(std::string &s, const std::string &from, const std::string &to) {
    if (from.empty()) {
        return;
    }
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string sanitize_output(std::string s, const std::filesystem::path &working_directory) {
    replace_all(s,
                "std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >",
                "std::string");
    replace_all(s, working_directory.string() + "/", "");
    return s;
}

std::string slugify(std::string s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!out.empty() && out.back() != '-') {
            out.push_back('-');
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out.empty() ? "evidence" : out;
}

void write_text_file(const std::filesystem::path &path, const std::string &text) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to write: " + path.string());
    }
    out << text;
}

