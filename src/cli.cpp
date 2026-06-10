#include "cli.hpp"

#include "cli/action.hpp"
#include "common/string_utils.hpp"
#include "common/json.hpp"
#include "gdb/gdb_session.hpp"
#include "gdb/mi_utils.hpp"
#include "replay/replay_plan.hpp"
#include "report/report.hpp"
#include "task/debug_task.hpp"
#include "workflow/crash_workflow.hpp"
#include "workflow/hypothesis.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string>
#include <map>
#include <vector>
#include <cstdio>
#include <chrono>
#include <cctype>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;

struct CliOptions {
    std::string command;
    fs::path task_file;
    fs::path assets = "report.assets";
    fs::path report = "report.md";
    fs::path replay_before_run;
    std::string session_id = "S1";
    int run_timeout_ms = -1;
};

struct ProbeState {
    using OnHitPolicy = OnHitPolicyRequest;

    struct OnHitActionResult {
        int index = 0;
        std::string action_name;
        std::string status;
        std::string failure_policy;
        std::string evidence_id;
        std::vector<std::string> action_evidence_ids;
        std::string error_evidence_id;
        std::string error;
        std::string skip_reason;
    };

    struct ProbeHitSnapshot {
        std::string number;
        std::string kind;
        std::string stop_reason;
        std::string signal_name;
        std::string event;
        std::string selector;
        std::string location;
        std::string expression;
        std::string condition;
        std::string comment;
        std::string purpose;
        int hit_count = 0;
        OnHitPolicy on_hit_policy;
        bool known_probe = false;
    };

    struct ProbeInfo {
        std::string number;
        std::string kind;
        std::string event;
        std::string selector;
        std::string location;
        std::string expression;
        std::string condition;
        std::string comment;
        std::string purpose;
        bool enabled = true;
        bool deleted = false;
        int hit_count = 0;
        std::string last_stop_reason;
        OnHitPolicy on_hit_policy;
    };

    struct HypothesisCheck {
        std::string id;
        std::string description;
        std::string expression;
        std::string assertion;
        std::string expected;
        std::string observed;
        std::string status;
        std::string evidence_id;
        std::string error_evidence_id;
    };

    struct HypothesisRecord {
        std::string id;
        std::string title;
        std::string description;
        std::string tool_status = "EvidenceCollectionStarted";
        std::string agent_conclusion;
        std::string agent_inference;
        std::vector<HypothesisCheck> checks;
    };

    std::map<std::string, ProbeInfo> probes_by_number;
    std::map<std::string, HypothesisRecord> hypotheses_by_id;
    int hypothesis_counter = 0;
};

static fs::path default_assets_for(const fs::path &report) {
    return report.string() + ".assets";
}

static CliOptions parse_cli(int argc, char **argv) {
    if (argc < 3) {
        throw std::runtime_error("usage: gdb-agent <check|serve> task.md [--assets dir] [--out report.md] [--session id]");
    }
    CliOptions opts;
    opts.command = argv[1];
    opts.task_file = argv[2];
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        auto require_value = [&](const std::string &name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + name);
            }
            return argv[++i];
        };
        if (arg == "--assets") {
            opts.assets = require_value(arg);
        } else if (arg == "--out") {
            opts.report = require_value(arg);
            if (opts.assets == "report.assets") {
                opts.assets = default_assets_for(opts.report);
            }
        } else if (arg == "--session") {
            opts.session_id = require_value(arg);
        } else if (arg == "--replay-before-run") {
            opts.replay_before_run = require_value(arg);
        } else if (arg == "--run-timeout-ms") {
            opts.run_timeout_ms = std::stoi(require_value(arg));
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    return opts;
}

static ActionOutput handle_action_request(GdbSession &session,
                                          const DebugTask *task,
                                          SessionOutcome *outcome,
                                          ProbeState &probe_state,
                                          const ActionRequest &request);
static ActionOutput handle_action_json(GdbSession &session,
                                       const DebugTask *task,
                                       SessionOutcome *outcome,
                                       ProbeState &probe_state,
                                       const Json &action);
static ActionOutput replay_action_file(GdbSession &session,
                                       const DebugTask *task,
                                       SessionOutcome *outcome,
                                       ProbeState &probe_state,
                                       const fs::path &path,
                                       bool force = false,
                                       const std::string &failure_policy_override = "");
static std::string json_escape(const std::string &s);
static std::string json_string_array(const std::vector<std::string> &items);
static std::string json_string_map(const std::map<std::string, std::string> &items);
static void flush_inferior_output(GdbSession &session, SessionOutcome &outcome);
static int effective_run_timeout_ms(const CliOptions &opts, const DebugTask &task) {
    return opts.run_timeout_ms > 0 ? opts.run_timeout_ms : task.run_timeout_ms;
}

static void collect_stop_followup(GdbSession &session,
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

static std::string watchpoint_number_from_stop_record(const CommandResult &result) {
    if (!result.breakpoint_number.empty()) {
        return result.breakpoint_number;
    }
    for (const auto &raw : result.raw_lines) {
        const std::string wpt_prefix = "wpt={";
        auto pos = raw.find(wpt_prefix);
        if (pos == std::string::npos) {
            continue;
        }
        auto number_pos = raw.find("number=\"", pos + wpt_prefix.size());
        if (number_pos == std::string::npos) {
            continue;
        }
        number_pos += std::string("number=\"").size();
        std::string number;
        bool escaped = false;
        for (; number_pos < raw.size(); ++number_pos) {
            char c = raw[number_pos];
            if (escaped) {
                number.push_back(c);
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"') {
                break;
            }
            number.push_back(c);
        }
        if (!number.empty()) {
            return number;
        }
    }
    return {};
}

static CommandResult run_inferior(GdbSession &session,
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

static void update_outcome_from_stop(SessionOutcome &outcome, const CommandResult &result) {
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

static void flush_inferior_output(GdbSession &session, SessionOutcome &outcome) {
    collect_inferior_output(session, outcome.inferior_stdout_offset, outcome.inferior_stderr_offset);
}

static void set_inferior_output_paths(SessionOutcome &outcome, const fs::path &assets) {
    outcome.inferior_stdout = (assets / "inferior" / "stdout.log").lexically_normal().string();
    outcome.inferior_stderr = (assets / "inferior" / "stderr.log").lexically_normal().string();
}

static std::string file_metadata_json(const fs::path &path) {
    std::ostringstream out;
    out << "{";
    out << "\"path\":" << json_escape(path.string());
    if (fs::exists(path)) {
        out << ",\"exists\":true";
        if (fs::is_regular_file(path)) {
            out << ",\"size\":" << fs::file_size(path);
        }
        auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         fs::last_write_time(path).time_since_epoch())
                         .count();
        out << ",\"mtime_ticks\":" << ticks;
    } else {
        out << ",\"exists\":false";
    }
    out << "}";
    return out.str();
}

static void collect_environment_info(GdbSession &session,
                                     const DebugTask &task,
                                     const SessionOutcome &outcome) {
    auto gdb_version = session.command("-gdb-version", std::chrono::milliseconds(5000));
    session.evidence_store().add("EnvironmentInfo", "GDB version", gdb_version.command, gdb_version.raw_lines, false, gdb_version.record_sequences);

    std::ostringstream text;
    text << "{\n";
    text << "  \"executable\": " << file_metadata_json(task.executable) << ",\n";
    text << "  \"core_dump\": "
         << (task.core_dump ? file_metadata_json(*task.core_dump) : std::string("null")) << ",\n";
    text << "  \"working_directory\": " << json_escape(task.working_directory.string()) << ",\n";
    text << "  \"argv\": " << json_string_array(task.args) << ",\n";
    text << "  \"stdin\": " << json_escape(task.stdin_path.string()) << ",\n";
    text << "  \"stdout\": " << json_escape(outcome.inferior_stdout) << ",\n";
    text << "  \"stderr\": " << json_escape(outcome.inferior_stderr) << ",\n";
    text << "  \"env\": " << json_string_map(task.env) << "\n";
    text << "}\n";
    session.evidence_store().add_text("EnvironmentInfo", "Debug target metadata", "task metadata", text.str());
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
        action == "save_action" || action == "raw_mi") {
        return true;
    }

    if (action == "backtrace" || action == "locals" || action == "args_info" ||
        action == "registers" || action == "frame_select" || action == "evaluate" ||
        action == "hypothesis_check" || action == "watchpoint_set") {
        return state_is_stopped_or_core(outcome)
                   ? true
                   : deny(action + " requires stopped state or core mode");
    }

    if (action == "continue") {
        return state == SessionState::Stopped ? true : deny("continue requires stopped state");
    }

    if (action == "run" || action == "breakpoint_set" || action == "replay") {
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

static std::string json_action_array(const std::vector<ActionRequestPtr> &actions) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < actions.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << (actions[i] ? dump_json(action_request_to_json(*actions[i])) : std::string("null"));
    }
    out << "]";
    return out.str();
}

static std::string json_string_vector(const std::vector<std::string> &items) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << json_escape(items[i]);
    }
    out << "]";
    return out.str();
}

static std::string on_hit_policy_json(const ProbeState::OnHitPolicy &policy) {
    std::ostringstream out;
    out << "{";
    out << "\"configured\":" << (policy.configured ? "true" : "false") << ",";
    out << "\"actions\":" << json_action_array(policy.actions) << ",";
    out << "\"timeout_ms\":" << policy.timeout_ms << ",";
    out << "\"max_output_bytes\":" << policy.max_output_bytes << ",";
    out << "\"max_summary_lines\":" << policy.max_summary_lines << ",";
    out << "\"failure_policy\":" << json_escape(policy.failure_policy) << ",";
    out << "\"continue_after_hit\":" << (policy.continue_after_hit ? "true" : "false");
    out << "}";
    return out.str();
}

static void write_probe_snapshot(GdbSession &session, const ProbeState &probe_state) {
    fs::path path = session.assets_dir() / "probes.json";
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"gdb-agent-probe-store-v1\",\n";
    out << "  \"probes\": [\n";
    bool first = true;
    for (const auto &[_, probe] : probe_state.probes_by_number) {
        if (!first) {
            out << ",\n";
        }
        first = false;
        out << "    {\n";
        out << "      \"number\": " << json_escape(probe.number) << ",\n";
        out << "      \"kind\": " << json_escape(probe.kind) << ",\n";
        out << "      \"event\": " << json_escape(probe.event) << ",\n";
        out << "      \"selector\": " << json_escape(probe.selector) << ",\n";
        out << "      \"location\": " << json_escape(probe.location) << ",\n";
        out << "      \"expression\": " << json_escape(probe.expression) << ",\n";
        out << "      \"condition\": " << json_escape(probe.condition) << ",\n";
        out << "      \"comment\": " << json_escape(probe.comment) << ",\n";
        out << "      \"purpose\": " << json_escape(probe.purpose) << ",\n";
        out << "      \"enabled\": " << (probe.enabled ? "true" : "false") << ",\n";
        out << "      \"deleted\": " << (probe.deleted ? "true" : "false") << ",\n";
        out << "      \"hit_count\": " << probe.hit_count << ",\n";
        out << "      \"last_stop_reason\": " << json_escape(probe.last_stop_reason) << ",\n";
        out << "      \"on_hit\": " << on_hit_policy_json(probe.on_hit_policy) << "\n";
        out << "    }";
    }
    out << "\n  ]\n";
    out << "}\n";
    write_text_file(path, out.str());
}

