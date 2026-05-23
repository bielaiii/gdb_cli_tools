#include "gdb_session.hpp"

#include "../common/string_utils.hpp"
#include "mi_utils.hpp"

#include <algorithm>
#include <stdexcept>
#include <sys/wait.h>

namespace fs = std::filesystem;

GdbSession::GdbSession(fs::path assets, fs::path working_directory)
    : assets_(std::move(assets)),
      working_directory_(std::move(working_directory)),
      evidence_(assets_, working_directory_) {
    fs::create_directories(assets_ / "logs");
    session_log_.open(assets_ / "logs" / "session.mi.raw.log");
    if (!session_log_) {
        throw std::runtime_error("failed to open session log");
    }
}

void GdbSession::start() {
    process_ = GdbProcess::spawn();
    drain_startup();
}

Task<CommandResult> GdbSession::command_async(const std::string &command,
                                              std::chrono::milliseconds timeout) {
    co_return command_blocking(command, timeout);
}

Task<CommandResult> GdbSession::exec_control_async(const std::string &command,
                                                   std::chrono::milliseconds run_deadline) {
    co_return exec_control_blocking(command, run_deadline);
}

CommandResult GdbSession::command(const std::string &command, std::chrono::milliseconds timeout) {
    return command_async(command, timeout).get();
}

CommandResult GdbSession::exec_control(const std::string &command, std::chrono::milliseconds run_deadline) {
    return exec_control_async(command, run_deadline).get();
}

void GdbSession::initialize(const DebugTask &task) {
    command("-gdb-set pagination off");
    command("-gdb-set confirm off");
    command("-gdb-set print pretty on");
    command("-gdb-set print object on");
    command("-gdb-set print elements 200");
    command("-gdb-set print max-depth 4");
    command("-gdb-set print repeats 10");
    command("-environment-cd " + mi_quote(task.working_directory.string()));
    command("-file-exec-and-symbols " + mi_quote(task.executable.string()), std::chrono::milliseconds(10000));
    if (!task.args_raw.empty()) {
        command("-exec-arguments " + task.args_raw);
    }
}

CommandResult GdbSession::load_core(const DebugTask &task) {
    if (!task.core_dump) {
        throw std::runtime_error("core dump path is missing");
    }
    return command("-target-select core " + mi_quote(task.core_dump->string()), std::chrono::milliseconds(10000));
}

EvidenceStore &GdbSession::evidence_store() {
    return evidence_;
}

const EvidenceStore &GdbSession::evidence_store() const {
    return evidence_;
}

const fs::path &GdbSession::assets_dir() const {
    return assets_;
}

void GdbSession::shutdown() {
    if (process_.pid > 0) {
        try {
            command("-gdb-exit", std::chrono::milliseconds(1000));
        } catch (...) {
        }
        int status = 0;
        waitpid(process_.pid, &status, WNOHANG);
        process_.terminate();
    }
}

void GdbSession::drain_startup() {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < deadline) {
        auto line = process_.read_line(std::chrono::milliseconds(100));
        if (!line) {
            continue;
        }
        log_line(*line);
        if (*line == "(gdb)") {
            break;
        }
    }
}

CommandResult GdbSession::command_blocking(const std::string &command, std::chrono::milliseconds timeout) {
    uint64_t token = ++token_;
    std::string prefix = std::to_string(token);
    process_.write_all(prefix + command + "\n");

    CommandResult result;
    result.command = command;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        auto line = process_.read_line(std::max(std::chrono::milliseconds(1), remaining));
        if (!line) {
            break;
        }
        log_line(*line);
        if (*line == "(gdb)") {
            continue;
        }
        result.raw_lines.push_back(*line);
        if (starts_with(*line, prefix + "^")) {
            result.result_class = line->substr(prefix.size() + 1);
            auto comma = result.result_class.find(',');
            if (comma != std::string::npos) {
                result.result_class.resize(comma);
            }
            return result;
        }
    }
    result.timed_out = true;
    return result;
}

CommandResult GdbSession::exec_control_blocking(const std::string &command, std::chrono::milliseconds run_deadline) {
    uint64_t token = ++token_;
    std::string prefix = std::to_string(token);
    process_.write_all(prefix + command + "\n");

    CommandResult result;
    result.command = command;
    auto deadline = std::chrono::steady_clock::now() + run_deadline;
    bool saw_running = false;

    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        auto line = process_.read_line(std::max(std::chrono::milliseconds(1), remaining));
        if (!line) {
            break;
        }
        log_line(*line);
        if (*line == "(gdb)") {
            continue;
        }
        result.raw_lines.push_back(*line);

        if (starts_with(*line, prefix + "^running")) {
            saw_running = true;
        } else if (line->find("*stopped") != std::string::npos) {
            result.stop_reason = field_value(*line, "reason");
            result.signal_name = field_value(*line, "signal-name");
            result.breakpoint_number = field_value(*line, "bkptno");
            return result;
        } else if (line->find("=thread-group-exited") != std::string::npos ||
                   line->find("reason=\"exited") != std::string::npos) {
            result.exited = true;
            result.stop_reason = "exited";
            return result;
        } else if (!saw_running && starts_with(*line, prefix + "^error")) {
            result.result_class = "error";
            return result;
        }
    }

    result.timed_out = true;
    result.stop_reason = "run_deadline_reached";
    interrupt_after_deadline(result);
    return result;
}

void GdbSession::interrupt_after_deadline(CommandResult &result) {
    uint64_t token = ++token_;
    std::string prefix = std::to_string(token);
    process_.write_all(prefix + "-exec-interrupt\n");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        auto line = process_.read_line(std::max(std::chrono::milliseconds(1), remaining));
        if (!line) {
            break;
        }
        log_line(*line);
        if (*line == "(gdb)") {
            continue;
        }
        result.raw_lines.push_back(*line);
        if (line->find("*stopped") != std::string::npos) {
            result.stop_reason = "interrupted_by_tool_deadline";
            result.signal_name = field_value(*line, "signal-name");
            return;
        }
    }
}

void GdbSession::log_line(const std::string &line) {
    session_log_ << line << '\n';
    session_log_.flush();
}
