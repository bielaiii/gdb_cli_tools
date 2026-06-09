#include "../src/common/json.hpp"
#include "../src/task/debug_task.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace fs = std::filesystem;

static void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

static void write_file(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to write " + path.string());
    }
    out << text;
}

template <typename Fn>
static void require_throws(Fn fn, const std::string &needle, const std::string &message) {
    try {
        fn();
    } catch (const std::exception &ex) {
        std::string error = ex.what();
        require(error.find(needle) != std::string::npos,
                message + " threw unexpected error: " + error);
        return;
    }
    throw std::runtime_error(message + " did not throw");
}

int main() {
    fs::path root = fs::temp_directory_path() /
                    ("gdb-agent-task-parser-tests-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "work" / "bin");
    write_file(root / "work" / "bin" / "app", "not an executable but it exists\n");
    write_file(root / "work" / "input.txt", "stdin\n");
    write_file(root / "work" / "core.1", "core\n");

    fs::path valid = root / "valid_task.md";
    write_file(valid,
               "### problem\n\n"
               "Parser edge task.\n\n"
               "### executable\n\n"
               "bin/app\n\n"
               "### working directory\n\n"
               "work\n\n"
               "### args\n\n"
               "--name \"hello world\" --path /tmp/a\\ b '' \"quoted\\\"value\"\n\n"
               "### stdin\n\n"
               "input.txt\n\n"
               "### env\n\n"
               "A=1\n"
               "B_TWO=hello=world\n\n"
               "### run timeout\n\n"
               "1234\n\n"
               "### core dump\n\n"
               "core.1\n");

    DebugTask task = load_task(valid);
    require(task.problem == "Parser edge task.", "problem should parse");
    require(task.working_directory == fs::weakly_canonical(root / "work"), "working directory should be absolute");
    require(task.executable == fs::weakly_canonical(root / "work" / "bin" / "app"), "executable should resolve relative to working directory");
    require(task.stdin_path == fs::weakly_canonical(root / "work" / "input.txt"), "stdin should resolve relative to working directory");
    require(task.core_dump && *task.core_dump == fs::weakly_canonical(root / "work" / "core.1"), "core dump should resolve relative to working directory");
    require(task.run_timeout_ms == 1234, "run timeout should parse");
    require(task.args.size() == 6, "args should preserve shell-like token count");
    require(task.args[0] == "--name" && task.args[1] == "hello world", "quoted arg should parse");
    require(task.args[2] == "--path" && task.args[3] == "/tmp/a b", "escaped space should parse");
    require(task.args[4].empty(), "empty quoted arg should parse");
    require(task.args[5] == "quoted\"value", "escaped quote should parse");
    require(task.env.at("A") == "1", "env A should parse");
    require(task.env.at("B_TWO") == "hello=world", "env value may contain equals");
    validate_task(task);

    write_file(root / "missing_executable.md",
               "### problem\n\nx\n\n### working directory\n\nwork\n");
    require_throws([&]() { (void)load_task(root / "missing_executable.md"); },
                   "missing required task field: executable",
                   "missing executable");

    write_file(root / "unterminated_args.md",
               "### problem\n\nx\n\n### executable\n\nbin/app\n\n### working directory\n\nwork\n\n### args\n\n\"unterminated\n");
    require_throws([&]() { (void)load_task(root / "unterminated_args.md"); },
                   "unterminated quote in args",
                   "unterminated args");

    write_file(root / "bad_env.md",
               "### problem\n\nx\n\n### executable\n\nbin/app\n\n### working directory\n\nwork\n\n### env\n\nBAD-KEY=value\n");
    require_throws([&]() { (void)load_task(root / "bad_env.md"); },
                   "invalid env key",
                   "bad env key");

    write_file(root / "bad_timeout.md",
               "### problem\n\nx\n\n### executable\n\nbin/app\n\n### working directory\n\nwork\n\n### run timeout\n\n0\n");
    require_throws([&]() { (void)load_task(root / "bad_timeout.md"); },
                   "run timeout must be positive",
                   "bad timeout");

    require_throws([&]() { (void)parse_json("{"); },
                   "unexpected JSON character",
                   "invalid JSON");
    Json typed = parse_json(R"({"action":42,"params":{"expression":true}})");
    require(typed.string_or("action", "fallback") == "fallback", "string_or should reject non-string action");

    std::string padded_json = R"(xx{"action":"run","schema_version":2,"force":true}yy)";
    std::string_view json_view(padded_json.data() + 2, padded_json.size() - 4);
    Json view_json = parse_json(json_view);
    std::string padded_key = "xxactionyy";
    std::string_view action_key(padded_key.data() + 2, 6);
    std::string padded_fallback = "xxfallbackyy";
    std::string_view fallback_view(padded_fallback.data() + 2, 8);
    require(view_json.string_or(action_key) == "run", "string_view key lookup should not require a string temporary");
    require(view_json.string_or("missing", fallback_view) == "fallback", "string_view fallback should be accepted");
    require(view_json.int_or("schema_version") == 2, "string_view parser should preserve numbers");
    require(view_json.bool_or("force", false), "string_view parser should preserve booleans");

    fs::remove_all(root);
    std::cout << "task_parser_tests ok\n";
    return 0;
}