static std::string probe_info_json(const ProbeState::ProbeInfo &probe) {
    std::ostringstream out;
    out << "{";
    out << "\"number\":" << json_escape(probe.number) << ",";
    out << "\"kind\":" << json_escape(probe.kind) << ",";
    out << "\"event\":" << json_escape(probe.event) << ",";
    out << "\"selector\":" << json_escape(probe.selector) << ",";
    out << "\"location\":" << json_escape(probe.location) << ",";
    out << "\"expression\":" << json_escape(probe.expression) << ",";
    out << "\"condition\":" << json_escape(probe.condition) << ",";
    out << "\"comment\":" << json_escape(probe.comment) << ",";
    out << "\"purpose\":" << json_escape(probe.purpose) << ",";
    out << "\"enabled\":" << (probe.enabled ? "true" : "false") << ",";
    out << "\"deleted\":" << (probe.deleted ? "true" : "false") << ",";
    out << "\"hit_count\":" << probe.hit_count << ",";
    out << "\"last_stop_reason\":" << json_escape(probe.last_stop_reason) << ",";
    out << "\"on_hit\":" << on_hit_policy_json(probe.on_hit_policy);
    out << "}";
    return out.str();
}

static std::string probe_array_json(const ProbeState &probe_state, bool include_deleted = false) {
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto &[_, probe] : probe_state.probes_by_number) {
        if (probe.deleted && !include_deleted) {
            continue;
        }
        if (!first) {
            out << ",";
        }
        first = false;
        out << probe_info_json(probe);
    }
    out << "]";
    return out.str();
}

static std::string truncate_on_hit_response(std::string text,
                                            int max_output_bytes,
                                            int max_summary_lines) {
    if (max_summary_lines > 0) {
        int lines = 0;
        size_t pos = 0;
        while (pos < text.size()) {
            if (text[pos] == '\n') {
                ++lines;
                if (lines >= max_summary_lines) {
                    text.resize(pos + 1);
                    text += "... truncated by on_hit.max_summary_lines ...\n";
                    break;
                }
            }
            ++pos;
        }
    }
    if (max_output_bytes > 0 && text.size() > static_cast<size_t>(max_output_bytes)) {
        text.resize(static_cast<size_t>(max_output_bytes));
        text += "\n... truncated by on_hit.max_output_bytes ...\n";
    }
    return text;
}

static ProbeState::ProbeHitSnapshot prepare_probe_hit(ProbeState &probe_state,
                                                      const CommandResult &result) {
    ProbeState::ProbeHitSnapshot hit;
    hit.number = result.stop_reason == "watchpoint-trigger"
                     ? watchpoint_number_from_stop_record(result)
                     : result.breakpoint_number;
    hit.stop_reason = result.stop_reason;
    hit.signal_name = result.signal_name;
    hit.kind = result.stop_reason == "watchpoint-trigger" ? "watchpoint" : "breakpoint";
    if (hit.number.empty() && result.stop_reason == "watchpoint-trigger") {
        std::string only_active_watchpoint;
        for (const auto &[number, probe] : probe_state.probes_by_number) {
            if (probe.kind != "watchpoint" || probe.deleted || !probe.enabled) {
                continue;
            }
            if (!only_active_watchpoint.empty()) {
                only_active_watchpoint.clear();
                break;
            }
            only_active_watchpoint = number;
        }
        hit.number = only_active_watchpoint;
    }
    if (result.breakpoint_number.empty()) {
        if (hit.number.empty()) {
            return hit;
        }
    }

    auto it = probe_state.probes_by_number.find(hit.number);
    if (it != probe_state.probes_by_number.end() && !it->second.kind.empty()) {
        hit.kind = it->second.kind;
    }

    if (it != probe_state.probes_by_number.end()) {
        ProbeState::ProbeInfo &probe = it->second;
        ++probe.hit_count;
        probe.last_stop_reason = result.stop_reason;
        hit.known_probe = true;
        hit.event = probe.event;
        hit.selector = probe.selector;
        hit.location = probe.location;
        hit.expression = probe.expression;
        hit.condition = probe.condition;
        hit.comment = probe.comment;
        hit.purpose = probe.purpose;
        hit.hit_count = probe.hit_count;
        hit.on_hit_policy = probe.on_hit_policy;
    }
    return hit;
}

static std::vector<std::string> evidence_ids_since(const GdbSession &session, size_t first_index) {
    std::vector<std::string> ids;
    const auto &all = session.evidence_store().all();
    for (size_t i = first_index; i < all.size(); ++i) {
        ids.push_back(all[i].id);
    }
    return ids;
}

static std::string on_hit_action_result_json(const ProbeState::OnHitActionResult &result) {
    std::ostringstream out;
    out << "{";
    out << "\"index\":" << result.index << ",";
    out << "\"action_name\":" << json_escape(result.action_name) << ",";
    out << "\"status\":" << json_escape(result.status) << ",";
    out << "\"failure_policy\":" << json_escape(result.failure_policy) << ",";
    out << "\"evidence\":" << json_escape(result.evidence_id) << ",";
    out << "\"action_evidence_ids\":" << json_string_vector(result.action_evidence_ids) << ",";
    out << "\"error_evidence\":" << json_escape(result.error_evidence_id) << ",";
    out << "\"error\":" << json_escape(result.error) << ",";
    out << "\"skip_reason\":" << json_escape(result.skip_reason);
    out << "}";
    return out.str();
}

static std::string on_hit_action_results_json(const std::vector<ProbeState::OnHitActionResult> &results) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < results.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << on_hit_action_result_json(results[i]);
    }
    out << "]";
    return out.str();
}

static std::vector<std::string> on_hit_result_evidence_ids(const std::vector<ProbeState::OnHitActionResult> &results) {
    std::vector<std::string> ids;
    for (const auto &result : results) {
        if (!result.evidence_id.empty()) {
            ids.push_back(result.evidence_id);
        }
        for (const auto &id : result.action_evidence_ids) {
            ids.push_back(id);
        }
    }
    return ids;
}

static std::vector<std::string> on_hit_result_error_ids(const std::vector<ProbeState::OnHitActionResult> &results) {
    std::vector<std::string> ids;
    for (const auto &result : results) {
        if (!result.error_evidence_id.empty()) {
            ids.push_back(result.error_evidence_id);
        }
    }
    return ids;
}

static ProbeState::OnHitActionResult add_on_hit_wrapper_evidence(
    GdbSession &session,
    const ProbeState::ProbeHitSnapshot &hit,
    const ProbeState::OnHitPolicy &policy,
    ProbeState::OnHitActionResult result,
    const ActionRequest &action_request,
    const std::string &response_text) {
    std::ostringstream evidence_text;
    evidence_text << "{\n";
    evidence_text << "  \"probe_number\": " << json_escape(hit.number) << ",\n";
    evidence_text << "  \"probe_kind\": " << json_escape(hit.kind) << ",\n";
    evidence_text << "  \"hit_count\": " << hit.hit_count << ",\n";
    evidence_text << "  \"index\": " << result.index << ",\n";
    evidence_text << "  \"action_name\": " << json_escape(result.action_name) << ",\n";
    evidence_text << "  \"status\": " << json_escape(result.status) << ",\n";
    evidence_text << "  \"failure_policy\": " << json_escape(result.failure_policy) << ",\n";
    evidence_text << "  \"action\": " << dump_json(action_request_to_json(action_request)) << ",\n";
    evidence_text << "  \"action_evidence_ids\": " << json_string_vector(result.action_evidence_ids) << ",\n";
    evidence_text << "  \"error_evidence\": " << json_escape(result.error_evidence_id) << ",\n";
    evidence_text << "  \"error\": " << json_escape(result.error) << ",\n";
    evidence_text << "  \"skip_reason\": " << json_escape(result.skip_reason) << ",\n";
    evidence_text << "  \"response\": "
                  << json_escape(truncate_on_hit_response(response_text,
                                                          policy.max_output_bytes,
                                                          policy.max_summary_lines))
                  << "\n";
    evidence_text << "}\n";
    auto ev = session.evidence_store().add_text("OnHitAction",
                                                "On-hit action " + std::to_string(result.index) +
                                                    " for " + hit.kind + " " + hit.number,
                                                result.action_name,
                                                evidence_text.str());
    result.evidence_id = ev.id;
    return result;
}

static std::vector<ProbeState::OnHitActionResult> run_on_hit_actions(
    GdbSession &session,
    const DebugTask *task,
    SessionOutcome *outcome,
    ProbeState &probe_state,
    const ProbeState::ProbeHitSnapshot &hit,
    std::vector<ActionResult> &prelude) {
    std::vector<ProbeState::OnHitActionResult> results;
    const auto &policy = hit.on_hit_policy;
    if (hit.number.empty() || policy.actions.empty()) {
        return results;
    }

    bool stop_remaining = false;
    for (size_t i = 0; i < policy.actions.size(); ++i) {
        ProbeState::OnHitActionResult result;
        result.index = static_cast<int>(i + 1);
        result.failure_policy = policy.failure_policy;
        const ActionRequestPtr &configured_action = policy.actions[i];
        result.action_name = configured_action ? configured_action->action : "";
        if (result.action_name.empty()) {
            result.action_name = "unknown";
        }

        if (stop_remaining) {
            result.status = "skipped";
            result.skip_reason = "previous on_hit action failed with stop_on_error";
            if (configured_action) {
                result = add_on_hit_wrapper_evidence(session, hit, policy, std::move(result), *configured_action, "");
            }
            results.push_back(std::move(result));
            continue;
        }

        if (!configured_action) {
            result.status = "failed";
            result.error = "missing on_hit action";
            results.push_back(std::move(result));
            continue;
        }
        ActionRequest action_request = with_timeout_defaults(*configured_action, policy.timeout_ms);
        size_t evidence_start = session.evidence_store().all().size();
        ActionOutput action_output = handle_action_request(session, task, outcome, probe_state, action_request);
        std::string response_text = action_output_text(action_output);
        prelude.insert(prelude.end(), action_output.prelude.begin(), action_output.prelude.end());
        prelude.push_back(action_output.final);
        const ActionResult &action_result = action_output.final;
        result.action_evidence_ids = evidence_ids_since(session, evidence_start);
        if (!action_result.ok) {
            result.status = "failed";
            result.error = action_result.error.empty() ? "on_hit action returned ok:false" : action_result.error;
            result.error_evidence_id = action_result.evidence_id;
            if (result.error_evidence_id.empty() && !result.action_evidence_ids.empty()) {
                result.error_evidence_id = result.action_evidence_ids.back();
            }
            if (policy.failure_policy == "stop_on_error") {
                stop_remaining = true;
            }
        } else {
            result.status = "success";
        }
        result = add_on_hit_wrapper_evidence(session, hit, policy, std::move(result), action_request, response_text);
        results.push_back(std::move(result));
    }

    if (policy.continue_after_hit && !stop_remaining) {
        ProbeState::OnHitActionResult result;
        result.index = static_cast<int>(results.size() + 1);
        result.action_name = "continue_after_hit";
        result.failure_policy = policy.failure_policy;
        ActionRequest action_request;
        action_request.kind = ActionKind::Continue;
        action_request.action = "continue";
        action_request.payload = ContinuePayload{policy.timeout_ms, true};
        size_t evidence_start = session.evidence_store().all().size();
        ActionOutput action_output = handle_action_request(session, task, outcome, probe_state, action_request);
        std::string response_text = action_output_text(action_output);
        prelude.insert(prelude.end(), action_output.prelude.begin(), action_output.prelude.end());
        prelude.push_back(action_output.final);
        const ActionResult &action_result = action_output.final;
        result.action_evidence_ids = evidence_ids_since(session, evidence_start);
        if (!action_result.ok) {
            result.status = "failed";
            result.error = action_result.error.empty() ? "continue_after_hit returned ok:false" : action_result.error;
            result.error_evidence_id = action_result.evidence_id;
            if (result.error_evidence_id.empty() && !result.action_evidence_ids.empty()) {
                result.error_evidence_id = result.action_evidence_ids.back();
            }
        } else {
            result.status = "success";
        }
        result = add_on_hit_wrapper_evidence(session, hit, policy, std::move(result), action_request, response_text);
        results.push_back(std::move(result));
    }

    return results;
}

