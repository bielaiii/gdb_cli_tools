#include "action_dispatch.hpp"

#include "session_executor.hpp"
#include "../common/json.hpp"
#include "../common/string_utils.hpp"
#include "../gdb/gdb_session.hpp"
#include "../gdb/mi_utils.hpp"
#include "../replay/record_store.hpp"
#include "../replay/replay_plan.hpp"
#include "../replay/replay_runtime.hpp"
#include "../task/debug_task.hpp"
#include "../workflow/crash_workflow.hpp"
#include "../workflow/hypothesis.hpp"
#include "../workflow/hypothesis_store.hpp"
#include "../workflow/probe_runtime.hpp"

#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>


namespace fs = std::filesystem;

namespace {

std::string json_escape(const std::string &s) {
    Json json;
    json.type = Json::Type::String;
    json.string_value = s;
    return dump_json(json);
}

} // namespace

void collect_stop_followup(GdbSession &session,
                                  SessionOutcome &outcome,
                                  const CommandResult &result) {
    flush_inferior_output(session, outcome);

    if (result.signal_name == "SIGSEGV" ||
        result.timed_out ||
        result.stop_reason == "interrupted_by_tool_deadline") {
        collect_light_evidence(session);
        return;
    }

    if (result.stop_reason == "breakpoint-hit" || result.stop_reason == "watchpoint-trigger") {
        collect_console(session, "Current frame", "frame");
        collect_console(session, "Frame arguments", "info args");
        collect_console(session, "Local variables", "info locals");
    }
}

static void add_command_evidence(GdbSession &session,
                                 const std::string &kind,
                                 const std::string &title,
                                 const CommandResult &result) {
    session.evidence_store().add(kind, title, result.command, result.raw_lines, false, result.record_sequences);
}

static Evidence add_tool_error(GdbSession &session,
                               const std::string &title,
                               const std::string &action_name,
                               const std::string &message) {
    std::ostringstream text;
    text << "{\n";
    text << "  \"action\": " << json_escape(action_name) << ",\n";
    text << "  \"error\": " << json_escape(message) << "\n";
    text << "}\n";
    return session.evidence_store().add_text("ToolError", title, action_name, text.str());
}

static Evidence add_action_tool_error(GdbSession &session,
                                      const std::string &title,
                                      const std::string &action_name,
                                      const std::string &message,
                                      const std::map<std::string, std::string> &details = {}) {
    std::ostringstream text;
    text << "{\n";
    text << "  \"action\": " << json_escape(action_name) << ",\n";
    text << "  \"error\": " << json_escape(message);
    for (const auto &[key, value] : details) {
        text << ",\n";
        text << "  " << json_escape(key) << ": " << json_escape(value);
    }
    text << "\n}\n";
    return session.evidence_store().add_text("ToolError", title, action_name, text.str());
}

static std::string command_error_message(const CommandResult &result, const std::string &fallback) {
    for (const auto &raw : result.raw_lines) {
        std::string msg = field_value(raw, "msg");
        if (!msg.empty()) {
            return msg;
        }
    }
    if (result.timed_out) {
        return "GDB command timed out";
    }
    return fallback;
}

static std::string breakpoint_number_from(const CommandResult &result) {
    for (const auto &raw : result.raw_lines) {
        std::string number = field_value(raw, "number");
        if (!number.empty()) {
            return number;
        }
        const std::string catchpoint_prefix = "Catchpoint ";
        auto catchpoint_pos = raw.find(catchpoint_prefix);
        if (catchpoint_pos != std::string::npos) {
            catchpoint_pos += catchpoint_prefix.size();
            std::string catchpoint_number;
            while (catchpoint_pos < raw.size() &&
                   std::isdigit(static_cast<unsigned char>(raw[catchpoint_pos]))) {
                catchpoint_number.push_back(raw[catchpoint_pos]);
                ++catchpoint_pos;
            }
            if (!catchpoint_number.empty()) {
                return catchpoint_number;
            }
        }
    }
    return {};
}



CommandResult run_inferior(GdbSession &session,
                                  const DebugTask &task,
                                  std::chrono::milliseconds deadline) {
    fs::path inferior_dir = session.assets_dir() / "inferior";
    fs::create_directories(inferior_dir);
    fs::path stdout_log = inferior_dir / "stdout.log";
    fs::path stderr_log = inferior_dir / "stderr.log";
    write_text_file(stdout_log, "");
    write_text_file(stderr_log, "");

    std::string run_command = "run";
    for (const auto &arg : task.args) {
        run_command.push_back(' ');
        run_command += shell_quote_for_report(arg);
    }
    run_command += " < ";
    run_command += shell_quote_for_report(task.stdin_path.string());
    run_command += " > ";
    run_command += shell_quote_for_report(stdout_log.string());
    run_command += " 2> ";
    run_command += shell_quote_for_report(stderr_log.string());
    std::string command = "-interpreter-exec console " + mi_quote(run_command);
    return session.exec_control(command, deadline);
}

void update_outcome_from_stop(SessionOutcome &outcome, const CommandResult &result) {
    outcome.stop_reason = result.stop_reason.empty() ? outcome.stop_reason : result.stop_reason;
    outcome.signal_name = result.signal_name.empty() ? outcome.signal_name : result.signal_name;
    outcome.segfault = outcome.segfault || result.signal_name == "SIGSEGV";
    outcome.run_timed_out = outcome.run_timed_out || result.timed_out;
    if (result.result_class == "error") {
        outcome.state = SessionState::Error;
    } else if (result.exited || starts_with(result.stop_reason, "exited")) {
        outcome.state = SessionState::Exited;
    } else {
        outcome.state = SessionState::Stopped;
    }
}

