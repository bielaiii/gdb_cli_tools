#include "string_utils.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

std::string regex_escape(std::string s) {
    static const std::string special = R"(\.^$|()[]{}*+?)";
    std::string out;
    out.reserve(s.size() * 2);
    for (char c : s) {
        if (special.find(c) != std::string::npos) {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

std::vector<std::string> split_template_args(std::string_view text) {
    std::vector<std::string> args;
    size_t start = 0;
    int depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '<') {
            ++depth;
        } else if (c == '>') {
            --depth;
        } else if (c == ',' && depth == 0) {
            args.push_back(trim(std::string(text.substr(start, i - start))));
            start = i + 1;
        }
    }
    args.push_back(trim(std::string(text.substr(start))));
    return args;
}

std::string join_template_args(const std::vector<std::string> &args, size_t count) {
    std::ostringstream out;
    for (size_t i = 0; i < count && i < args.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << args[i];
    }
    return out.str();
}

std::string strip_const_key(std::string arg) {
    arg = trim(std::move(arg));
    constexpr std::string_view prefix = "const ";
    constexpr std::string_view suffix = " const";
    if (starts_with(arg, prefix)) {
        arg = trim(arg.substr(prefix.size()));
    }
    if (arg.size() > suffix.size() && arg.compare(arg.size() - suffix.size(), suffix.size(), suffix) == 0) {
        arg = trim(arg.substr(0, arg.size() - suffix.size()));
    }
    return arg;
}

size_t matching_template_close(std::string_view s, size_t open) {
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i) {
        if (s[i] == '<') {
            ++depth;
        } else if (s[i] == '>') {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }
    return std::string_view::npos;
}

std::string simplify_std_templates(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, 5, "std::") != 0) {
            out.push_back(s[i++]);
            continue;
        }

        size_t name_start = i + 5;
        size_t name_end = name_start;
        while (name_end < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[name_end]);
            if (!(std::isalnum(c) || c == '_' || s[name_end] == ':')) {
                break;
            }
            ++name_end;
        }
        if (name_end >= s.size() || s[name_end] != '<') {
            out.append(s, i, name_end - i);
            i = name_end;
            continue;
        }
        size_t close = matching_template_close(s, name_end);
        if (close == std::string::npos) {
            out.append(s, i, name_end - i);
            i = name_end;
            continue;
        }

        std::string name = s.substr(name_start, name_end - name_start);
        auto args = split_template_args(std::string_view(s).substr(name_end + 1, close - name_end - 1));
        for (auto &arg : args) {
            arg = simplify_std_templates(std::move(arg));
        }

        if (name == "basic_string" || name == "__cxx11::basic_string") {
            out += "std::string";
        } else if ((name == "vector" || name == "list" || name == "deque" || name == "set") && !args.empty()) {
            out += "std::" + name + "<" + args[0] + ">";
        } else if ((name == "map" || name == "unordered_map") && args.size() >= 2) {
            out += "std::" + name + "<" + args[0] + ", " + args[1] + ">";
        } else if (name == "unique_ptr" && !args.empty()) {
            out += "std::unique_ptr<" + args[0] + ">";
        } else if ((name == "shared_ptr" || name == "weak_ptr" || name == "optional") && !args.empty()) {
            out += "std::" + name + "<" + args[0] + ">";
        } else if (name == "pair" && args.size() >= 2) {
            out += "std::pair<" + strip_const_key(args[0]) + ", " + args[1] + ">";
        } else if ((name == "tuple" || name == "variant") && !args.empty()) {
            out += "std::" + name + "<" + join_template_args(args, args.size()) + ">";
        } else {
            out += "std::" + name + "<" + join_template_args(args, args.size()) + ">";
        }
        i = close + 1;
    }
    return out;
}

std::string relativize_working_directory_paths(std::string s, const std::filesystem::path &working_directory) {
    if (working_directory.empty()) {
        return s;
    }
    std::filesystem::path root = working_directory.lexically_normal();
    std::string root_string = root.string();
    if (root_string.empty()) {
        return s;
    }
    std::regex path_regex(regex_escape(root_string) + R"(/[^ \t\n\r"')]+)");
    std::string out;
    std::sregex_iterator it(s.begin(), s.end(), path_regex);
    std::sregex_iterator end;
    size_t last = 0;
    for (; it != end; ++it) {
        const auto &match = *it;
        out.append(s, last, static_cast<size_t>(match.position()) - last);
        std::string token = match.str();
        std::string suffix;
        while (!token.empty() && (token.back() == '.' || token.back() == ',' || token.back() == ';')) {
            suffix.insert(suffix.begin(), token.back());
            token.pop_back();
        }
        std::filesystem::path normalized = std::filesystem::path(token).lexically_normal();
        std::filesystem::path relative = normalized.lexically_relative(root);
        if (!relative.empty() && relative.native().find("..") != 0) {
            out += relative.string();
        } else {
            out += match.str();
        }
        out += suffix;
        last = static_cast<size_t>(match.position() + match.length());
    }
    out.append(s, last, std::string::npos);
    return out;
}

} // namespace

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
    s = simplify_std_templates(std::move(s));
    replace_all(s,
                "std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >",
                "std::string");
    replace_all(s,
                "std::basic_string<char, std::char_traits<char>, std::allocator<char> >",
                "std::string");
    replace_all(s,
                "std::__cxx11::basic_string<char,std::char_traits<char>,std::allocator<char> >",
                "std::string");
    replace_all(s,
                "std::basic_string<char,std::char_traits<char>,std::allocator<char> >",
                "std::string");
    s = std::regex_replace(s,
                           std::regex(R"(std::(__cxx11::)?basic_string<char,\s*std::char_traits<char>,\s*std::allocator<char>\s*>)"),
                           "std::string");
    s = std::regex_replace(s, std::regex(R"(\s*>\s*>)"), ">>");
    s = std::regex_replace(s, std::regex(R"(,\s*std::allocator<([^<>]+)>)"), "");
    s = std::regex_replace(s, std::regex(R"(,\s*std::default_delete<([^<>]+)>)"), "");
    s = std::regex_replace(s,
                           std::regex(R"(std::map<\s*([^,<>]+)\s*,\s*([^,<>]+)\s*,\s*std::less<[^<>]+>\s*,\s*std::allocator<std::pair<[^<>]+>>\s*>)"),
                           "std::map<$1, $2>");
    s = std::regex_replace(s,
                           std::regex(R"(std::unordered_map<\s*([^,<>]+)\s*,\s*([^,<>]+)\s*,\s*std::hash<[^<>]+>\s*,\s*std::equal_to<[^<>]+>\s*,\s*std::allocator<std::pair<[^<>]+>>\s*>)"),
                           "std::unordered_map<$1, $2>");
    s = std::regex_replace(s,
                           std::regex(R"(std::pair<\s*const\s+([^,<>]+)\s*,\s*([^<>]+)\s*>)"),
                           "std::pair<$1, $2>");
    s = std::regex_replace(s,
                           std::regex(R"(std::pair<\s*([^,<>]+)\s+const\s*,\s*([^<>]+)\s*>)"),
                           "std::pair<$1, $2>");
    s = simplify_std_templates(std::move(s));
    s = std::regex_replace(s, std::regex(R"(<\s+)"), "<");
    s = std::regex_replace(s, std::regex(R"(\s+>)"), ">");
    s = std::regex_replace(s, std::regex(R"(,\s+)"), ", ");
    s = relativize_working_directory_paths(std::move(s), working_directory);
    replace_all(s, working_directory.lexically_normal().string() + "/", "");
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