static void record_probe_hit(GdbSession &session,
                             const ProbeState::ProbeHitSnapshot &hit,
                             const std::vector<ProbeState::OnHitActionResult> &on_hit_results) {
    if (hit.number.empty() && hit.kind != "watchpoint") {
        return;
    }
    std::ostringstream text;
    text << "{\n";
    text << "  \"number\": " << json_escape(hit.number) << ",\n";
    text << "  \"kind\": " << json_escape(hit.kind) << ",\n";
    text << "  \"stop_reason\": " << json_escape(hit.stop_reason) << ",\n";
    text << "  \"signal\": " << json_escape(hit.signal_name) << ",\n";
    text << "  \"known_probe\": " << (hit.known_probe ? "true" : "false");
    if (hit.known_probe) {
        text << ",\n";
        text << "  \"location\": " << json_escape(hit.location) << ",\n";
        text << "  \"expression\": " << json_escape(hit.expression) << ",\n";
        text << "  \"event\": " << json_escape(hit.event) << ",\n";
        text << "  \"selector\": " << json_escape(hit.selector) << ",\n";
        text << "  \"condition\": " << json_escape(hit.condition) << ",\n";
        text << "  \"comment\": " << json_escape(hit.comment) << ",\n";
        text << "  \"purpose\": " << json_escape(hit.purpose) << ",\n";
        text << "  \"hit_count\": " << hit.hit_count << ",\n";
        text << "  \"on_hit_policy\": " << on_hit_policy_json(hit.on_hit_policy) << ",\n";
        text << "  \"on_hit_results\": " << on_hit_action_results_json(on_hit_results) << ",\n";
        text << "  \"on_hit_evidence_ids\": " << json_string_vector(on_hit_result_evidence_ids(on_hit_results)) << ",\n";
        text << "  \"on_hit_error_ids\": " << json_string_vector(on_hit_result_error_ids(on_hit_results));
    } else if (hit.kind == "watchpoint") {
        text << ",\n";
        text << "  \"attribution\": \"watchpoint stop observed but no unique active probe could be associated\"";
    }
    text << "\n}\n";

    std::string evidence_kind = "BreakpointHit";
    if (hit.kind == "watchpoint") {
        evidence_kind = "WatchpointHit";
    } else if (hit.kind == "catchpoint") {
        evidence_kind = "CatchpointHit";
    }
    session.evidence_store().add_text(evidence_kind,
                                      hit.number.empty() ? evidence_kind + " unattributed" : evidence_kind + " " + hit.number,
                                      hit.stop_reason,
                                      text.str());
}

static std::vector<ActionResult> handle_probe_stop(GdbSession &session,
                                                   const DebugTask *task,
                                                   SessionOutcome *outcome,
                                                   ProbeState &probe_state,
                                                   const CommandResult &result) {
    std::vector<ActionResult> prelude;
    auto hit = prepare_probe_hit(probe_state, result);
    if (outcome != nullptr && !hit.number.empty()) {
        outcome->state = SessionState::Stopped;
        outcome->stop_reason = hit.stop_reason;
        outcome->signal_name = hit.signal_name;
    }
    auto on_hit_results = run_on_hit_actions(session, task, outcome, probe_state, hit, prelude);
    record_probe_hit(session, hit, on_hit_results);
    return prelude;
}

static std::string next_hypothesis_id(ProbeState &probe_state) {
    ++probe_state.hypothesis_counter;
    std::ostringstream out;
    out << 'H';
    out.width(4);
    out.fill('0');
    out << probe_state.hypothesis_counter;
    return out.str();
}

static fs::path hypothesis_file_for(GdbSession &session, const std::string &id) {
    fs::path dir = session.assets_dir() / "hypotheses";
    fs::create_directories(dir);
    return dir / (id + ".md");
}

static void write_hypothesis_index(GdbSession &session, const ProbeState &probe_state) {
    fs::path dir = session.assets_dir() / "hypotheses";
    fs::create_directories(dir);
    fs::path path = dir / "index.json";
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"gdb-agent-hypotheses-v1\",\n";
    out << "  \"hypotheses\": [\n";
    bool first_hypothesis = true;
    for (const auto &[_, hypothesis] : probe_state.hypotheses_by_id) {
        if (!first_hypothesis) {
            out << ",\n";
        }
        first_hypothesis = false;
        out << "    {\n";
        out << "      \"id\": " << json_escape(hypothesis.id) << ",\n";
        out << "      \"title\": " << json_escape(hypothesis.title) << ",\n";
        out << "      \"description\": " << json_escape(hypothesis.description) << ",\n";
        out << "      \"tool_status\": " << json_escape(hypothesis.tool_status) << ",\n";
        out << "      \"agent_conclusion\": " << json_escape(hypothesis.agent_conclusion) << ",\n";
        out << "      \"agent_inference\": " << json_escape(hypothesis.agent_inference) << ",\n";
        out << "      \"checks\": [\n";
        for (size_t i = 0; i < hypothesis.checks.size(); ++i) {
            const auto &check = hypothesis.checks[i];
            out << "        {\n";
            out << "          \"id\": " << json_escape(check.id) << ",\n";
            out << "          \"check_id\": " << json_escape(check.id) << ",\n";
            out << "          \"description\": " << json_escape(check.description) << ",\n";
            out << "          \"expression\": " << json_escape(check.expression) << ",\n";
            out << "          \"assertion\": " << json_escape(check.assertion) << ",\n";
            out << "          \"expected\": " << json_escape(check.expected) << ",\n";
            out << "          \"observed\": " << json_escape(check.observed) << ",\n";
            out << "          \"status\": " << json_escape(check.status) << ",\n";
            out << "          \"evidence\": " << json_escape(check.evidence_id) << ",\n";
            out << "          \"error_evidence\": "
                << (check.error_evidence_id.empty() ? std::string("null") : json_escape(check.error_evidence_id)) << "\n";
            out << "        }";
            if (i + 1 != hypothesis.checks.size()) {
                out << ",";
            }
            out << "\n";
        }
        out << "      ]\n";
        out << "    }";
    }
    out << "\n  ]\n";
    out << "}\n";
    write_text_file(path, out.str());
}

static std::vector<ActionRequest> load_replay_jsonl_actions(const fs::path &path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open replay file: " + path.string());
    }

    std::vector<ActionRequest> actions;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        Json action = parse_json(line);
        if (!action.is_object()) {
            throw std::runtime_error("replay JSONL action must be an object: " + path.string());
        }
        actions.push_back(parse_action_request(action));
    }
    return actions;
}

static void rebuild_replay_plan_from_jsonl(const fs::path &jsonl_file,
                                           const fs::path &plan_file,
                                           const std::string &name,
                                           const DebugTask *task,
                                           const std::string &source_session_id,
                                           const std::string &failure_policy) {
    write_replay_plan(plan_file,
                      name,
                      load_replay_jsonl_actions(jsonl_file),
                      task,
                      source_session_id,
                      failure_policy);
}

static void append_text(const fs::path &path, const std::string &text) {
    std::ofstream out(path, std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to append file: " + path.string());
    }
    out << text;
}

static ActionOutput single_output(ActionResult result) {
    ActionOutput output;
    output.final = std::move(result);
    return output;
}

