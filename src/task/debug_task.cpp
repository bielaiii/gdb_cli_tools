#include "debug_task.hpp"

#include "../common/string_utils.hpp"

#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

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
}

