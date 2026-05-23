#pragma once

#include "../evidence/evidence_store.hpp"
#include "../gdb/gdb_session.hpp"

#include <chrono>
#include <string>

struct SessionOutcome {
    std::string stop_reason;
    std::string signal_name;
    bool core_mode = false;
    bool segfault = false;
    bool run_timed_out = false;
};

Evidence collect_console(GdbSession &session,
                         const std::string &title,
                         const std::string &console_command,
                         bool backtrace_summary = false,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

void collect_light_evidence(GdbSession &session);