static ActionResult make_action_error(GdbSession &session,
                                      ActionKind kind,
                                      const std::string &action_name,
                                      const std::string &message,
                                      const std::string &title = "Action validation failed",
                                      const std::map<std::string, std::string> &details = {}) {
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

static ResultObject probe_info_result_object(const ProbeState::ProbeInfo &probe) {
    ResultObject object;
    result_object_set_string(object, "number", probe.number);
    result_object_set_string(object, "kind", probe.kind);
    result_object_set_string(object, "event", probe.event);
    result_object_set_string(object, "selector", probe.selector);
    result_object_set_string(object, "location", probe.location);
    result_object_set_string(object, "expression", probe.expression);
    result_object_set_string(object, "condition", probe.condition);
    result_object_set_string(object, "comment", probe.comment);
    result_object_set_string(object, "purpose", probe.purpose);
    result_object_set_bool(object, "enabled", probe.enabled);
    result_object_set_bool(object, "deleted", probe.deleted);
    result_object_set_int(object, "hit_count", probe.hit_count);
    result_object_set_string(object, "last_stop_reason", probe.last_stop_reason);
    return object;
}

static ResultArray probe_array_result(const ProbeState &probe_state, bool include_deleted = false) {
    ResultArray array;
    for (const auto &[_, probe] : probe_state.probes_by_number) {
        if (probe.deleted && !include_deleted) {
            continue;
        }
        array.items.push_back(probe_info_result_object(probe));
    }
    return array;
}

static ActionOutput dispatch_action(GdbSession &session,
                                    const DebugTask *task,
                                    SessionOutcome *outcome,
                                    ProbeState &probe_state,
                                    const ActionRequest &request) {
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
            output.prelude = handle_probe_stop(session, task, outcome, probe_state, command_result);
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
            output.prelude = handle_probe_stop(session, task, outcome, probe_state, command_result);
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
            std::string id = payload.id.empty() ? next_hypothesis_id(probe_state) : payload.id;
            std::string title = payload.title.empty() ? "Untitled hypothesis" : payload.title;
            fs::path file = hypothesis_file_for(session, id);
            std::ostringstream md;
            md << "# " << id << " " << title << "\n\n";
            md << "Status: EvidenceCollectionStarted\n\n";
            if (!payload.description.empty()) {
                md << "## Description\n\n" << payload.description << "\n\n";
            }
            write_text_file(file, md.str());
            ProbeState::HypothesisRecord record;
            record.id = id;
            record.title = title;
            record.description = payload.description;
            probe_state.hypotheses_by_id[id] = std::move(record);
            write_hypothesis_index(session, probe_state);
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
            auto &record = probe_state.hypotheses_by_id[payload.hypothesis];
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
            append_text(file, md.str());
            ProbeState::HypothesisCheck check;
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
            write_hypothesis_index(session, probe_state);
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
            append_text(file, md.str());
            auto &record = probe_state.hypotheses_by_id[payload.hypothesis];
            if (record.id.empty()) {
                record.id = payload.hypothesis;
                record.title = payload.hypothesis;
            }
            record.agent_conclusion = payload.conclusion;
            record.agent_inference = payload.inference;
            write_hypothesis_index(session, probe_state);
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
                fs::path plan = replay_dir / (slugify(payload.name) + ".json");
                fs::path jsonl = replay_dir / (slugify(payload.name) + ".jsonl");
                replay_file = fs::exists(plan) ? plan : jsonl;
            } else {
                return single_output(make_action_error(session, request.kind, action_name, "replay requires file or name"));
            }
            try {
                return replay_action_file(session,
                                          task,
                                          outcome,
                                          probe_state,
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
        case ActionKind::Unknown:
            break;
    }
#undef RETURN_PROBE_SET_ERROR
    return single_output(make_action_error(session, request.kind, action_name, "unsupported action", "Unsupported action"));
}

static ActionOutput handle_action_request(GdbSession &session,
                                          const DebugTask *task,
                                          SessionOutcome *outcome,
                                          ProbeState &probe_state,
                                          const ActionRequest &request) {
    return dispatch_action(session, task, outcome, probe_state, request);
}

static ActionOutput handle_action_json(GdbSession &session,
                                       const DebugTask *task,
                                       SessionOutcome *outcome,
                                       ProbeState &probe_state,
                                       const Json &action) {
    if (!action.is_object()) {
        return single_output(make_action_error(session, ActionKind::Unknown, "", "action must be a JSON object"));
    }
    ActionRequest request = parse_action_request(action);
    if (request.action.empty()) {
        return single_output(make_action_error(session, ActionKind::Unknown, "", "missing action"));
    }
    return handle_action_request(session, task, outcome, probe_state, request);
}


struct ReplayStepRunResult {
    int index = 0;
    std::string step_id;
    std::string action_name;
    std::string status = "success";
    std::string failure_policy = kReplayPolicyContinue;
    std::string evidence_id;
    std::string error_evidence_id;
    std::string action_evidence_id;
    std::string error;
    std::string skip_reason;
};

struct ReplayRunResult {
    bool ok = true;
    bool force = false;
    bool task_metadata_match = true;
    std::string plan_name;
    std::string schema = kReplayPlanSchema;
    int schema_version = kReplayPlanSchemaVersion;
    std::string failure_policy = kReplayPolicyContinue;
    std::string warning;
    std::string warning_evidence_id;
    std::string error;
    std::string error_evidence_id;
    std::string run_evidence_id;
    std::vector<ReplayStepRunResult> steps;
};

struct ReplayActionStep {
    int index = 0;
    std::string step_id;
    std::string name;
    bool enabled = true;
    std::string failure_policy = kReplayPolicyContinue;
    ActionRequest action;
};

static std::string replay_step_json_fragment(const ReplayStepRunResult &step) {
    std::ostringstream out;
    out << "{"
        << "\"index\":" << step.index << ","
        << "\"step_id\":" << json_escape(step.step_id) << ","
        << "\"action_name\":" << json_escape(step.action_name) << ","
        << "\"status\":" << json_escape(step.status) << ","
        << "\"failure_policy\":" << json_escape(step.failure_policy) << ","
        << "\"evidence\":" << json_escape(step.evidence_id) << ","
        << "\"action_evidence\":" << json_escape(step.action_evidence_id) << ","
        << "\"error_evidence\":" << json_escape(step.error_evidence_id) << ","
        << "\"error\":" << json_escape(step.error) << ","
        << "\"skip_reason\":" << json_escape(step.skip_reason)
        << "}";
    return out.str();
}

static ResultObject replay_step_result_object(const ReplayStepRunResult &step) {
    ResultObject object;
    result_object_set_int(object, "index", step.index);
    result_object_set_string(object, "step_id", step.step_id);
    result_object_set_string(object, "action_name", step.action_name);
    result_object_set_string(object, "status", step.status);
    result_object_set_string(object, "failure_policy", step.failure_policy);
    result_object_set_string(object, "evidence", step.evidence_id);
    result_object_set_string(object, "action_evidence", step.action_evidence_id);
    result_object_set_string(object, "error_evidence", step.error_evidence_id);
    result_object_set_string(object, "error", step.error);
    result_object_set_string(object, "skip_reason", step.skip_reason);
    return object;
}

static ActionResult replay_result_action(const fs::path &path, const ReplayRunResult &run) {
    ActionResult result = run.ok
                              ? ActionResult::success(ActionKind::Replay, "replay")
                              : ActionResult::failure(ActionKind::Replay, "replay", run.error);
    action_result_set_string(result, "file", path.lexically_normal().string());
    action_result_set_string(result, "plan", run.plan_name);
    action_result_set_string(result, "schema", run.schema);
    action_result_set_int(result, "schema_version", run.schema_version);
    action_result_set_bool(result, "force", run.force);
    action_result_set_bool(result, "task_metadata_match", run.task_metadata_match);
    action_result_set_string(result, "failure_policy", run.failure_policy);
    action_result_set_string(result, "warning", run.warning);
    action_result_set_string(result, "warning_evidence", run.warning_evidence_id);
    action_result_set_string(result, "error_evidence", run.error_evidence_id);
    action_result_set_string(result, "run_evidence", run.run_evidence_id);
    ResultArray steps;
    for (const auto &step : run.steps) {
        steps.items.push_back(replay_step_result_object(step));
    }
    action_result_set_array(result, "steps", std::move(steps));
    return result;
}

static std::string replay_result_json(const fs::path &path, const ReplayRunResult &result) {
    std::ostringstream out;
    out << "{\"ok\":" << (result.ok ? "true" : "false")
        << ",\"action\":\"replay\""
        << ",\"file\":" << json_escape(path.lexically_normal().string())
        << ",\"plan\":" << json_escape(result.plan_name)
        << ",\"schema\":" << json_escape(result.schema)
        << ",\"schema_version\":" << result.schema_version
        << ",\"force\":" << (result.force ? "true" : "false")
        << ",\"task_metadata_match\":" << (result.task_metadata_match ? "true" : "false")
        << ",\"failure_policy\":" << json_escape(result.failure_policy)
        << ",\"warning\":" << json_escape(result.warning)
        << ",\"warning_evidence\":" << json_escape(result.warning_evidence_id)
        << ",\"error\":" << json_escape(result.error)
        << ",\"error_evidence\":" << json_escape(result.error_evidence_id)
        << ",\"run_evidence\":" << json_escape(result.run_evidence_id)
        << ",\"steps\":[";
    for (size_t i = 0; i < result.steps.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << replay_step_json_fragment(result.steps[i]);
    }
    out << "]}\n";
    return out.str();
}

static void add_replay_run_evidence(GdbSession &session,
                                    const fs::path &path,
                                    ReplayRunResult &result) {
    std::ostringstream evidence_text;
    evidence_text << "{\n";
    evidence_text << "  \"file\": " << json_escape(path.lexically_normal().string()) << ",\n";
    evidence_text << "  \"result\": " << replay_result_json(path, result);
    evidence_text << "}\n";
    auto ev = session.evidence_store().add_text("ReplayRun", "Replay plan " + result.plan_name, result.plan_name, evidence_text.str());
    result.run_evidence_id = ev.id;
}

static ReplayStepRunResult write_skipped_replay_step(GdbSession &session,
                                                     const std::string &plan_name,
                                                     const std::string &step_id,
                                                     int index,
                                                     const std::string &action_name,
                                                     const std::string &failure_policy,
                                                     const std::string &skip_reason) {
    ReplayStepRunResult result;
    result.index = index;
    result.step_id = step_id;
    result.action_name = action_name;
    result.status = "skipped";
    result.failure_policy = failure_policy;
    result.skip_reason = skip_reason;
    std::ostringstream evidence_text;
    evidence_text << "{\n";
    evidence_text << "  \"plan\": " << json_escape(plan_name) << ",\n";
    evidence_text << "  \"step_id\": " << json_escape(step_id) << ",\n";
    evidence_text << "  \"index\": " << index << ",\n";
    evidence_text << "  \"action_name\": " << json_escape(action_name) << ",\n";
    evidence_text << "  \"status\": \"skipped\",\n";
    evidence_text << "  \"failure_policy\": " << json_escape(failure_policy) << ",\n";
    evidence_text << "  \"skip_reason\": " << json_escape(skip_reason) << "\n";
    evidence_text << "}\n";
    auto ev = session.evidence_store().add_text("ReplayStep", "Replay step " + step_id + " skipped", action_name, evidence_text.str());
    result.evidence_id = ev.id;
    return result;
}

static ReplayStepRunResult replay_action_step(GdbSession &session,
                                              const DebugTask *task,
                                              SessionOutcome *outcome,
                                              ProbeState &probe_state,
                                              const std::string &plan_name,
                                              const ReplayActionStep &step,
                                              std::vector<ActionResult> &prelude) {
    ReplayStepRunResult result;
    result.index = step.index;
    result.step_id = step.step_id;
    result.failure_policy = step.failure_policy;
    std::string action_name = step.action.action.empty() ? step.name : step.action.action;
    if (action_name.empty()) {
        action_name = "unknown";
    }
    result.action_name = action_name;

    try {
        ActionOutput action_output = handle_action_request(session, task, outcome, probe_state, step.action);
        std::string step_output = action_output_text(action_output);
        prelude.insert(prelude.end(), action_output.prelude.begin(), action_output.prelude.end());
        prelude.push_back(action_output.final);
        const ActionResult &action_result = action_output.final;
        bool failed = !action_result.ok;
        result.status = failed ? "failed" : "success";
        result.error = failed ? action_result.error : "";
        result.action_evidence_id = action_result.evidence_id.empty()
                                      ? action_result.command_evidence_id
                                      : action_result.evidence_id;
        if (failed && result.error_evidence_id.empty()) {
            std::ostringstream error_text;
            error_text << "{\n";
            error_text << "  \"plan\": " << json_escape(plan_name) << ",\n";
            error_text << "  \"step_id\": " << json_escape(step.step_id) << ",\n";
            error_text << "  \"index\": " << step.index << ",\n";
            error_text << "  \"action_name\": " << json_escape(action_name) << ",\n";
            error_text << "  \"failure_policy\": " << json_escape(step.failure_policy) << ",\n";
            error_text << "  \"error\": " << json_escape(result.error) << ",\n";
            error_text << "  \"response\": " << json_escape(step_output) << "\n";
            error_text << "}\n";
            auto ev = session.evidence_store().add_text("ToolError", "Replay step reported failure " + step.step_id, action_name, error_text.str());
            result.error_evidence_id = ev.id;
        }
        std::ostringstream evidence_text;
        evidence_text << "{\n";
        evidence_text << "  \"plan\": " << json_escape(plan_name) << ",\n";
        evidence_text << "  \"step_id\": " << json_escape(step.step_id) << ",\n";
        evidence_text << "  \"index\": " << step.index << ",\n";
        evidence_text << "  \"action_name\": " << json_escape(action_name) << ",\n";
        evidence_text << "  \"status\": " << json_escape(result.status) << ",\n";
        evidence_text << "  \"failure_policy\": " << json_escape(step.failure_policy) << ",\n";
        evidence_text << "  \"action_json\": " << json_escape(dump_json(action_request_to_json(step.action))) << ",\n";
        evidence_text << "  \"action_evidence\": " << json_escape(result.action_evidence_id) << ",\n";
        evidence_text << "  \"error_evidence\": " << json_escape(result.error_evidence_id) << ",\n";
        evidence_text << "  \"error\": " << json_escape(result.error) << ",\n";
        evidence_text << "  \"response\": " << json_escape(step_output) << "\n";
        evidence_text << "}\n";
        auto ev = session.evidence_store().add_text("ReplayStep", "Replay step " + step.step_id + " " + result.status, action_name, evidence_text.str());
        result.evidence_id = ev.id;
    } catch (const std::exception &ex) {
        result.status = "failed";
        result.error = ex.what();
        std::ostringstream evidence_text;
        evidence_text << "{\n";
        evidence_text << "  \"plan\": " << json_escape(plan_name) << ",\n";
        evidence_text << "  \"step_id\": " << json_escape(step.step_id) << ",\n";
        evidence_text << "  \"index\": " << step.index << ",\n";
        evidence_text << "  \"action_name\": " << json_escape(action_name) << ",\n";
        evidence_text << "  \"status\": \"failed\",\n";
        evidence_text << "  \"failure_policy\": " << json_escape(step.failure_policy) << ",\n";
        evidence_text << "  \"action\": " << json_escape(dump_json(action_request_to_json(step.action))) << ",\n";
        evidence_text << "  \"error\": " << json_escape(ex.what()) << "\n";
        evidence_text << "}\n";
        auto ev = session.evidence_store().add_text("ToolError", "Replay step failed " + step.step_id, action_name, evidence_text.str());
        result.evidence_id = ev.id;
        result.error_evidence_id = ev.id;
        ActionResult emitted = ActionResult::failure(ActionKind::Replay, "replay_step", ex.what());
        emitted.evidence_id = ev.id;
        action_result_set_string(emitted, "step_id", step.step_id);
        prelude.push_back(std::move(emitted));
    }
    return result;
}

static ReplayRunResult replay_json_plan(GdbSession &session,
                                        const DebugTask *task,
                                        SessionOutcome *outcome,
                                        ProbeState &probe_state,
                                        const fs::path &path,
                                        const Json &plan,
                                        std::vector<ActionResult> &prelude,
                                        bool force,
                                        const std::string &failure_policy_override) {
    ReplayRunResult result;
    std::string plan_name = plan.string_or("name", path.stem().string());
    result.plan_name = plan_name;
    result.force = force;
    result.schema = plan.string_or("schema", kReplayPlanSchema);
    result.schema_version = plan.int_or("schema_version", kReplayPlanSchemaVersion);
    const Json *actions = plan.find("actions");
    if (actions == nullptr || !actions->is_array()) {
        throw std::runtime_error("replay plan missing actions array: " + path.string());
    }

    ReplayPlanValidation validation = validate_replay_plan(plan, task, force);
    result.schema = validation.schema.empty() ? kReplayPlanSchema : validation.schema;
    result.schema_version = validation.schema_version;
    result.warning = validation.warning;
    result.task_metadata_match = validation.task_metadata_match;
    if (!validation.ok) {
        result.ok = false;
        result.error = validation.error;
        std::ostringstream evidence_text;
        evidence_text << "{\n";
        evidence_text << "  \"plan\": " << json_escape(plan_name) << ",\n";
        evidence_text << "  \"file\": " << json_escape(path.lexically_normal().string()) << ",\n";
        evidence_text << "  \"force\": " << (force ? "true" : "false") << ",\n";
        evidence_text << "  \"task_metadata_match\": " << (validation.task_metadata_match ? "true" : "false") << ",\n";
        evidence_text << "  \"plan_fingerprint\": " << json_escape(validation.plan_fingerprint) << ",\n";
        evidence_text << "  \"current_fingerprint\": " << json_escape(validation.current_fingerprint) << ",\n";
        evidence_text << "  \"error\": " << json_escape(validation.error) << "\n";
        evidence_text << "}\n";
        auto ev = session.evidence_store().add_text("ToolError", "Replay plan rejected", plan_name, evidence_text.str());
        result.error_evidence_id = ev.id;
        return result;
    }
    if (!validation.warning.empty()) {
        std::ostringstream evidence_text;
        evidence_text << "{\n";
        evidence_text << "  \"plan\": " << json_escape(plan_name) << ",\n";
        evidence_text << "  \"file\": " << json_escape(path.lexically_normal().string()) << ",\n";
        evidence_text << "  \"force\": " << (force ? "true" : "false") << ",\n";
        evidence_text << "  \"task_metadata_match\": " << (validation.task_metadata_match ? "true" : "false") << ",\n";
        evidence_text << "  \"plan_fingerprint\": " << json_escape(validation.plan_fingerprint) << ",\n";
        evidence_text << "  \"current_fingerprint\": " << json_escape(validation.current_fingerprint) << ",\n";
        evidence_text << "  \"warning\": " << json_escape(validation.warning) << "\n";
        evidence_text << "}\n";
        auto ev = session.evidence_store().add_text("ReplayWarning", "Replay plan warning", plan_name, evidence_text.str());
        result.warning_evidence_id = ev.id;
    }

    std::string plan_policy = normalize_replay_failure_policy(failure_policy_override,
                                                              plan.string_or("failure_policy", kReplayPolicyContinue));
    result.failure_policy = plan_policy;
    bool stop_due_to_failure = false;
    std::string stop_reason;
    int index = 0;
    for (const auto &step : actions->array_value) {
        ++index;
        if (!step.is_object()) {
            std::ostringstream step_id;
            step_id << 'a' << index;
            result.steps.push_back(write_skipped_replay_step(session,
                                                             plan_name,
                                                             step_id.str(),
                                                             index,
                                                             "unknown",
                                                             plan_policy,
                                                             "step is not an object"));
            continue;
        }
        std::string step_id = step.string_or("id");
        if (step_id.empty()) {
            std::ostringstream fallback;
            fallback << 'a' << index;
            step_id = fallback.str();
        }
        std::string action_name = step.string_or("name", "unknown");
        std::string step_policy = normalize_replay_failure_policy(step.string_or("failure_policy"), plan_policy);
        if (stop_due_to_failure) {
            result.steps.push_back(write_skipped_replay_step(session,
                                                             plan_name,
                                                             step_id,
                                                             index,
                                                             action_name,
                                                             step_policy,
                                                             stop_reason));
            continue;
        }
        if (!step.bool_or("enabled", true)) {
            result.steps.push_back(write_skipped_replay_step(session,
                                                             plan_name,
                                                             step_id,
                                                             index,
                                                             action_name,
                                                             step_policy,
                                                             "step disabled"));
            continue;
        }
        const Json *action = step.find("action");
        if (action == nullptr || !action->is_object()) {
            ReplayStepRunResult step_result;
            step_result.index = index;
            step_result.step_id = step_id;
            step_result.action_name = action_name;
            step_result.status = "failed";
            step_result.failure_policy = step_policy;
            step_result.error = "missing action object";
            std::ostringstream evidence_text;
            evidence_text << "{\n";
            evidence_text << "  \"plan\": " << json_escape(plan_name) << ",\n";
            evidence_text << "  \"step_id\": " << json_escape(step_id) << ",\n";
            evidence_text << "  \"index\": " << index << ",\n";
            evidence_text << "  \"action_name\": " << json_escape(action_name) << ",\n";
            evidence_text << "  \"status\": \"failed\",\n";
            evidence_text << "  \"failure_policy\": " << json_escape(step_policy) << ",\n";
            evidence_text << "  \"error\": \"missing action object\"\n";
            evidence_text << "}\n";
            auto ev = session.evidence_store().add_text("ToolError", "Replay step missing action " + step_id, plan_name, evidence_text.str());
            step_result.evidence_id = ev.id;
            step_result.error_evidence_id = ev.id;
            result.steps.push_back(step_result);
            result.ok = false;
            if (step_policy == kReplayPolicyStop) {
                stop_due_to_failure = true;
                stop_reason = "previous step " + step_id + " failed under stop_on_error";
            }
            continue;
        }
        ReplayActionStep replay_step;
        replay_step.index = index;
        replay_step.step_id = step_id;
        replay_step.name = action_name;
        replay_step.failure_policy = step_policy;
        replay_step.action = parse_action_request(*action);
        ReplayStepRunResult step_result = replay_action_step(session,
                                                             task,
                                                             outcome,
                                                             probe_state,
                                                             plan_name,
                                                             replay_step,
                                                             prelude);
        if (step_result.status == "failed") {
            result.ok = false;
            if (step_policy == kReplayPolicyStop) {
                stop_due_to_failure = true;
                stop_reason = "previous step " + step_id + " failed under stop_on_error";
            }
        }
        result.steps.push_back(std::move(step_result));
    }
    return result;
}

static ActionOutput replay_action_file(GdbSession &session,
                                       const DebugTask *task,
                                       SessionOutcome *outcome,
                                       ProbeState &probe_state,
                                       const fs::path &path,
                                       bool force,
                                       const std::string &failure_policy_override) {
    ActionOutput output;
    ReplayRunResult result;
    result.plan_name = path.stem().string();
    result.force = force;
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open replay file: " + path.string());
    }

    std::ostringstream content_stream;
    content_stream << in.rdbuf();
    std::string content = trim(content_stream.str());
    if (content.empty()) {
        add_replay_run_evidence(session, path, result);
        output.final = replay_result_action(path, result);
        return output;
    }

    if (!content.empty() && content.front() == '{') {
        Json plan = parse_json(content);
        const Json *actions = plan.find("actions");
        if (actions != nullptr && actions->is_array()) {
            result = replay_json_plan(session,
                                      task,
                                      outcome,
                                      probe_state,
                                      path,
                                      plan,
                                      output.prelude,
                                      force,
                                      failure_policy_override);
        } else {
            std::string policy = normalize_replay_failure_policy(failure_policy_override);
            result.failure_policy = policy;
            ReplayActionStep step;
            step.index = 1;
            step.step_id = "a1";
            step.name = replay_action_display_name(plan, 1);
            step.failure_policy = policy;
            step.action = parse_action_request(plan);
            result.steps.push_back(replay_action_step(session,
                                                      task,
                                                      outcome,
                                                      probe_state,
                                                      path.stem().string(),
                                                      step,
                                                      output.prelude));
            result.ok = result.steps.back().status != "failed";
        }
        add_replay_run_evidence(session, path, result);
        output.final = replay_result_action(path, result);
        return output;
    }

    std::istringstream lines(content);
    std::string line;
    int index = 0;
    std::string plan_name = path.stem().string();
    std::string plan_policy = normalize_replay_failure_policy(failure_policy_override);
    result.plan_name = plan_name;
    result.failure_policy = plan_policy;
    bool stop_due_to_failure = false;
    std::string stop_reason;
    while (std::getline(lines, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        ++index;
        std::ostringstream step_id;
        step_id << 'a' << index;
        if (stop_due_to_failure) {
            result.steps.push_back(write_skipped_replay_step(session,
                                                             plan_name,
                                                             step_id.str(),
                                                             index,
                                                             "unknown",
                                                             plan_policy,
                                                             stop_reason));
            continue;
        }
        Json action = parse_json(line);
        if (!action.is_object()) {
            throw std::runtime_error("replay JSONL action must be an object: " + path.string());
        }
        ReplayActionStep step;
        step.index = index;
        step.step_id = step_id.str();
        step.name = replay_action_display_name(action, index);
        step.failure_policy = plan_policy;
        step.action = parse_action_request(action);
        ReplayStepRunResult step_result = replay_action_step(session,
                                                             task,
                                                             outcome,
                                                             probe_state,
                                                             plan_name,
                                                             step,
                                                             output.prelude);
        if (step_result.status == "failed") {
            result.ok = false;
            if (plan_policy == kReplayPolicyStop) {
                stop_due_to_failure = true;
                stop_reason = "previous step " + step_id.str() + " failed under stop_on_error";
            }
        }
        result.steps.push_back(std::move(step_result));
    }
    add_replay_run_evidence(session, path, result);
    output.final = replay_result_action(path, result);
    return output;
}

static int run_check(const DebugTask &task) {
    validate_task(task);
    std::cout << "ok\n";
    std::cout << "executable: " << shell_quote_for_report(task.executable.string()) << "\n";
    std::cout << "working directory: " << shell_quote_for_report(task.working_directory.string()) << "\n";
    std::cout << "args: " << task.args_raw << "\n";
    std::cout << "argv:";
    for (const auto &arg : task.args) {
        std::cout << " " << shell_quote_for_report(arg);
    }
    std::cout << "\n";
    std::cout << "stdin: " << shell_quote_for_report(task.stdin_path.string()) << "\n";
    std::cout << "run timeout ms: " << task.run_timeout_ms << "\n";
    if (!task.env.empty()) {
        std::cout << "env:\n";
        for (const auto &[key, value] : task.env) {
            std::cout << "  " << key << "=" << shell_quote_for_report(value) << "\n";
        }
    }
    if (task.core_dump) {
        std::cout << "core dump: " << shell_quote_for_report(task.core_dump->string()) << "\n";
    }
    return 0;
}

static std::string json_escape(const std::string &s) {
    Json json;
    json.type = Json::Type::String;
    json.string_value = s;
    return dump_json(json);
}

static std::string json_string_array(const std::vector<std::string> &items) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << json_escape(items[i]);
    }
    out << "]";
    return out.str();
}

