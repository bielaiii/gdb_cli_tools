#pragma once

#include "../evidence/evidence_store.hpp"
#include "../gdb/gdb_session.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

enum class SessionState {
    Created,
    Starting,
    Ready,
    Loading,
    Stopped,
    Running,
    Interrupting,
    Exited,
    Error,
    Finishing,
    Closed
};

struct SessionOutcome {
    std::string stop_reason;
    std::string signal_name;
    SessionState state = SessionState::Created;
    bool core_mode = false;
    bool segfault = false;
    bool run_timed_out = false;
    int run_timeout_ms = 30000;
    std::uintmax_t inferior_stdout_offset = 0;
    std::uintmax_t inferior_stderr_offset = 0;
    std::string inferior_stdout;
    std::string inferior_stderr;
    std::string agent_inference;
    std::string final_agent_conclusion;
};

Evidence collect_console(GdbSession &session,
                         const std::string &title,
                         const std::string &console_command,
                         bool backtrace_summary = false,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

void collect_light_evidence(GdbSession &session);
void collect_core_evidence(GdbSession &session);
void collect_inferior_output(GdbSession &session,
                             std::uintmax_t &stdout_offset,
                             std::uintmax_t &stderr_offset);
std::string_view session_state_name(SessionState state);
