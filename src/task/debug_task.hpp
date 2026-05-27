#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct DebugTask {
    std::string problem;
    std::filesystem::path executable;
    std::filesystem::path working_directory;
    std::string args_raw;
    std::vector<std::string> args;
    std::filesystem::path stdin_path = "/dev/null";
    std::map<std::string, std::string> env;
    int run_timeout_ms = 30000;
    std::optional<std::filesystem::path> core_dump;
};

DebugTask load_task(const std::filesystem::path &task_file);
void validate_task(const DebugTask &task);