static std::string json_string_map(const std::map<std::string, std::string> &items) {
    std::ostringstream out;
    out << "{";
    bool first = true;
    for (const auto &[key, value] : items) {
        if (!first) {
            out << ",";
        }
        first = false;
        out << json_escape(key) << ":" << json_escape(value);
    }
    out << "}";
    return out.str();
}

static std::string json_evidence_array(const std::vector<Evidence> &evidence) {
    std::ostringstream out;
    out << "[\n";
    for (size_t i = 0; i < evidence.size(); ++i) {
        const auto &ev = evidence[i];
        out << "    {\n";
        out << "      \"id\": " << json_escape(ev.id) << ",\n";
        out << "      \"kind\": " << json_escape(ev.kind) << ",\n";
        out << "      \"title\": " << json_escape(ev.title) << ",\n";
        out << "      \"command\": " << json_escape(ev.command) << ",\n";
        out << "      \"view_file\": " << json_escape(ev.view_file.string()) << ",\n";
        out << "      \"summary_file\": " << json_escape(ev.summary_file.string()) << ",\n";
        out << "      \"raw_file\": " << json_escape(ev.raw_file.string()) << ",\n";
        out << "      \"raw_sha256\": " << json_escape(ev.raw_sha256) << ",\n";
        out << "      \"captured_at\": " << json_escape(ev.captured_at) << ",\n";
        out << "      \"raw_bytes\": " << ev.raw_bytes << ",\n";
        out << "      \"kept_bytes\": " << ev.kept_bytes << ",\n";
        out << "      \"truncated\": " << (ev.truncated ? "true" : "false") << ",\n";
        out << "      \"lossy_summary\": " << (ev.lossy_summary ? "true" : "false") << ",\n";
        out << "      \"included_records\": " << ev.included_records.size() << ",\n";
        out << "      \"related_records\": " << ev.related_records.size() << ",\n";
        out << "      \"concurrent_records\": " << ev.concurrent_records.size() << ",\n";
        out << "      \"raw_record_count\": " << ev.raw_records.size() << "\n";
        out << "    }";
        if (i + 1 != evidence.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]";
    return out.str();
}

static void write_session_files(const CliOptions &opts,
                                const DebugTask &task,
                                const SessionOutcome &outcome,
                                const GdbSession &session) {
    fs::create_directories(opts.assets);

    std::ostringstream task_json;
    task_json << "{\n";
    task_json << "  \"problem\": " << json_escape(task.problem) << ",\n";
    task_json << "  \"executable\": " << json_escape(task.executable.string()) << ",\n";
    task_json << "  \"working_directory\": " << json_escape(task.working_directory.string()) << ",\n";
    task_json << "  \"args\": " << json_escape(task.args_raw) << ",\n";
    task_json << "  \"argv\": " << json_string_array(task.args) << ",\n";
    task_json << "  \"stdin\": " << json_escape(task.stdin_path.string()) << ",\n";
    task_json << "  \"stdout\": " << json_escape(outcome.inferior_stdout) << ",\n";
    task_json << "  \"stderr\": " << json_escape(outcome.inferior_stderr) << ",\n";
    task_json << "  \"env\": " << json_string_map(task.env) << ",\n";
    task_json << "  \"run_timeout_ms\": " << effective_run_timeout_ms(opts, task) << ",\n";
    task_json << "  \"core_dump\": "
              << (task.core_dump ? json_escape(task.core_dump->string()) : std::string("null")) << "\n";
    task_json << "}\n";
    write_text_file(opts.assets / "task.normalized.json", task_json.str());

    std::ostringstream summary;
    int replay_step_count = 0;
    int replay_warning_count = 0;
    int probe_hit_count = 0;
    int on_hit_action_count = 0;
    int on_hit_error_count = 0;
    for (const auto &ev : session.evidence_store().all()) {
        if (ev.kind == "ReplayStep") {
            ++replay_step_count;
        } else if (ev.kind == "ReplayWarning") {
            ++replay_warning_count;
        } else if (ev.kind == "BreakpointHit" || ev.kind == "WatchpointHit" || ev.kind == "CatchpointHit") {
            ++probe_hit_count;
        } else if (ev.kind == "OnHitAction") {
            ++on_hit_action_count;
            if (ev.summary.find("\"status\": \"failed\"") != std::string::npos ||
                ev.summary.find("\"status\":\"failed\"") != std::string::npos) {
                ++on_hit_error_count;
            }
        }
    }
    summary << "{\n";
    summary << "  \"session_id\": " << json_escape(opts.session_id) << ",\n";
    summary << "  \"state\": " << json_escape(std::string(session_state_name(outcome.state))) << ",\n";
    summary << "  \"mode\": " << json_escape(outcome.core_mode ? "core" : "run") << ",\n";
    summary << "  \"stop_reason\": " << json_escape(outcome.stop_reason) << ",\n";
    summary << "  \"signal\": " << json_escape(outcome.signal_name) << ",\n";
    summary << "  \"segfault\": " << (outcome.segfault ? "true" : "false") << ",\n";
    summary << "  \"run_timed_out\": " << (outcome.run_timed_out ? "true" : "false") << ",\n";
    summary << "  \"run_timeout_ms\": " << outcome.run_timeout_ms << ",\n";
    summary << "  \"core_dump\": "
            << (task.core_dump ? json_escape(task.core_dump->string()) : std::string("null")) << ",\n";
    summary << "  \"core_loaded\": " << (outcome.core_mode && outcome.stop_reason == "core_loaded" ? "true" : "false") << ",\n";
    summary << "  \"stdin\": " << json_escape(task.stdin_path.string()) << ",\n";
    summary << "  \"stdout\": " << json_escape(outcome.inferior_stdout) << ",\n";
    summary << "  \"stderr\": " << json_escape(outcome.inferior_stderr) << ",\n";
    summary << "  \"evidence_count\": " << session.evidence_store().all().size() << ",\n";
    summary << "  \"replay_step_count\": " << replay_step_count << ",\n";
    summary << "  \"replay_warning_count\": " << replay_warning_count << ",\n";
    summary << "  \"probe_hit_count\": " << probe_hit_count << ",\n";
    summary << "  \"on_hit_action_count\": " << on_hit_action_count << ",\n";
    summary << "  \"on_hit_error_count\": " << on_hit_error_count << "\n";
    summary << "}\n";
    write_text_file(opts.assets / "session_summary.json", summary.str());

    std::ostringstream snapshot;
    snapshot << "{\n";
    snapshot << "  \"schema\": \"gdb-agent-session-snapshot-v1\",\n";
    snapshot << "  \"restorable\": false,\n";
    snapshot << "  \"note\": \"Historical snapshot only; it cannot restore a live GDB process.\",\n";
    snapshot << "  \"session_id\": " << json_escape(opts.session_id) << ",\n";
    snapshot << "  \"state\": " << json_escape(std::string(session_state_name(outcome.state))) << ",\n";
    snapshot << "  \"mode\": " << json_escape(outcome.core_mode ? "core" : "run") << ",\n";
    snapshot << "  \"task\": {\n";
    snapshot << "    \"problem\": " << json_escape(task.problem) << ",\n";
    snapshot << "    \"executable\": " << json_escape(task.executable.string()) << ",\n";
    snapshot << "    \"working_directory\": " << json_escape(task.working_directory.string()) << ",\n";
    snapshot << "    \"args\": " << json_escape(task.args_raw) << ",\n";
    snapshot << "    \"argv\": " << json_string_array(task.args) << ",\n";
    snapshot << "    \"core_dump\": "
             << (task.core_dump ? json_escape(task.core_dump->string()) : std::string("null")) << "\n";
    snapshot << "  },\n";
    snapshot << "  \"io\": {\n";
    snapshot << "    \"stdin\": " << json_escape(task.stdin_path.string()) << ",\n";
    snapshot << "    \"stdout\": " << json_escape(outcome.inferior_stdout) << ",\n";
    snapshot << "    \"stderr\": " << json_escape(outcome.inferior_stderr) << ",\n";
    snapshot << "    \"interactive_stdin\": false\n";
    snapshot << "  },\n";
    fs::path probes_file = opts.assets / "probes.json";
    snapshot << "  \"probe_store\": "
             << (fs::exists(probes_file) ? json_escape(probes_file.lexically_normal().string()) : std::string("null"))
             << ",\n";
    fs::path hypothesis_index = opts.assets / "hypotheses" / "index.json";
    snapshot << "  \"hypotheses\": "
             << (fs::exists(hypothesis_index) ? json_escape(hypothesis_index.lexically_normal().string()) : std::string("null"))
             << ",\n";
    snapshot << "  \"environment\": " << json_string_map(task.env) << ",\n";
    fs::path replay_dir = opts.assets / "replay";
    std::vector<std::string> replay_plans;
    if (fs::exists(replay_dir)) {
        for (const auto &entry : fs::directory_iterator(replay_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                replay_plans.push_back(entry.path().lexically_normal().string());
            }
        }
    }
    snapshot << "  \"replay_plans\": " << json_string_array(replay_plans) << ",\n";
    snapshot << "  \"outcome\": {\n";
    snapshot << "    \"stop_reason\": " << json_escape(outcome.stop_reason) << ",\n";
    snapshot << "    \"signal\": " << json_escape(outcome.signal_name) << ",\n";
    snapshot << "    \"segfault\": " << (outcome.segfault ? "true" : "false") << ",\n";
    snapshot << "    \"run_timed_out\": " << (outcome.run_timed_out ? "true" : "false") << ",\n";
    snapshot << "    \"run_timeout_ms\": " << outcome.run_timeout_ms << "\n";
    snapshot << "  },\n";
    snapshot << "  \"agent_inference\": " << json_escape(outcome.agent_inference) << ",\n";
    snapshot << "  \"final_agent_conclusion\": " << json_escape(outcome.final_agent_conclusion) << ",\n";
    snapshot << "  \"evidence\": " << json_evidence_array(session.evidence_store().all()) << "\n";
    snapshot << "}\n";
    write_text_file(opts.assets / "session_snapshot.json", snapshot.str());
}

static std::string option_value(int argc, char **argv, const std::string &name, const std::string &fallback = "") {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) {
            return argv[i + 1];
        }
    }
    return fallback;
}

