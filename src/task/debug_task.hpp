#pragma once

#include <filesystem>
#include <optional>
#include <string>

struct DebugTask {
    std::string problem;
    std::filesystem::path executable;
    std::filesystem::path working_directory;
    std::string args_raw;
    std::optional<std::filesystem::path> core_dump;
};

DebugTask load_task(const std::filesystem::path &task_file);
void validate_task(const DebugTask &task);