void flush_inferior_output(GdbSession &session, SessionOutcome &outcome) {
    collect_inferior_output(session, outcome.inferior_stdout_offset, outcome.inferior_stderr_offset);
}

static bool state_is_live(SessionState state) {
    return state != SessionState::Closed && state != SessionState::Finishing;
}

static bool state_is_stopped_or_core(const SessionOutcome &outcome) {
    return outcome.core_mode || outcome.state == SessionState::Stopped;
}

static bool action_allowed_in_state(const SessionOutcome &outcome,
                                    const std::string &action,
                                    std::string &reason) {
    SessionState state = outcome.state;
    auto deny = [&](std::string why) {
        reason = std::move(why);
        return false;
    };

    if (outcome.core_mode &&
        (action == "run" || action == "continue" || action == "breakpoint_set" ||
         action == "watchpoint_set" || action == "catchpoint_set" ||
         action == "probe_delete" || action == "probe_enable" || action == "probe_disable")) {
        return deny(action + " is not available in core mode");
    }

    if (action == "finish_session" || action == "finish") {
        if (state == SessionState::Stopped || state == SessionState::Exited || state == SessionState::Error) {
            return true;
        }
        return deny("finish_session requires stopped, exited, or error state");
    }

    if (!state_is_live(state)) {
        return deny("session is not live");
    }

    if (action == "hypothesis_create" || action == "hypothesis_conclude" ||
        action == "save_action" || action == "raw_mi" ||
        action == "record_start" || action == "record_status" ||
        action == "record_stop" || action == "record_discard") {
        return true;
    }

    if (action == "backtrace" || action == "locals" || action == "args_info" ||
        action == "registers" || action == "frame_select" || action == "evaluate" ||
        action == "hypothesis_check") {
        return state_is_stopped_or_core(outcome)
                   ? true
                   : deny(action + " requires stopped state or core mode");
    }

    if (action == "continue") {
        return state == SessionState::Stopped ? true : deny("continue requires stopped state");
    }

    if (action == "run" || action == "breakpoint_set" || action == "watchpoint_set" || action == "replay") {
        return (state == SessionState::Ready || state == SessionState::Stopped || state == SessionState::Exited)
                   ? true
                   : deny(action + " requires ready, stopped, or exited state");
    }

    if (action == "catchpoint_set") {
        return (state == SessionState::Ready || state == SessionState::Stopped || state == SessionState::Exited)
                   ? true
                   : deny("catchpoint_set requires ready, stopped, or exited state");
    }

    if (action == "probe_list" || action == "probe_delete" || action == "probe_enable" ||
        action == "probe_disable" || action == "threads") {
        return (state == SessionState::Ready || state == SessionState::Stopped ||
                state == SessionState::Exited || outcome.core_mode)
                   ? true
                   : deny(action + " requires loaded session state");
    }

    return true;
}

static bool valid_syscall_selector(const std::string &selector) {
    if (selector.empty()) {
        return false;
    }
    for (char ch : selector) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || ch == '_')) {
            return false;
        }
    }
    return true;
}





ActionOutput single_output(ActionResult result) {
    ActionOutput output;
    output.final = std::move(result);
    return output;
}

ActionResult make_action_error(GdbSession &session,
                                      ActionKind kind,
                                      const std::string &action_name,
                                      const std::string &message,
                                      const std::string &title,
                                      const std::map<std::string, std::string> &details) {
    auto ev = add_action_tool_error(session, title, action_name, message, details);
    ActionResult result = ActionResult::failure(kind, action_name, message);
    result.evidence_id = ev.id;
    for (const auto &[key, value] : details) {
        if (key == "command_evidence") {
            result.command_evidence_id = value;
        } else {
            action_result_set_string(result, key, value);
        }
    }
    return result;
}

static bool console_action_error_if_needed(GdbSession &session,
                                           const std::string &action_name,
                                           ActionKind kind,
                                           const std::string &title,
                                           const CollectedConsoleEvidence &collected,
                                           ActionResult &result,
                                           const std::map<std::string, std::string> &details = {}) {
    if (collected.result.result_class != "error" && !collected.result.timed_out) {
        return false;
    }
    std::map<std::string, std::string> error_details = details;
    error_details["command_evidence"] = collected.evidence.id;
    result = make_action_error(session,
                               kind,
                               action_name,
                               command_error_message(collected.result, "GDB rejected " + action_name),
                               title,
                               error_details);
    return true;
}

static bool guard_action_state_result(GdbSession &session,
                                      const SessionOutcome *outcome,
                                      const std::string &action_name,
                                      ActionKind kind,
                                      ActionResult &result) {
    if (outcome == nullptr) {
        return true;
    }
    std::string reason;
    if (action_allowed_in_state(*outcome, action_name, reason)) {
        return true;
    }
    std::ostringstream evidence_text;
    evidence_text << "{\n";
    evidence_text << "  \"action\": " << json_escape(action_name) << ",\n";
    evidence_text << "  \"state\": " << json_escape(std::string(session_state_name(outcome->state))) << ",\n";
    evidence_text << "  \"reason\": " << json_escape(reason) << "\n";
    evidence_text << "}\n";
    auto ev = session.evidence_store().add_text("ToolError", "Action rejected by state guard", action_name, evidence_text.str());
    result = ActionResult::failure(kind, action_name, reason);
    result.evidence_id = ev.id;
    action_result_set_string(result, "state", std::string(session_state_name(outcome->state)));
    return false;
}