static bool looks_like_inline_json(const std::string &arg) {
    std::string trimmed = trim(arg);
    return !trimmed.empty() && (trimmed.front() == '{' || trimmed.front() == '[');
}

static std::string read_text_file_arg(const std::string &arg) {
    if (looks_like_inline_json(arg)) {
        return arg;
    }
    fs::path path(arg);
    bool exists = false;
    try {
        exists = fs::exists(path);
    } catch (const fs::filesystem_error &) {
        return arg;
    }
    if (!exists) {
        return arg;
    }
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to read file: " + path.string());
    }
    std::ostringstream out;
    out << in.rdbuf();
    return trim(out.str());
}

static int unix_listen(const fs::path &socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket failed");
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::string path = socket_path.string();
    if (path.size() >= sizeof(addr.sun_path)) {
        close(fd);
        throw std::runtime_error("socket path is too long");
    }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    unlink(path.c_str());

    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        throw std::runtime_error("bind failed: " + path);
    }
    if (listen(fd, 16) != 0) {
        close(fd);
        throw std::runtime_error("listen failed");
    }
    return fd;
}

static void set_close_on_exec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

static int unix_connect(const fs::path &socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket failed");
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::string path = socket_path.string();
    if (path.size() >= sizeof(addr.sun_path)) {
        close(fd);
        throw std::runtime_error("socket path is too long");
    }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        throw std::runtime_error("connect failed: " + path);
    }
    return fd;
}

