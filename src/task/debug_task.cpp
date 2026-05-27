#include "debug_task.hpp"

#include "../common/string_utils.hpp"

#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;

static std::map<std::string, std::string> parse_markdown_fields(const fs::path &path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open task file: " + path.string());
    }

    std::map<std::string, std::string> fields;
    std::string current_key;
    std::ostringstream current_value;
    std::string line;

    auto flush = [&]() {
        if (!current_key.empty()) {
            fields[current_key] = trim(current_value.str());
            current_value.str("");
            current_value.clear();
        }
    };

    while (std::getline(in, line)) {
        if (starts_with(line, "### ")) {
            flush();
            current_key = lower(trim(line.substr(4)));
            continue;
        }
        if (!current_key.empty()) {
            current_value << line << '\n';
        }
    }
    flush();
    return fields;
}

static fs::path absolute_from(const fs::path &base, const std::string &value) {
    fs::path p(trim(value));
    if (p.empty()) {
        return p;
    }
    if (p.is_absolute()) {
        return fs::weakly_canonical(p);
    }
    return fs::weakly_canonical(base / p);
}

static std::vector<std::string> parse_shell_like_args(std::string_view text) {
    std::vector<std::string> args;
    std::string current;
    bool in_single = false;
    bool in_double = false;
    bool escaping = false;
    bool have_token = false;

    auto finish_token = [&]() {
        if (have_token) {
            args.push_back(current);
            current.clear();
            have_token = false;
        }
    };

    for (char c : text) {
        if (escaping) {
            current.push_back(c);
            have_token = true;
            escaping = false;
            continue;
        }

        if (c == '\\' && !in_single) {
            escaping = true;
            have_token = true;
            continue;
        }

        if (c == '\'' && !in_double) {
            in_single = !in_single;
            have_token = true;
            continue;
        }

        if (c == '"' && !in_single) {
            in_double = !in_double;
            have_token = true;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c)) && !in_single && !in_double) {
            finish_token();
            continue;
        }

        current.push_back(c);
        have_token = true;
    }

    if (escaping) {
        current.push_back('\\');
    }
    if (in_single || in_double) {
        throw std::runtime_error("unterminated quote in args");
    }
    finish_token();
    return args;
}

static std::map<std::string, std::string> parse_env_lines(const std::string &text) {
    std::map<std::string, std::string> env;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        auto pos = line.find('=');
        if (pos == std::string::npos || pos == 0) {
            throw std::runtime_error("invalid env entry, expected KEY=value: " + line);
        }
        std::string key = line.substr(0, pos);
        for (char c : key) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
                throw std::runtime_error("invalid env key: " + key);
            }
        }
        env[std::move(key)] = line.substr(pos + 1);
    }
    return env;
}

static int parse_positive_int(const std::string &value, const std::string &field) {
    try {
        int parsed = std::stoi(trim(value));
        if (parsed <= 0) {
            throw std::runtime_error(field + " must be positive");
        }
        return parsed;
    } catch (const std::invalid_argument &) {
        throw std::runtime_error(field + " must be an integer");
    } catch (const std::out_of_range &) {
        throw std::runtime_error(field + " is out of range");
    }
}

DebugTask load_task(const fs::path &task_file) {
    auto fields = parse_markdown_fields(task_file);
    for (const auto *key : {"problem", "executable", "working directory"}) {
        if (!fields.contains(key) || trim(fields[key]).empty()) {
            throw std::runtime_error(std::string("missing required task field: ") + key);
        }
    }

    fs::path task_base = fs::absolute(task_file).parent_path();
    fs::path working = absolute_from(task_base, fields["working directory"]);
    fs::path executable = absolute_from(working, fields["executable"]);

    DebugTask task;
    task.problem = fields["problem"];
    task.executable = executable;
    task.working_directory = working;
    task.args_raw = fields.contains("args") ? trim(fields["args"]) : "";
    task.args = parse_shell_like_args(task.args_raw);
    if (fields.contains("stdin") && !trim(fields["stdin"]).empty()) {
        task.stdin_path = absolute_from(working, fields["stdin"]);
    }
    if (fields.contains("env") && !trim(fields["env"]).empty()) {
        task.env = parse_env_lines(fields["env"]);
    }
    if (fields.contains("run timeout") && !trim(fields["run timeout"]).empty()) {
        task.run_timeout_ms = parse_positive_int(fields["run timeout"], "run timeout");
    }
    if (fields.contains("core dump") && !trim(fields["core dump"]).empty()) {
        task.core_dump = absolute_from(working, fields["core dump"]);
    }
    return task;
}

void validate_task(const DebugTask &task) {
    if (!fs::exists(task.working_directory) || !fs::is_directory(task.working_directory)) {
        throw std::runtime_error("working directory does not exist: " + task.working_directory.string());
    }
    if (!fs::exists(task.executable)) {
        throw std::runtime_error("executable does not exist: " + task.executable.string());
    }
    if (task.core_dump && !fs::exists(*task.core_dump)) {
        throw std::runtime_error("core dump does not exist: " + task.core_dump->string());
    }
    if (!fs::exists(task.stdin_path)) {
        throw std::runtime_error("stdin file does not exist: " + task.stdin_path.string());
    }
}