ActionOutput dispatch_action(ActionContext &context, const ActionRequest &request) {
    GdbSession &session = context.session;
    const DebugTask *task = context.task;
    SessionOutcome *outcome = context.outcome;
    ProbeState &probe_state = context.probe_state;
    std::string action_name = request.action.empty() ? action_kind_name(request.kind) : request.action;
    if (action_name.empty()) {
        return single_output(make_action_error(session, ActionKind::Unknown, "", "missing action"));
    }
    ActionResult guard_error;
    if (!guard_action_state_result(session, outcome, action_name, request.kind, guard_error)) {
        return single_output(std::move(guard_error));
    }

    switch (request.kind) {
        case ActionKind::FinishSession: {
            const auto &payload = std::get<FinishPayload>(request.payload);
            if (outcome != nullptr) {
                if (!payload.agent_inference.empty()) {
                    outcome->agent_inference = payload.agent_inference;
                }
                if (!payload.final_conclusion.empty()) {
                    outcome->final_agent_conclusion = payload.final_conclusion;
                }
            }
            ActionResult result = ActionResult::success(ActionKind::FinishSession, "finish_session");
            result.finished = true;
            return single_output(std::move(result));
        }
        case ActionKind::Backtrace:
        case ActionKind::Locals:
        case ActionKind::Registers:
        case ActionKind::Threads:
        case ActionKind::ArgsInfo: {
            const auto &payload = std::get<TimeoutPayload>(request.payload);
            std::string title;
            std::string command;
            bool terminal = false;
            if (request.kind == ActionKind::Backtrace) {
                title = "Backtrace";
                command = "bt";
                terminal = true;
            } else if (request.kind == ActionKind::Locals) {
                title = "Local variables";
                command = "info locals";
            } else if (request.kind == ActionKind::Registers) {
                title = "Registers";
                command = "info registers";
            } else if (request.kind == ActionKind::Threads) {
                title = "Threads";
                command = "info threads";
            } else {
                title = "Frame arguments";
                command = "info args";
            }
            auto collected = collect_console_with_result(session,
                                                         title,
                                                         command,
                                                         terminal,
                                                         std::chrono::milliseconds(payload.timeout_ms));
            ActionResult error;
            if (console_action_error_if_needed(session, action_name, request.kind, title + " failed", collected, error)) {
                return single_output(std::move(error));
            }
            ActionResult result = ActionResult::success(request.kind, action_name);
            result.evidence_id = collected.evidence.id;
            return single_output(std::move(result));
        }
        case ActionKind::FrameSelect: {
            const auto &payload = std::get<FrameSelectPayload>(request.payload);
            std::string console_command = "frame " + std::to_string(payload.frame);
            auto command_result = session.command("-interpreter-exec console " + mi_quote(console_command),
                                                  std::chrono::milliseconds(payload.timeout_ms));
            auto ev = session.evidence_store().add("GdbCommand", "Frame select", console_command, command_result.raw_lines, false, command_result.record_sequences);
            if (command_result.result_class == "error" || command_result.timed_out) {
                return single_output(make_action_error(session,
                                                       request.kind,
                                                       action_name,
                                                       command_error_message(command_result, "GDB rejected frame_select"),
                                                       "Frame select failed",
                                                       {{"command_evidence", ev.id}, {"frame", std::to_string(payload.frame)}}));
            }
            ActionResult result = ActionResult::success(request.kind, action_name);
            result.evidence_id = ev.id;
            action_result_set_int(result, "frame", payload.frame);
            return single_output(std::move(result));
        }
        case ActionKind::Evaluate: {
            const auto &payload = std::get<EvaluatePayload>(request.payload);
            if (payload.expression.empty()) {
                return single_output(make_action_error(session, request.kind, action_name, "missing expression"));
            }
            std::string console_command = "p " + payload.expression;
            auto command_result = session.command("-interpreter-exec console " + mi_quote(console_command),
                                                  std::chrono::milliseconds(payload.timeout_ms));
            auto ev = session.evidence_store().add("GdbCommand", "Evaluate", console_command, command_result.raw_lines, false, command_result.record_sequences);
            if (command_result.result_class == "error" || command_result.timed_out) {
                return single_output(make_action_error(session,
                                                       request.kind,
                                                       action_name,
                                                       command_error_message(command_result, "GDB rejected evaluate"),
                                                       "Evaluate failed",
                                                       {{"command_evidence", ev.id}, {"expression", payload.expression}}));
            }
            ActionResult result = ActionResult::success(request.kind, action_name);
            result.evidence_id = ev.id;
            return single_output(std::move(result));
        }
#define RETURN_PROBE_SET_ERROR(kind_value, action_value, message_value, title_value) \
        return single_output(make_action_error(session, kind_value, action_value, message_value, title_value))
        case ActionKind::BreakpointSet: {
            const auto &payload = std::get<BreakpointSetPayload>(request.payload);
            if (payload.location.empty()) {
                return single_output(make_action_error(session, request.kind, action_name, "missing location"));
            }
            if (!payload.on_hit.error.empty()) {
                RETURN_PROBE_SET_ERROR(request.kind, action_name, "invalid on_hit policy", "Breakpoint on-hit policy failed");
            }
            auto insert = session.command("-break-insert " + mi_quote(payload.location));
            add_command_evidence(session, "GdbCommand", "Breakpoint set", insert);
            std::string number = breakpoint_number_from(insert);
            if (insert.result_class == "error" || number.empty()) {
                RETURN_PROBE_SET_ERROR(request.kind,
                                       action_name,
                                       "failed to set breakpoint",
                                       "Breakpoint set failed");
            }
            std::string condition_evidence;
            if (!payload.condition.empty()) {
                auto cond = session.command("-break-condition " + number + " " + payload.condition);
                auto ev = session.evidence_store().add("GdbCommand", "Breakpoint condition", cond.command, cond.raw_lines, false, cond.record_sequences);
                condition_evidence = ev.id;
                if (cond.result_class == "error") {
                    ActionResult error = make_action_error(session,
                                                           request.kind,
                                                           action_name,
                                                           "failed to set breakpoint condition",
                                                           "Breakpoint condition failed",
                                                           {{"condition_evidence", condition_evidence}});
                    action_result_set_string(error, "breakpoint", number);
                    return single_output(std::move(error));
                }
            }
            ProbeState::ProbeInfo probe;
            probe.number = number;
            probe.kind = "breakpoint";
            probe.location = payload.location;
            probe.condition = payload.condition;
            probe.comment = payload.comment;
            probe.purpose = payload.purpose;
            probe.on_hit_policy = payload.on_hit;
            probe_state.probes_by_number[number] = std::move(probe);
            ActionResult result = ActionResult::success(request.kind, action_name);
            action_result_set_string(result, "breakpoint", number);
            if (!condition_evidence.empty()) {
                action_result_set_string(result, "condition_evidence", condition_evidence);
            }
            return single_output(std::move(result));
        }
        case ActionKind::WatchpointSet: {
            const auto &payload = std::get<WatchpointSetPayload>(request.payload);
            if (payload.expression.empty()) {
                return single_output(make_action_error(session, request.kind, action_name, "missing expression"));
            }
            if (!payload.on_hit.error.empty()) {
                RETURN_PROBE_SET_ERROR(request.kind, action_name, "invalid on_hit policy", "Watchpoint on-hit policy failed");
            }
            auto command_result = session.command("-break-watch " + payload.expression);
            auto ev = session.evidence_store().add("GdbCommand", "Watchpoint set", command_result.command, command_result.raw_lines, false, command_result.record_sequences);
            std::string number = breakpoint_number_from(command_result);
            if (command_result.result_class == "error" || number.empty()) {
                ActionResult error = make_action_error(session,
                                                       request.kind,
                                                       action_name,
                                                       "failed to set watchpoint",
                                                       "Watchpoint set failed",
                                                       {{"command_evidence", ev.id}});
                return single_output(std::move(error));
            }
            std::string condition_evidence;
            if (!payload.condition.empty()) {
                auto cond = session.command("-break-condition " + number + " " + payload.condition);
                auto cond_ev = session.evidence_store().add("GdbCommand", "Watchpoint condition", cond.command, cond.raw_lines, false, cond.record_sequences);
                condition_evidence = cond_ev.id;
                if (cond.result_class == "error") {
                    ActionResult error = make_action_error(session,
                                                           request.kind,
                                                           action_name,
                                                           "failed to set watchpoint condition",
                                                           "Watchpoint condition failed",
                                                           {{"condition_evidence", condition_evidence}});
                    action_result_set_string(error, "watchpoint", number);
                    return single_output(std::move(error));
                }
            }
            ProbeState::ProbeInfo probe;
            probe.number = number;
            probe.kind = "watchpoint";
            probe.expression = payload.expression;
            probe.condition = payload.condition;
            probe.comment = payload.comment;
            probe.purpose = payload.purpose;
            probe.on_hit_policy = payload.on_hit;
            probe_state.probes_by_number[number] = std::move(probe);
            ActionResult result = ActionResult::success(request.kind, action_name);
            result.evidence_id = ev.id;
            action_result_set_string(result, "watchpoint", number);
            if (!condition_evidence.empty()) {
                action_result_set_string(result, "condition_evidence", condition_evidence);
            }
            return single_output(std::move(result));
        }
        case ActionKind::CatchpointSet: {
            const auto &payload = std::get<CatchpointSetPayload>(request.payload);
            if (payload.event.empty()) {
                RETURN_PROBE_SET_ERROR(request.kind, action_name, "missing event", "Catchpoint set failed");
            }
            std::string command;
            if (payload.event == "throw") {
                command = "catch throw";
            } else if (payload.event == "catch") {
                command = "catch catch";
            } else if (payload.event == "syscall") {
                if (!payload.selector.empty() && !valid_syscall_selector(payload.selector)) {
                    ActionResult error = make_action_error(session, request.kind, action_name, "invalid syscall selector", "Catchpoint set failed");
                    action_result_set_string(error, "event", payload.event);
                    action_result_set_string(error, "selector", payload.selector);
                    return single_output(std::move(error));
                }
                command = payload.selector.empty() ? "catch syscall" : "catch syscall " + payload.selector;
            } else if (payload.event == "fork") {
                command = "catch fork";
            } else if (payload.event == "vfork") {
                command = "catch vfork";
            } else if (payload.event == "exec") {
                command = "catch exec";
            } else {
                ActionResult error = make_action_error(session, request.kind, action_name, "unsupported catchpoint event", "Catchpoint set failed");
                action_result_set_string(error, "event", payload.event);
                return single_output(std::move(error));
            }
            if (!payload.on_hit.error.empty()) {
                RETURN_PROBE_SET_ERROR(request.kind, action_name, "invalid on_hit policy", "Catchpoint on-hit policy failed");
            }
            auto command_result = session.command("-interpreter-exec console " + mi_quote(command));
            auto ev = session.evidence_store().add("GdbCommand", "Catchpoint set", command_result.command, command_result.raw_lines, false, command_result.record_sequences);
            std::string number = breakpoint_number_from(command_result);
            if (command_result.result_class == "error" || number.empty()) {
                ActionResult error = make_action_error(session,
                                                       request.kind,
                                                       action_name,
                                                       "failed to set catchpoint",
                                                       "Catchpoint set failed",
                                                       {{"command_evidence", ev.id}});
                action_result_set_string(error, "event", payload.event);
                action_result_set_string(error, "selector", payload.selector);
                return single_output(std::move(error));
            }
            ProbeState::ProbeInfo probe;
            probe.number = number;
            probe.kind = "catchpoint";
            probe.event = payload.event;
            probe.selector = payload.selector;
            probe.location = command;
            probe.comment = payload.comment;
            probe.purpose = payload.purpose;
            probe.on_hit_policy = payload.on_hit;
            probe_state.probes_by_number[number] = std::move(probe);
            ActionResult result = ActionResult::success(request.kind, action_name);
            result.evidence_id = ev.id;
            action_result_set_string(result, "catchpoint", number);
            action_result_set_string(result, "event", payload.event);
            action_result_set_string(result, "selector", payload.selector);
            return single_output(std::move(result));
        }
        case ActionKind::ProbeList: {
            auto command_result = session.command("-break-list");
            auto ev = session.evidence_store().add("GdbCommand", "Probe list", command_result.command, command_result.raw_lines, false, command_result.record_sequences);
            std::ostringstream evidence_text;
            evidence_text << "{\n";
            evidence_text << "  \"probes\": " << probe_array_json(probe_state) << "\n";
            evidence_text << "}\n";
            auto metadata_ev = session.evidence_store().add_text("SessionEvent", "Probe metadata snapshot", "probe_list", evidence_text.str());
            ActionResult result = ActionResult::success(request.kind, action_name);
            result.evidence_id = ev.id;
            action_result_set_string(result, "metadata_evidence", metadata_ev.id);
            action_result_set_array(result, "probes", probe_array_result(probe_state));
            return single_output(std::move(result));
        }
        case ActionKind::ProbeDelete:
        case ActionKind::ProbeEnable:
        case ActionKind::ProbeDisable: {
            const auto &payload = std::get<ProbeMutationPayload>(request.payload);
            if (payload.number < 0) {
                return single_output(make_action_error(session, request.kind, action_name, "missing probe number"));
            }
            std::string command = "-break-delete ";
            std::string title = "Probe delete";
            if (request.kind == ActionKind::ProbeEnable) {
                command = "-break-enable ";
                title = "Probe enable";
            } else if (request.kind == ActionKind::ProbeDisable) {
                command = "-break-disable ";
                title = "Probe disable";
            }
            auto command_result = session.command(command + std::to_string(payload.number));
            auto ev = session.evidence_store().add("GdbCommand", title, command_result.command, command_result.raw_lines, false, command_result.record_sequences);
            if (command_result.result_class == "error" || command_result.timed_out) {
                return single_output(make_action_error(session,
                                                       request.kind,
                                                       action_name,
                                                       command_error_message(command_result, "GDB rejected probe operation"),
                                                       title + " failed",
                                                       {{"command_evidence", ev.id}, {"number", std::to_string(payload.number)}}));
            }
            auto probe_it = probe_state.probes_by_number.find(std::to_string(payload.number));
            if (probe_it != probe_state.probes_by_number.end()) {
                if (request.kind == ActionKind::ProbeDelete) {
                    probe_it->second.deleted = true;
                    probe_it->second.enabled = false;
                } else if (request.kind == ActionKind::ProbeEnable) {
                    probe_it->second.enabled = true;
                } else if (request.kind == ActionKind::ProbeDisable) {
                    probe_it->second.enabled = false;
                }
            }
            ActionResult result = ActionResult::success(request.kind, action_name);
            result.evidence_id = ev.id;
            action_result_set_int(result, "number", payload.number);
            return single_output(std::move(result));
        }
        case ActionKind::Run: {
            const auto &payload = std::get<RunPayload>(request.payload);
            int default_deadline_ms = outcome != nullptr ? outcome->run_timeout_ms : (task != nullptr ? task->run_timeout_ms : 30000);
            int deadline_ms = payload.deadline_ms > 0 ? payload.deadline_ms : default_deadline_ms;
            CommandResult command_result;
            if (outcome != nullptr) {
                outcome->state = SessionState::Running;
                outcome->inferior_stdout_offset = 0;
                outcome->inferior_stderr_offset = 0;
            }
            if (task != nullptr) {
                DebugTask run_task = *task;
                if (!payload.stdin_path.empty()) {
                    fs::path input(payload.stdin_path);
                    run_task.stdin_path = input.is_absolute()
                                              ? input
                                              : fs::weakly_canonical(run_task.working_directory / input);
                }
                command_result = run_inferior(session, run_task, std::chrono::milliseconds(deadline_ms));
            } else if (payload.stdin_path.empty()) {
                command_result = session.exec_control("-exec-run", std::chrono::milliseconds(deadline_ms));
            } else {
                command_result = session.exec_control("-interpreter-exec console " +
                                                          mi_quote("run < " + shell_quote_for_report(payload.stdin_path)),
                                                      std::chrono::milliseconds(deadline_ms));
            }
            auto ev = session.evidence_store().add("StopEvent", "Run stop", command_result.command, command_result.raw_lines, false, command_result.record_sequences);
            if (outcome != nullptr) {
                update_outcome_from_stop(*outcome, command_result);
            }
            if (command_result.result_class == "error") {
                return single_output(make_action_error(session,
                                                       request.kind,
                                                       action_name,
                                                       command_error_message(command_result, "GDB rejected run"),
                                                       "Run failed",
                                                       {{"command_evidence", ev.id}}));
            }
            if (outcome != nullptr) {
                collect_stop_followup(session, *outcome, command_result);
            }
            ActionOutput output;
            ActionContext stop_context{session, task, outcome, probe_state};
            output.prelude = handle_probe_stop(stop_context, command_result);
            output.final = ActionResult::success(request.kind, action_name);
            output.final.evidence_id = ev.id;
            action_result_set_string(output.final, "stop_reason", command_result.stop_reason);
            action_result_set_string(output.final, "signal", command_result.signal_name);
            return output;
        }
        case ActionKind::Continue: {
            const auto &payload = std::get<ContinuePayload>(request.payload);
            if (outcome != nullptr) {
                outcome->state = SessionState::Running;
            }
            auto command_result = session.exec_control("-exec-continue", std::chrono::milliseconds(payload.deadline_ms));
            auto ev = session.evidence_store().add("StopEvent", "Continue stop", command_result.command, command_result.raw_lines, false, command_result.record_sequences);
            if (outcome != nullptr) {
                update_outcome_from_stop(*outcome, command_result);
            }
            if (command_result.result_class == "error") {
                return single_output(make_action_error(session,
                                                       request.kind,
                                                       action_name,
                                                       command_error_message(command_result, "GDB rejected continue"),
                                                       "Continue failed",
                                                       {{"command_evidence", ev.id}}));
            }
            if (outcome != nullptr) {
                collect_stop_followup(session, *outcome, command_result);
            }
            ActionOutput output;
            ActionContext stop_context{session, task, outcome, probe_state};
            output.prelude = handle_probe_stop(stop_context, command_result);
            output.final = ActionResult::success(request.kind, action_name);
            output.final.evidence_id = ev.id;
            action_result_set_string(output.final, "stop_reason", command_result.stop_reason);
            action_result_set_string(output.final, "signal", command_result.signal_name);
            return output;
        }
        case ActionKind::RawMi: {
            const auto &payload = std::get<RawMiPayload>(request.payload);
            if (payload.command.empty()) {
                return single_output(make_action_error(session, request.kind, action_name, "missing command"));
            }
            if (payload.risk != "advanced") {
                return single_output(make_action_error(session, request.kind, action_name, "raw_mi requires risk=advanced"));
            }
            auto command_result = session.command(payload.command, std::chrono::milliseconds(payload.timeout_ms));
            auto ev = session.evidence_store().add("GdbCommand", "Raw MI", command_result.command, command_result.raw_lines, false, command_result.record_sequences);
            ActionResult result = ActionResult::success(request.kind, action_name);
            result.evidence_id = ev.id;
            action_result_set_string(result, "result_class", command_result.result_class);
            return single_output(std::move(result));
        }
        case ActionKind::HypothesisCreate: {
            const auto &payload = std::get<HypothesisCreatePayload>(request.payload);
            std::string id = payload.id.empty() ? next_hypothesis_id(probe_state.hypotheses) : payload.id;
            std::string title = payload.title.empty() ? "Untitled hypothesis" : payload.title;
            fs::path file = hypothesis_file_for(session, id);
            std::ostringstream md;
            md << "# " << id << " " << title << "\n\n";
            md << "Status: EvidenceCollectionStarted\n\n";
            if (!payload.description.empty()) {
                md << "## Description\n\n" << payload.description << "\n\n";
            }
            write_text_file(file, md.str());
            HypothesisRecord record;
            record.id = id;
            record.title = title;
            record.description = payload.description;
            probe_state.hypotheses.hypotheses_by_id[id] = std::move(record);
            write_hypothesis_index(session, probe_state.hypotheses);
            ActionResult result = ActionResult::success(request.kind, action_name);
            action_result_set_string(result, "id", id);
            action_result_set_string(result, "file", file.lexically_normal().string());
            return single_output(std::move(result));
        }
        case ActionKind::HypothesisCheck: {
            const auto &payload = std::get<HypothesisCheckPayload>(request.payload);
            if (payload.hypothesis.empty() || payload.expression.empty()) {
                return single_output(make_action_error(session, request.kind, action_name, "hypothesis_check requires hypothesis and expression"));
            }
            std::string description = payload.description.empty() ? payload.expression : payload.description;
            auto collected = collect_console_with_result(session, "Hypothesis check", "p " + payload.expression);
            ActionResult error;
            if (console_action_error_if_needed(session,
                                               action_name,
                                               request.kind,
                                               "Hypothesis check command failed",
                                               collected,
                                               error,
                                               {{"hypothesis", payload.hypothesis}, {"expression", payload.expression}})) {
                return single_output(std::move(error));
            }
            const auto &ev = collected.evidence;
            std::string observed = ev.summary;
            auto assertion_result = evaluate_hypothesis_assertion(payload.assertion, observed, payload.expected);
            std::string error_evidence_id;
            if (assertion_result.status == "unknown") {
                std::ostringstream message;
                message << "hypothesis_check assertion could not be evaluated";
                if (!assertion_result.reason.empty()) {
                    message << ": " << assertion_result.reason;
                }
                auto error_ev = add_tool_error(session, "Hypothesis check assertion unknown", action_name, message.str());
                error_evidence_id = error_ev.id;
            }
            fs::path file = hypothesis_file_for(session, payload.hypothesis);
            auto &record = probe_state.hypotheses.hypotheses_by_id[payload.hypothesis];
            if (record.id.empty()) {
                record.id = payload.hypothesis;
                record.title = payload.hypothesis;
            }
            std::ostringstream check_id;
            check_id << "C" << (record.checks.size() + 1);
            std::ostringstream md;
            md << "## Check: " << description << "\n\n";
            md << "- Check ID: `" << check_id.str() << "`\n";
            md << "- Expression: `" << payload.expression << "`\n";
            md << "- Evidence: `" << ev.id << "`\n";
            md << "- Assertion: `" << payload.assertion << "`\n";
            if (!payload.expected.empty()) {
                md << "- Expected: `" << payload.expected << "`\n";
            }
            md << "- Status: `" << assertion_result.status << "`\n";
            if (!error_evidence_id.empty()) {
                md << "- Error evidence: `" << error_evidence_id << "`\n";
            }
            md << "\n### Observed\n\n";
            md << "```text\n" << observed << "\n```\n\n";
            append_hypothesis_text(file, md.str());
            HypothesisCheck check;
            check.id = check_id.str();
            check.description = description;
            check.expression = payload.expression;
            check.assertion = payload.assertion;
            check.expected = payload.expected;
            check.observed = observed;
            check.status = assertion_result.status;
            check.evidence_id = ev.id;
            check.error_evidence_id = error_evidence_id;
            record.checks.push_back(std::move(check));
            if (assertion_result.status == "passed") {
                record.tool_status = "EvidenceSupportsCheck";
            } else if (assertion_result.status == "failed") {
                record.tool_status = "EvidenceContradictsCheck";
            } else {
                record.tool_status = "EvidenceCheckUnknown";
            }
            write_hypothesis_index(session, probe_state.hypotheses);
            ActionResult result = ActionResult::success(request.kind, action_name);
            result.evidence_id = ev.id;
            action_result_set_string(result, "hypothesis", payload.hypothesis);
            action_result_set_string(result, "check_id", record.checks.back().id);
            action_result_set_string(result, "description", description);
            action_result_set_string(result, "expression", payload.expression);
            action_result_set_string(result, "assertion", payload.assertion);
            action_result_set_string(result, "expected", payload.expected);
            action_result_set_string(result, "observed", observed);
            action_result_set_string(result, "status", assertion_result.status);
            if (error_evidence_id.empty()) {
                action_result_set_null(result, "error_evidence");
            } else {
                action_result_set_string(result, "error_evidence", error_evidence_id);
            }
            return single_output(std::move(result));
        }
        case ActionKind::HypothesisConclude: {
            const auto &payload = std::get<HypothesisConcludePayload>(request.payload);
            if (payload.hypothesis.empty()) {
                return single_output(make_action_error(session, request.kind, action_name, "hypothesis_conclude requires hypothesis"));
            }
            fs::path file = hypothesis_file_for(session, payload.hypothesis);
            std::ostringstream md;
            md << "## Agent Conclusion\n\n";
            md << "- Conclusion: `" << payload.conclusion << "`\n";
            if (!payload.inference.empty()) {
                md << "\n" << payload.inference << "\n";
            }
            md << "\n";
            append_hypothesis_text(file, md.str());
            auto &record = probe_state.hypotheses.hypotheses_by_id[payload.hypothesis];
            if (record.id.empty()) {
                record.id = payload.hypothesis;
                record.title = payload.hypothesis;
            }
            record.agent_conclusion = payload.conclusion;
            record.agent_inference = payload.inference;
            write_hypothesis_index(session, probe_state.hypotheses);
            ActionResult result = ActionResult::success(request.kind, action_name);
            action_result_set_string(result, "hypothesis", payload.hypothesis);
            action_result_set_string(result, "conclusion", payload.conclusion);
            return single_output(std::move(result));
        }
        case ActionKind::SaveAction: {
            const auto &payload = std::get<SaveActionPayload>(request.payload);
            std::string failure_policy = normalize_replay_failure_policy(payload.failure_policy);
            if (payload.name.empty() || !payload.saved_action) {
                return single_output(make_action_error(session, request.kind, action_name, "save_action requires name and saved_action"));
            }
            fs::path replay_dir = session.assets_dir() / "replay";
            fs::create_directories(replay_dir);
            std::string replay_base = slugify(payload.name);
            fs::path replay_file = replay_dir / (replay_base + ".jsonl");
            fs::path replay_plan = replay_dir / (replay_base + ".json");
            std::ofstream replay_out(replay_file, std::ios::app);
            if (!replay_out) {
                return single_output(make_action_error(session, request.kind, action_name, "failed to write replay file"));
            }
            replay_out << dump_json(action_request_to_json(*payload.saved_action)) << '\n';
            replay_out.close();
            try {
                rebuild_replay_plan_from_jsonl(replay_file,
                                               replay_plan,
                                               payload.name,
                                               task,
                                               session.session_id(),
                                               failure_policy);
            } catch (const std::exception &ex) {
                auto ev = session.evidence_store().add_text("ToolError",
                                                            "Replay plan write failed",
                                                            replay_plan.lexically_normal().string(),
                                                            ex.what());
                ActionResult error = ActionResult::failure(request.kind, action_name, "failed to write replay plan");
                error.evidence_id = ev.id;
                return single_output(std::move(error));
            }
            ActionResult result = ActionResult::success(request.kind, action_name);
            action_result_set_string(result, "file", replay_file.lexically_normal().string());
            action_result_set_string(result, "plan", replay_plan.lexically_normal().string());
            action_result_set_string(result, "failure_policy", failure_policy);
            return single_output(std::move(result));
        }
        case ActionKind::Replay: {
            const auto &payload = std::get<ReplayPayload>(request.payload);
            fs::path replay_file;
            if (!payload.file.empty()) {
                replay_file = payload.file;
            } else if (!payload.name.empty()) {
                fs::path replay_dir = session.assets_dir() / "replay";
                fs::path record = replay_dir / (slugify(payload.name) + ".gar");
                fs::path plan = replay_dir / (slugify(payload.name) + ".json");
                fs::path jsonl = replay_dir / (slugify(payload.name) + ".jsonl");
                replay_file = fs::exists(record) ? record : (fs::exists(plan) ? plan : jsonl);
            } else {
                return single_output(make_action_error(session, request.kind, action_name, "replay requires file or name"));
            }
            try {
                ActionContext replay_context{session, task, outcome, probe_state};
                return replay_action_file(replay_context,
                                          replay_file,
                                          payload.force,
                                          payload.failure_policy);
            } catch (const std::exception &ex) {
                return single_output(make_action_error(session,
                                                       request.kind,
                                                       action_name,
                                                       ex.what(),
                                                       "Replay failed",
                                                       {{"file", replay_file.lexically_normal().string()}}));
            }
        }
        case ActionKind::RecordStart: {
            const auto &payload = std::get<RecordStartPayload>(request.payload);
            if (context.recording == nullptr) {
                return single_output(make_action_error(session, request.kind, action_name, "recording state is not available"));
            }
            if (payload.name.empty()) {
                return single_output(make_action_error(session, request.kind, action_name, "record_start requires name"));
            }
            if (context.recording->active) {
                return single_output(make_action_error(session,
                                                       request.kind,
                                                       action_name,
                                                       "recording is already active",
                                                       "Record start failed",
                                                       {{"name", context.recording->name}}));
            }
            recording_start(*context.recording,
                            payload.name,
                            payload.failure_policy,
                            payload.include_raw_mi);
            ActionResult result = ActionResult::success(request.kind, action_name);
            action_result_set_bool(result, "active", true);
            action_result_set_string(result, "name", context.recording->name);
            action_result_set_string(result, "failure_policy", context.recording->failure_policy);
            action_result_set_bool(result, "include_raw_mi", context.recording->include_raw_mi);
            action_result_set_int(result, "step_count", 0);
            return single_output(std::move(result));
        }
        case ActionKind::RecordStatus: {
            if (context.recording == nullptr) {
                return single_output(make_action_error(session, request.kind, action_name, "recording state is not available"));
            }
            ActionResult result = ActionResult::success(request.kind, action_name);
            action_result_set_bool(result, "active", context.recording->active);
            action_result_set_string(result, "name", context.recording->name);
            action_result_set_string(result, "failure_policy", context.recording->failure_policy);
            action_result_set_bool(result, "include_raw_mi", context.recording->include_raw_mi);
            action_result_set_int(result, "step_count", static_cast<int>(context.recording->actions.size()));
            action_result_set_string(result, "binary", context.recording->binary_path.lexically_normal().string());
            action_result_set_string(result, "export", context.recording->export_path.lexically_normal().string());
            return single_output(std::move(result));
        }
        case ActionKind::RecordDiscard: {
            if (context.recording == nullptr) {
                return single_output(make_action_error(session, request.kind, action_name, "recording state is not available"));
            }
            if (!context.recording->active) {
                return single_output(make_action_error(session, request.kind, action_name, "no active recording"));
            }
            int step_count = static_cast<int>(context.recording->actions.size());
            std::string name = context.recording->name;
            recording_clear(*context.recording);
            ActionResult result = ActionResult::success(request.kind, action_name);
            action_result_set_string(result, "name", name);
            action_result_set_int(result, "discarded_steps", step_count);
            return single_output(std::move(result));
        }
        case ActionKind::RecordStop: {
            if (context.recording == nullptr) {
                return single_output(make_action_error(session, request.kind, action_name, "recording state is not available"));
            }
            if (!context.recording->active) {
                return single_output(make_action_error(session, request.kind, action_name, "no active recording"));
            }
            try {
                RecordPersistResult persisted = persist_recording(*context.recording,
                                                                  task,
                                                                  session.session_id(),
                                                                  session.assets_dir());
                std::string name = context.recording->name;
                std::string policy = context.recording->failure_policy;
                context.recording->active = false;
                context.recording->actions.clear();
                context.recording->binary_path = persisted.binary_path;
                context.recording->export_path = persisted.export_path;
                ActionResult result = ActionResult::success(request.kind, action_name);
                action_result_set_string(result, "name", name);
                action_result_set_string(result, "failure_policy", policy);
                action_result_set_int(result, "step_count", persisted.step_count);
                action_result_set_string(result, "binary", persisted.binary_path.lexically_normal().string());
                action_result_set_string(result, "export", persisted.export_path.lexically_normal().string());
                return single_output(std::move(result));
            } catch (const std::exception &ex) {
                return single_output(make_action_error(session, request.kind, action_name, ex.what(), "Record stop failed"));
            }
        }
        case ActionKind::Unknown:
            break;
    }
#undef RETURN_PROBE_SET_ERROR
    return single_output(make_action_error(session, request.kind, action_name, "unsupported action", "Unsupported action"));
}

ActionOutput handle_action_request(ActionContext &context, const ActionRequest &request) {
    return execute_session_operation(context, request);
}

ActionOutput handle_action_json(ActionContext &context, const Json &action) {
    GdbSession &session = context.session;
    if (!action.is_object()) {
        return single_output(make_action_error(session, ActionKind::Unknown, "", "action must be a JSON object"));
    }
    ActionRequest request = parse_action_request(action);
    if (request.action.empty()) {
        return single_output(make_action_error(session, ActionKind::Unknown, "", "missing action"));
    }
    return handle_action_request(context, request);
}