static void write_fd_all(int fd, const std::string &data) {
    const char *ptr = data.data();
    size_t left = data.size();
    while (left > 0) {
        ssize_t n = write(fd, ptr, left);
        if (n < 0) {
            throw std::runtime_error("socket write failed");
        }
        ptr += n;
        left -= static_cast<size_t>(n);
    }
}

static std::string read_fd_all(int fd) {
    std::string out;
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            throw std::runtime_error("socket read failed");
        }
        if (n == 0) {
            break;
        }
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

static std::string read_fd_line(int fd) {
    std::string out;
    char c = '\0';
    while (true) {
        ssize_t n = read(fd, &c, 1);
        if (n < 0) {
            throw std::runtime_error("socket read failed");
        }
        if (n == 0) {
            break;
        }
        if (c == '\n') {
            break;
        }
        out.push_back(c);
    }
    return out;
}

static std::string client_request(const fs::path &socket_path, const std::string &request) {
    int fd = unix_connect(socket_path);
    write_fd_all(fd, request + "\n");
    shutdown(fd, SHUT_WR);
    std::string response = read_fd_all(fd);
    close(fd);
    return response;
}

struct LiveSession {
    DebugTask task;
    CliOptions opts;
    std::unique_ptr<GdbSession> session;
    SessionOutcome outcome;
    ProbeState probe_state;
};

static void start_live_session(LiveSession &live) {
    validate_task(live.task);
    fs::create_directories(live.opts.assets);
    live.session = std::make_unique<GdbSession>(live.opts.assets, live.task.working_directory);
    live.session->set_session_id(live.opts.session_id);
    live.outcome.run_timeout_ms = effective_run_timeout_ms(live.opts, live.task);
    set_inferior_output_paths(live.outcome, live.opts.assets);
    live.outcome.state = SessionState::Starting;
    live.session->start();
    live.session->initialize(live.task);
    live.outcome.state = SessionState::Ready;
    collect_environment_info(*live.session, live.task, live.outcome);

    if (!live.opts.replay_before_run.empty()) {
        (void)replay_action_file(*live.session, &live.task, &live.outcome, live.probe_state, live.opts.replay_before_run);
    }

    if (live.task.core_dump) {
        live.outcome.core_mode = true;
        live.outcome.state = SessionState::Loading;
        auto load = live.session->load_core(live.task);
        live.session->evidence_store().add("SessionEvent", "Core load", load.command, load.raw_lines, false, load.record_sequences);
        live.outcome.stop_reason = "core_loaded";
        live.outcome.state = SessionState::Stopped;
        collect_core_evidence(*live.session);
    } else {
        live.outcome.state = SessionState::Running;
        live.outcome.inferior_stdout_offset = 0;
        live.outcome.inferior_stderr_offset = 0;
        auto run = run_inferior(*live.session,
                                live.task,
                                std::chrono::milliseconds(effective_run_timeout_ms(live.opts, live.task)));
        live.session->evidence_store().add("StopEvent", "Initial run stop", run.command, run.raw_lines, false, run.record_sequences);
        live.outcome.stop_reason = run.stop_reason.empty() ? "unknown" : run.stop_reason;
        live.outcome.signal_name = run.signal_name;
        live.outcome.segfault = run.signal_name == "SIGSEGV";
        live.outcome.run_timed_out = run.timed_out;
        update_outcome_from_stop(live.outcome, run);
        collect_stop_followup(*live.session, live.outcome, run);
        (void)handle_probe_stop(*live.session, &live.task, &live.outcome, live.probe_state, run);
    }
}

static std::string daemon_response(bool ok, const std::string &message) {
    std::ostringstream out;
    out << "{\"ok\":" << (ok ? "true" : "false") << ",\"message\":" << json_escape(message) << "}\n";
    return out.str();
}

static void shutdown_live_sessions(std::map<std::string, LiveSession> &sessions) {
    for (auto &[_, live] : sessions) {
        if (live.session) {
            live.session->shutdown();
        }
    }
    sessions.clear();
}

static std::string live_session_status_json(const std::string &session_id, const LiveSession &live) {
    std::ostringstream out;
    out << "{"
        << "\"session_id\":" << json_escape(session_id) << ","
        << "\"task\":" << json_escape(live.opts.task_file.string()) << ","
        << "\"report\":" << json_escape(live.opts.report.string()) << ","
        << "\"assets\":" << json_escape(live.opts.assets.string()) << ","
        << "\"mode\":" << json_escape(live.outcome.core_mode ? "core" : "run") << ","
        << "\"state\":" << json_escape(std::string(session_state_name(live.outcome.state))) << ","
        << "\"stop_reason\":" << json_escape(live.outcome.stop_reason) << ","
        << "\"signal\":" << json_escape(live.outcome.signal_name) << ","
        << "\"segfault\":" << (live.outcome.segfault ? "true" : "false") << ","
        << "\"run_timed_out\":" << (live.outcome.run_timed_out ? "true" : "false") << ","
        << "\"evidence_count\":" << (live.session ? live.session->evidence_store().all().size() : 0)
        << "}";
    return out.str();
}

static std::string list_daemon_sessions(const std::map<std::string, LiveSession> &sessions) {
    std::ostringstream out;
    out << "{\"ok\":true,\"sessions\":[";
    bool first = true;
    for (const auto &[session_id, live] : sessions) {
        if (!first) {
            out << ",";
        }
        first = false;
        out << live_session_status_json(session_id, live);
    }
    out << "]}\n";
    return out.str();
}

static std::string finish_live_session_response(const std::string &session_id,
                                                LiveSession &live,
                                                const std::string &report,
                                                const std::string &agent_inference = "",
                                                const std::string &final_conclusion = "") {
    if (!report.empty()) {
        live.opts.report = report;
    }
    if (!agent_inference.empty()) {
        live.outcome.agent_inference = agent_inference;
    }
    if (!final_conclusion.empty()) {
        live.outcome.final_agent_conclusion = final_conclusion;
    }
    flush_inferior_output(*live.session, live.outcome);
    write_probe_snapshot(*live.session, live.probe_state);
    write_session_files(live.opts, live.task, live.outcome, *live.session);
    write_report(live.opts.report, live.opts.assets, live.task, live.outcome, live.session->evidence_store().all());
    live.outcome.state = SessionState::Finishing;
    live.session->shutdown();
    live.outcome.state = SessionState::Closed;

    std::ostringstream out;
    out << "{\"ok\":true,\"session_id\":" << json_escape(session_id)
        << ",\"report\":" << json_escape(live.opts.report.string())
        << ",\"assets\":" << json_escape(live.opts.assets.string()) << "}\n";
    return out.str();
}

static std::string handle_daemon_request(const Json &request,
                                         std::map<std::string, LiveSession> &sessions,
                                         bool &shutdown_requested) {
    std::string op = request.string_or("op");
    std::string session_id = request.string_or("session_id", "S1");

    if (op == "list") {
        return list_daemon_sessions(sessions);
    }

    if (op == "shutdown") {
        shutdown_live_sessions(sessions);
        shutdown_requested = true;
        return daemon_response(true, "daemon shutting down");
    }

    if (op == "create") {
        if (sessions.contains(session_id)) {
            return daemon_response(false, "session already exists");
        }

        LiveSession live;
        live.opts.command = "serve";
        live.opts.session_id = session_id;
        live.opts.task_file = request.string_or("task");
        live.opts.assets = request.string_or("assets", "report.assets");
        live.opts.report = request.string_or("report", "report.md");
        live.opts.replay_before_run = request.string_or("replay_before_run");
        live.opts.run_timeout_ms = request.int_or("run_timeout_ms", -1);
        live.task = load_task(live.opts.task_file);
        start_live_session(live);
        std::ostringstream out;
        out << "{\"ok\":true,\"session_id\":" << json_escape(session_id)
            << ",\"mode\":" << json_escape(live.outcome.core_mode ? "core" : "run")
            << ",\"state\":" << json_escape(std::string(session_state_name(live.outcome.state)))
            << ",\"stop_reason\":" << json_escape(live.outcome.stop_reason)
            << ",\"signal\":" << json_escape(live.outcome.signal_name) << "}\n";
        sessions.emplace(session_id, std::move(live));
        return out.str();
    }

    auto it = sessions.find(session_id);
    if (it == sessions.end()) {
        return daemon_response(false, "session not found");
    }
    LiveSession &live = it->second;

    if (op == "status") {
        std::ostringstream out;
        out << "{\"ok\":true,\"session\":" << live_session_status_json(session_id, live) << "}\n";
        return out.str();
    }

    if (op == "action") {
        const Json *payload = request.find("payload");
        if (payload == nullptr || !payload->is_object()) {
            return daemon_response(false, "action request missing payload");
        }
        ActionOutput output = handle_action_json(*live.session, &live.task, &live.outcome, live.probe_state, *payload);
        if (output.final.finished) {
            std::string response = finish_live_session_response(session_id, live, "");
            sessions.erase(it);
            return response;
        }
        return action_output_text(output);
    }

    if (op == "finish") {
        std::string report = request.string_or("report");
        std::string inference = request.string_or("agent_inference");
        std::string conclusion = request.string_or("final_conclusion");
        if (conclusion.empty()) {
            conclusion = request.string_or("final_agent_conclusion");
        }
        std::string response = finish_live_session_response(session_id, live, report, inference, conclusion);
        sessions.erase(it);
        return response;
    }

    if (op == "close") {
        live.session->shutdown();
        sessions.erase(it);
        return daemon_response(true, "closed");
    }

    return daemon_response(false, "unsupported daemon op");
}

