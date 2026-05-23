#include "string_utils.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

std::string_view trim_view(std::string_view s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::string trim(std::string s) {
    auto view = trim_view(s);
    if (view.size() != s.size()) {
        s = std::string(view);
    }
    return s;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string shell_quote_for_report(std::string_view s) {
    if (s.find_first_of(" \t\n\"'\\") == std::string::npos) {
        return std::string(s);
    }
    std::string out = "'";
    out.reserve(s.size() + 2);
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

void replace_all(std::string &s, std::string_view from, std::string_view to) {
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

std::string slugify(std::string_view s) {
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

void write_text_file(const std::filesystem::path &path, std::string_view text) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to write: " + path.string());
    }
    out << text;
}
