#include "crash_workflow.hpp"

#include "../gdb/mi_utils.hpp"

Evidence collect_console(GdbSession &session,
                         const std::string &title,
                         const std::string &console_command,
                         bool backtrace_summary,
                         std::chrono::milliseconds timeout) {
    auto result = session.command("-interpreter-exec console " + mi_quote(console_command), timeout);
    return session.evidence_store().add("GdbCommand", title, console_command, result.raw_lines, backtrace_summary);
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