static int run_daemon(int argc, char **argv) {
    fs::path socket_path = option_value(argc, argv, "--socket", "/tmp/gdb-agent.sock");
    int server = unix_listen(socket_path);
    set_close_on_exec(server);
    std::map<std::string, LiveSession> sessions;
    std::cout << "{\"ok\":true,\"daemon\":\"listening\",\"socket\":" << json_escape(socket_path.string()) << "}\n";
    std::cout.flush();

    bool shutdown_requested = false;
    while (!shutdown_requested) {
        int client = accept(server, nullptr, nullptr);
        if (client < 0) {
            continue;
        }
        set_close_on_exec(client);
        std::string request_text = read_fd_line(client);
        std::string response;
        try {
            Json request = parse_json(request_text);
            response = handle_daemon_request(request, sessions, shutdown_requested);
        } catch (const std::exception &ex) {
            response = daemon_response(false, ex.what());
        }
        write_fd_all(client, response);
        close(client);
    }

    close(server);
    unlink(socket_path.string().c_str());
    shutdown_live_sessions(sessions);
    return 0;
}

static int run_client_command(int argc, char **argv) {
    std::string cmd = argv[1];
    fs::path socket_path = option_value(argc, argv, "--socket", "/tmp/gdb-agent.sock");
    std::ostringstream request;

    if (cmd == "create") {
        if (argc < 3) {
            throw std::runtime_error("usage: gdb-agent create task.md [--socket path] [--session id]");
        }
        fs::path task = fs::absolute(argv[2]);
        fs::path report = option_value(argc, argv, "--out", "report.md");
        fs::path assets = option_value(argc, argv, "--assets", default_assets_for(report).string());
        std::string session_id = option_value(argc, argv, "--session", "S1");
        std::string replay = option_value(argc, argv, "--replay-before-run", "");
        int timeout = std::stoi(option_value(argc, argv, "--run-timeout-ms", "-1"));
        request << "{"
                << "\"op\":\"create\","
                << "\"session_id\":" << json_escape(session_id) << ","
                << "\"task\":" << json_escape(task.string()) << ","
                << "\"report\":" << json_escape(report.string()) << ","
                << "\"assets\":" << json_escape(assets.string()) << ","
                << "\"replay_before_run\":" << json_escape(replay) << ","
                << "\"run_timeout_ms\":" << timeout
                << "}";
    } else if (cmd == "action") {
        if (argc < 4) {
            throw std::runtime_error("usage: gdb-agent action SESSION_ID JSON_OR_FILE [--socket path]");
        }
        std::string session_id = argv[2];
        Json payload = parse_json(read_text_file_arg(argv[3]));
        request << "{"
                << "\"op\":\"action\","
                << "\"session_id\":" << json_escape(session_id) << ","
                << "\"payload\":" << dump_json(payload)
                << "}";
    } else if (cmd == "save-action") {
        if (argc < 5) {
            throw std::runtime_error("usage: gdb-agent save-action SESSION_ID JSON_OR_FILE --name NAME [--socket path] [--failure-policy POLICY]");
        }
        std::string session_id = argv[2];
        Json saved_action = parse_json(read_text_file_arg(argv[3]));
        std::string name = option_value(argc, argv, "--name");
        std::string failure_policy = option_value(argc, argv, "--failure-policy", "");
        if (name.empty()) {
            throw std::runtime_error("save-action requires --name");
        }
        request << "{"
                << "\"op\":\"action\","
                << "\"session_id\":" << json_escape(session_id) << ","
                << "\"payload\":{"
                << "\"action\":\"save_action\","
                << "\"name\":" << json_escape(name) << ","
                << "\"saved_action\":" << dump_json(saved_action);
        if (!failure_policy.empty()) {
            request << ",\"failure_policy\":" << json_escape(failure_policy);
        }
        request
                << "}}";
    } else if (cmd == "replay") {
        if (argc < 4) {
            throw std::runtime_error("usage: gdb-agent replay SESSION_ID NAME [--socket path] [--force] [--failure-policy POLICY] or gdb-agent replay SESSION_ID --file PATH [--socket path] [--force] [--failure-policy POLICY]");
        }
        std::string session_id = argv[2];
        std::string file = option_value(argc, argv, "--file");
        std::string failure_policy = option_value(argc, argv, "--failure-policy", "");
        bool force = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--force") {
                force = true;
            }
        }
        std::string name;
        if (file.empty()) {
            name = argv[3];
        }
        request << "{"
                << "\"op\":\"action\","
                << "\"session_id\":" << json_escape(session_id) << ","
                << "\"payload\":{"
                << "\"action\":\"replay\",";
        if (!file.empty()) {
            request << "\"file\":" << json_escape(file);
        } else {
            request << "\"name\":" << json_escape(name);
        }
        request << ",\"force\":" << (force ? "true" : "false");
        if (!failure_policy.empty()) {
            request << ",\"failure_policy\":" << json_escape(failure_policy);
        }
        request << "}}";
    } else if (cmd == "finish") {
        if (argc < 3) {
            throw std::runtime_error("usage: gdb-agent finish SESSION_ID [--socket path] [--out report.md] [--agent-inference TEXT_OR_FILE] [--final-conclusion TEXT_OR_FILE]");
        }
        std::string session_id = argv[2];
        std::string report = option_value(argc, argv, "--out", "");
        std::string inference_arg = option_value(argc, argv, "--agent-inference", "");
        std::string conclusion_arg = option_value(argc, argv, "--final-conclusion", "");
        std::string inference = inference_arg.empty() ? "" : read_text_file_arg(inference_arg);
        std::string conclusion = conclusion_arg.empty() ? "" : read_text_file_arg(conclusion_arg);
        request << "{"
                << "\"op\":\"finish\","
                << "\"session_id\":" << json_escape(session_id) << ","
                << "\"report\":" << json_escape(report) << ","
                << "\"agent_inference\":" << json_escape(inference) << ","
                << "\"final_conclusion\":" << json_escape(conclusion)
                << "}";
    } else if (cmd == "close") {
        if (argc < 3) {
            throw std::runtime_error("usage: gdb-agent close SESSION_ID [--socket path]");
        }
        std::string session_id = argv[2];
        request << "{"
                << "\"op\":\"close\","
                << "\"session_id\":" << json_escape(session_id)
                << "}";
    } else if (cmd == "status") {
        if (argc < 3) {
            throw std::runtime_error("usage: gdb-agent status SESSION_ID [--socket path]");
        }
        std::string session_id = argv[2];
        request << "{"
                << "\"op\":\"status\","
                << "\"session_id\":" << json_escape(session_id)
                << "}";
    } else if (cmd == "list") {
        request << "{\"op\":\"list\"}";
    } else if (cmd == "shutdown") {
        request << "{\"op\":\"shutdown\"}";
    } else {
        throw std::runtime_error("unsupported client command: " + cmd);
    }

    std::cout << client_request(socket_path, request.str());
    return 0;
}

static int run_serve(const CliOptions &opts, const DebugTask &task) {
    validate_task(task);
    fs::create_directories(opts.assets);

    GdbSession session(opts.assets, task.working_directory);
    session.set_session_id(opts.session_id);
    SessionOutcome outcome;
    outcome.run_timeout_ms = effective_run_timeout_ms(opts, task);
    set_inferior_output_paths(outcome, opts.assets);
    ProbeState probe_state;

    try {
        outcome.state = SessionState::Starting;
        session.start();
        session.initialize(task);
        outcome.state = SessionState::Ready;
        collect_environment_info(session, task, outcome);

        if (!opts.replay_before_run.empty()) {
            std::cout << action_output_text(replay_action_file(session, &task, &outcome, probe_state, opts.replay_before_run));
        }

        if (task.core_dump) {
            outcome.core_mode = true;
            outcome.state = SessionState::Loading;
            auto load = session.load_core(task);
            session.evidence_store().add("SessionEvent", "Core load", load.command, load.raw_lines, false, load.record_sequences);
            outcome.stop_reason = "core_loaded";
            outcome.state = SessionState::Stopped;
            collect_core_evidence(session);
        } else {
            outcome.state = SessionState::Running;
            outcome.inferior_stdout_offset = 0;
            outcome.inferior_stderr_offset = 0;
            auto run = run_inferior(session,
                                    task,
                                    std::chrono::milliseconds(effective_run_timeout_ms(opts, task)));
            session.evidence_store().add("StopEvent", "Initial run stop", run.command, run.raw_lines, false, run.record_sequences);
            outcome.stop_reason = run.stop_reason.empty() ? "unknown" : run.stop_reason;
            outcome.signal_name = run.signal_name;
            outcome.segfault = run.signal_name == "SIGSEGV";
            outcome.run_timed_out = run.timed_out;
            update_outcome_from_stop(outcome, run);
            collect_stop_followup(session, outcome, run);
            for (const auto &result : handle_probe_stop(session, &task, &outcome, probe_state, run)) {
                std::cout << action_result_line(result);
            }
        }

        std::cout << "{\"ok\":true,\"session_id\":" << json_escape(opts.session_id)
                  << ",\"state\":" << json_escape(std::string(session_state_name(outcome.state)))
                  << ",\"stop_reason\":" << json_escape(outcome.stop_reason)
                  << ",\"signal\":" << json_escape(outcome.signal_name) << "}\n";

        bool finished = false;
        std::string line;
        while (!finished && std::getline(std::cin, line)) {
            ActionOutput output;
            if (!trim(line).empty()) {
                try {
                    output = handle_action_json(session, &task, &outcome, probe_state, parse_json(line));
                } catch (const std::exception &ex) {
                    output = single_output(make_action_error(session, ActionKind::Unknown, "", std::string("invalid json: ") + ex.what()));
                }
            }
            finished = output.final.finished;
            if (!output.final.action.empty() || !trim(line).empty()) {
                std::cout << action_output_text(output);
            }
        }

        flush_inferior_output(session, outcome);
        write_probe_snapshot(session, probe_state);
        write_session_files(opts, task, outcome, session);
        write_report(opts.report, opts.assets, task, outcome, session.evidence_store().all());
        std::cout << "{\"ok\":true,\"report\":" << json_escape(opts.report.string())
                  << ",\"assets\":" << json_escape(opts.assets.string()) << "}\n";
        bool tool_error = outcome.state == SessionState::Error;
        outcome.state = SessionState::Finishing;
        session.shutdown();
        outcome.state = SessionState::Closed;
        return tool_error ? 1 : 0;
    } catch (...) {
        try {
            session.shutdown();
        } catch (...) {
        }
        throw;
    }
}

int run_cli(int argc, char **argv) {
    try {
        if (argc >= 2) {
            std::string command = argv[1];
            if (command == "daemon") {
                return run_daemon(argc, argv);
            }
            if (command == "create" || command == "action" || command == "save-action" ||
                command == "replay" || command == "finish" ||
                command == "close" || command == "status" || command == "list" ||
                command == "shutdown") {
                return run_client_command(argc, argv);
            }
        }

        auto opts = parse_cli(argc, argv);
        auto task = load_task(opts.task_file);
        if (opts.command == "check") {
            return run_check(task);
        }
        if (opts.command == "serve") {
            return run_serve(opts, task);
        }
        throw std::runtime_error("unknown command: " + opts.command);
    } catch (const std::exception &ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
