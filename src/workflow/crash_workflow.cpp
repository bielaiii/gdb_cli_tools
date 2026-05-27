#include "crash_workflow.hpp"

#include "../gdb/mi_utils.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

static std::string read_text_delta(const fs::path &path, std::uintmax_t &offset) {
    if (!fs::exists(path) || !fs::is_regular_file(path)) {
        return {};
    }
    std::uintmax_t size = fs::file_size(path);
    if (size < offset) {
        offset = 0;
    }
    if (size <= offset) {
        return {};
    }
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    in.seekg(static_cast<std::streamoff>(offset));
    std::ostringstream out;
    out << in.rdbuf();
    offset = size;
    return out.str();
}

Evidence collect_console(GdbSession &session,
                         const std::string &title,
                         const std::string &console_command,
                         bool backtrace_summary,
                         std::chrono::milliseconds timeout) {
    auto result = session.command("-interpreter-exec console " + mi_quote(console_command), timeout);
    return session.evidence_store().add("GdbCommand",
                                        title,
                                        console_command,
                                        result.raw_lines,
                                        backtrace_summary,
                                        result.record_sequences);
}

void collect_light_evidence(GdbSession &session) {
    collect_console(session, "Backtrace", "bt", true);
    collect_console(session, "Frame arguments", "info args");
    collect_console(session, "Local variables", "info locals");
    collect_console(session, "Registers", "info registers");
}

void collect_core_evidence(GdbSession &session) {
    collect_console(session, "Core info files", "info files");
    collect_console(session, "Core shared libraries", "info sharedlibrary", false, std::chrono::milliseconds(10000));
    collect_console(session, "Core threads", "info threads");
    collect_console(session, "Core all thread backtraces", "thread apply all bt", true, std::chrono::milliseconds(15000));
    collect_light_evidence(session);
}

void collect_inferior_output(GdbSession &session,
                             std::uintmax_t &stdout_offset,
                             std::uintmax_t &stderr_offset) {
    fs::path dir = session.assets_dir() / "inferior";
    fs::path stdout_log = dir / "stdout.log";
    fs::path stderr_log = dir / "stderr.log";
    std::string stdout_delta = read_text_delta(stdout_log, stdout_offset);
    if (!stdout_delta.empty()) {
        session.evidence_store().add_text("InferiorOutput",
                                          "Inferior stdout",
                                          stdout_log.lexically_normal().string(),
                                          stdout_delta);
    }
    std::string stderr_delta = read_text_delta(stderr_log, stderr_offset);
    if (!stderr_delta.empty()) {
        session.evidence_store().add_text("InferiorOutput",
                                          "Inferior stderr",
                                          stderr_log.lexically_normal().string(),
                                          stderr_delta);
    }
}

std::string_view session_state_name(SessionState state) {
    switch (state) {
        case SessionState::Created: return "created";
        case SessionState::Starting: return "starting";
        case SessionState::Ready: return "ready";
        case SessionState::Loading: return "loading";
        case SessionState::Stopped: return "stopped";
        case SessionState::Running: return "running";
        case SessionState::Interrupting: return "interrupting";
        case SessionState::Exited: return "exited";
        case SessionState::Error: return "error";
        case SessionState::Finishing: return "finishing";
        case SessionState::Closed: return "closed";
    }
    return "unknown";
}
