#pragma once

#include "../async/task.hpp"
#include "../evidence/evidence_store.hpp"
#include "../task/debug_task.hpp"
#include "command_result.hpp"
#include "gdb_process.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

class GdbSession {
public:
    GdbSession(std::filesystem::path assets, std::filesystem::path working_directory);

    void start();
    Task<CommandResult> command_async(const std::string &command,
                                      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
    Task<CommandResult> exec_control_async(const std::string &command,
                                           std::chrono::milliseconds run_deadline);

    CommandResult command(const std::string &command,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
    CommandResult exec_control(const std::string &command,
                               std::chrono::milliseconds run_deadline);

    void initialize(const DebugTask &task);
    CommandResult load_core(const DebugTask &task);
    EvidenceStore &evidence_store();
    const std::filesystem::path &assets_dir() const;
    void shutdown();

private:
    void drain_startup();
    CommandResult command_blocking(const std::string &command, std::chrono::milliseconds timeout);
    CommandResult exec_control_blocking(const std::string &command, std::chrono::milliseconds run_deadline);
    void interrupt_after_deadline(CommandResult &result);
    void log_line(const std::string &line);

    std::filesystem::path assets_;
    std::filesystem::path working_directory_;
    EvidenceStore evidence_;
    GdbProcess process_;
    uint64_t token_ = 0;
    std::ofstream session_log_;
};
