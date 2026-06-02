#include "cli.hpp"

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
#include <optional>
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
    struct OnHitPolicy {
        bool configured = false;
        std::vector<std::string> actions;
        int timeout_ms = 5000;
        int max_output_bytes = 8192;
        int max_summary_lines = 80;
        std::string failure_policy = "continue_on_error";
        bool continue_after_hit = false;
    };

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

static void handle_action_line(GdbSession &session,
                               const DebugTask *task,
                               SessionOutcome *outcome,
                               ProbeState &probe_state,
                               const std::string &line,
                               bool &finished,
                               std::ostream &out);
static void replay_action_file(GdbSession &session,
                               const DebugTask *task,
                               SessionOutcome *outcome,
                               ProbeState &probe_state,
                               const fs::path &path,
                               std::ostream &out,
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

static void write_action_error(GdbSession &session,
                               std::ostream &out,
                               const std::string &action_name,
                               const std::string &message,
                               const std::string &title = "Action validation failed",
                               const std::map<std::string, std::string> &details = {}) {
    auto ev = add_action_tool_error(session, title, action_name, message, details);
    out << "{\"ok\":false";
    if (!action_name.empty()) {
        out << ",\"action\":" << json_escape(action_name);
    }
    out << ",\"error\":" << json_escape(message)
        << ",\"evidence\":" << json_escape(ev.id);
    for (const auto &[key, value] : details) {
        out << "," << json_escape(key) << ":" << json_escape(value);
    }
    out << "}\n";
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

static bool guard_action_state(GdbSession &session,
                               const SessionOutcome *outcome,
                               const std::string &action_name,
                               std::ostream &out) {
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
    out << "{\"ok\":false,\"action\":" << json_escape(action_name)
        << ",\"state\":" << json_escape(std::string(session_state_name(outcome->state)))
        << ",\"error\":" << json_escape(reason)
        << ",\"evidence\":" << json_escape(ev.id) << "}\n";
    return false;
}

static std::string json_string_field(const Json &action, const std::string &key) {
    std::string value = action.string_or(key);
    if (!value.empty()) {
        return value;
    }
    const Json *params = action.find("params");
    return params == nullptr ? "" : params->string_or(key);
}

static int json_int_field(const Json &action, const std::string &key, int fallback) {
    const Json *direct = action.find(key);
    if (direct != nullptr && direct->is_number()) {
        return static_cast<int>(direct->number_value);
    }
    const Json *params = action.find("params");
    return params == nullptr ? fallback : params->int_or(key, fallback);
}

static const Json *json_field(const Json &action, const std::string &key) {
    const Json *direct = action.find(key);
    if (direct != nullptr) {
        return direct;
    }
    const Json *params = action.find("params");
    return params == nullptr ? nullptr : params->find(key);
}

static std::string json_action_array(const std::vector<std::string> &actions) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < actions.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << actions[i];
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

static bool valid_on_hit_failure_policy(const std::string &policy) {
    return policy == "continue_on_error" || policy == "stop_on_error";
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

static const Json *on_hit_json_from(const Json &action) {
    const Json *on_hit = action.find("on_hit");
    if (on_hit == nullptr) {
        const Json *params = action.find("params");
        if (params != nullptr) {
            on_hit = params->find("on_hit");
        }
    }
    return on_hit;
}

static bool parse_on_hit_action(const Json &item,
                                std::vector<std::string> &actions,
                                std::string &error) {
    if (!item.is_object()) {
        error = "on_hit actions must be JSON objects";
        return false;
    }
    std::string action_name = item.string_or("action");
    if (action_name == "raw_mi") {
        error = "on_hit raw_mi action is not allowed";
        return false;
    }
    actions.push_back(dump_json(item));
    return true;
}

static bool parse_on_hit_actions_array(const Json &actions_json,
                                       std::vector<std::string> &actions,
                                       std::string &error) {
    if (!actions_json.is_array()) {
        error = "on_hit actions must be an array";
        return false;
    }
    for (const auto &item : actions_json.array_value) {
        if (!parse_on_hit_action(item, actions, error)) {
            return false;
        }
    }
    return true;
}

static bool parse_on_hit_policy(const Json &action,
                                ProbeState::OnHitPolicy &policy,
                                std::string &error) {
    const Json *on_hit = on_hit_json_from(action);
    if (on_hit == nullptr || on_hit->is_null()) {
        return true;
    }

    policy.configured = true;
    if (on_hit->is_array()) {
        return parse_on_hit_actions_array(*on_hit, policy.actions, error);
    }
    if (!on_hit->is_object()) {
        error = "on_hit must be an array or policy object";
        return false;
    }

    const Json *actions = on_hit->find("actions");
    if (actions != nullptr && !parse_on_hit_actions_array(*actions, policy.actions, error)) {
        return false;
    }

    if (const Json *timeout = on_hit->find("timeout_ms"); timeout != nullptr) {
        if (!timeout->is_number() || timeout->number_value <= 0) {
            error = "on_hit.timeout_ms must be a positive number";
            return false;
        }
        policy.timeout_ms = static_cast<int>(timeout->number_value);
    }
    if (const Json *max_output = on_hit->find("max_output_bytes"); max_output != nullptr) {
        if (!max_output->is_number() || max_output->number_value <= 0) {
            error = "on_hit.max_output_bytes must be a positive number";
            return false;
        }
        policy.max_output_bytes = static_cast<int>(max_output->number_value);
    }
    if (const Json *max_lines = on_hit->find("max_summary_lines"); max_lines != nullptr) {
        if (!max_lines->is_number() || max_lines->number_value <= 0) {
            error = "on_hit.max_summary_lines must be a positive number";
            return false;
        }
        policy.max_summary_lines = static_cast<int>(max_lines->number_value);
    }
    std::string failure_policy = on_hit->string_or("failure_policy");
    if (!failure_policy.empty()) {
        if (!valid_on_hit_failure_policy(failure_policy)) {
            error = "on_hit.failure_policy must be continue_on_error or stop_on_error";
            return false;
        }
        policy.failure_policy = failure_policy;
    }
    policy.continue_after_hit = on_hit->bool_or("continue_after_hit", false);
    return true;
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

static bool on_hit_response_failed(const std::string &response_text,
                                   std::string &error,
                                   std::string &evidence_id) {
    std::istringstream in(response_text);
    std::string line;
    bool saw_json = false;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line.front() != '{') {
            continue;
        }
        Json response;
        try {
            response = parse_json(line);
        } catch (const std::exception &ex) {
            error = std::string("on_hit action returned invalid JSON: ") + ex.what();
            return true;
        }
        if (!response.is_object()) {
            continue;
        }
        saw_json = true;
        std::string response_evidence = response.string_or("evidence");
        if (!response_evidence.empty()) {
            evidence_id = response_evidence;
        }
        std::string response_error_evidence = response.string_or("error_evidence");
        if (!response_error_evidence.empty()) {
            evidence_id = response_error_evidence;
        }
        const Json *ok = response.find("ok");
        if (ok != nullptr && ok->is_bool() && !ok->bool_value) {
            error = response.string_or("error", "on_hit action returned ok:false");
            return true;
        }
    }
    if (!saw_json && !trim(response_text).empty()) {
        error = "on_hit action did not return JSON status";
        return true;
    }
    return false;
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

static std::string action_name_from_text(const std::string &action_text) {
    try {
        Json action = parse_json(action_text);
        if (action.is_object()) {
            return action.string_or("action");
        }
    } catch (const std::exception &) {
    }
    return {};
}

static void add_timeout_to_on_hit_action(std::string &action_text, int timeout_ms) {
    Json action = parse_json(action_text);
    if (!action.is_object()) {
        return;
    }
    if (action.find("timeout_ms") == nullptr) {
        Json timeout;
        timeout.type = Json::Type::Number;
        timeout.number_value = timeout_ms;
        action.object_value["timeout_ms"] = timeout;
    }
    if (action.find("deadline_ms") == nullptr) {
        Json deadline;
        deadline.type = Json::Type::Number;
        deadline.number_value = timeout_ms;
        action.object_value["deadline_ms"] = deadline;
    }
    action_text = dump_json(action);
}

static ProbeState::OnHitActionResult add_on_hit_wrapper_evidence(
    GdbSession &session,
    const ProbeState::ProbeHitSnapshot &hit,
    const ProbeState::OnHitPolicy &policy,
    ProbeState::OnHitActionResult result,
    const std::string &action_text,
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
    evidence_text << "  \"action\": " << action_text << ",\n";
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
    std::ostream &out) {
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
        result.action_name = action_name_from_text(policy.actions[i]);
        if (result.action_name.empty()) {
            result.action_name = "unknown";
        }

        if (stop_remaining) {
            result.status = "skipped";
            result.skip_reason = "previous on_hit action failed with stop_on_error";
            result = add_on_hit_wrapper_evidence(session, hit, policy, std::move(result), policy.actions[i], "");
            results.push_back(std::move(result));
            continue;
        }

        std::string action_text = policy.actions[i];
        add_timeout_to_on_hit_action(action_text, policy.timeout_ms);
        size_t evidence_start = session.evidence_store().all().size();
        bool ignored_finish = false;
        std::ostringstream response;
        handle_action_line(session, task, outcome, probe_state, action_text, ignored_finish, response);
        result.action_evidence_ids = evidence_ids_since(session, evidence_start);
        std::string error_evidence;
        if (on_hit_response_failed(response.str(), result.error, error_evidence)) {
            result.status = "failed";
            result.error_evidence_id = error_evidence;
            if (result.error_evidence_id.empty() && !result.action_evidence_ids.empty()) {
                result.error_evidence_id = result.action_evidence_ids.back();
            }
            if (policy.failure_policy == "stop_on_error") {
                stop_remaining = true;
            }
        } else {
            result.status = "success";
        }
        out << response.str();
        result = add_on_hit_wrapper_evidence(session, hit, policy, std::move(result), action_text, response.str());
        results.push_back(std::move(result));
    }

    if (policy.continue_after_hit && !stop_remaining) {
        ProbeState::OnHitActionResult result;
        result.index = static_cast<int>(results.size() + 1);
        result.action_name = "continue_after_hit";
        result.failure_policy = policy.failure_policy;
        std::ostringstream action_text;
        action_text << "{\"action\":\"continue\",\"deadline_ms\":" << policy.timeout_ms << "}";
        size_t evidence_start = session.evidence_store().all().size();
        bool ignored_finish = false;
        std::ostringstream response;
        handle_action_line(session, task, outcome, probe_state, action_text.str(), ignored_finish, response);
        result.action_evidence_ids = evidence_ids_since(session, evidence_start);
        std::string error_evidence;
        if (on_hit_response_failed(response.str(), result.error, error_evidence)) {
            result.status = "failed";
            result.error_evidence_id = error_evidence;
            if (result.error_evidence_id.empty() && !result.action_evidence_ids.empty()) {
                result.error_evidence_id = result.action_evidence_ids.back();
            }
        } else {
            result.status = "success";
        }
        out << response.str();
        result = add_on_hit_wrapper_evidence(session, hit, policy, std::move(result), action_text.str(), response.str());
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

static void handle_probe_stop(GdbSession &session,
                              const DebugTask *task,
                              SessionOutcome *outcome,
                              ProbeState &probe_state,
                              const CommandResult &result,
                              std::ostream &out) {
    auto hit = prepare_probe_hit(probe_state, result);
    if (outcome != nullptr && !hit.number.empty()) {
        outcome->state = SessionState::Stopped;
        outcome->stop_reason = hit.stop_reason;
        outcome->signal_name = hit.signal_name;
    }
    auto on_hit_results = run_on_hit_actions(session, task, outcome, probe_state, hit, out);
    record_probe_hit(session, hit, on_hit_results);
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

static std::vector<std::string> load_replay_jsonl_actions(const fs::path &path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open replay file: " + path.string());
    }

    std::vector<std::string> actions;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        actions.push_back(line);
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

static void handle_action_line(GdbSession &session,
                               const DebugTask *task,
                               SessionOutcome *outcome,
                               ProbeState &probe_state,
                               const std::string &line,
                               bool &finished,
                               std::ostream &out) {
    if (trim(line).empty()) {
        return;
    }
    Json action;
    try {
        action = parse_json(line);
    } catch (const std::exception &ex) {
        write_action_error(session, out, "", std::string("invalid json: ") + ex.what());
        return;
    }
    std::string action_name = action.string_or("action");
    if (action_name.empty()) {
        write_action_error(session, out, "", "missing action");
        return;
    }
    if (!guard_action_state(session, outcome, action_name, out)) {
        return;
    }

    if (action_name == "finish_session" || action_name == "finish") {
        if (outcome != nullptr) {
            std::string inference = json_string_field(action, "agent_inference");
            std::string conclusion = json_string_field(action, "final_conclusion");
            if (conclusion.empty()) {
                conclusion = json_string_field(action, "final_agent_conclusion");
            }
            if (!inference.empty()) {
                outcome->agent_inference = inference;
            }
            if (!conclusion.empty()) {
                outcome->final_agent_conclusion = conclusion;
            }
        }
        finished = true;
        return;
    }
    if (action_name == "backtrace") {
        auto ev = collect_console(session, "Backtrace", "bt", true, std::chrono::milliseconds(json_int_field(action, "timeout_ms", 5000)));
        out << "{\"ok\":true,\"action\":\"backtrace\",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "locals") {
        auto ev = collect_console(session, "Local variables", "info locals", false, std::chrono::milliseconds(json_int_field(action, "timeout_ms", 5000)));
        out << "{\"ok\":true,\"action\":\"locals\",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "args_info") {
        auto ev = collect_console(session, "Frame arguments", "info args", false, std::chrono::milliseconds(json_int_field(action, "timeout_ms", 5000)));
        out << "{\"ok\":true,\"action\":\"args_info\",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "registers") {
        auto ev = collect_console(session, "Registers", "info registers", false, std::chrono::milliseconds(json_int_field(action, "timeout_ms", 5000)));
        out << "{\"ok\":true,\"action\":\"registers\",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "threads") {
        auto ev = collect_console(session, "Threads", "info threads", false, std::chrono::milliseconds(json_int_field(action, "timeout_ms", 5000)));
        out << "{\"ok\":true,\"action\":\"threads\",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "frame_select") {
        int frame = json_int_field(action, "frame", 0);
        std::string console_command = "frame " + std::to_string(frame);
        auto result = session.command("-interpreter-exec console " + mi_quote(console_command),
                                      std::chrono::milliseconds(json_int_field(action, "timeout_ms", 5000)));
        auto ev = session.evidence_store().add("GdbCommand", "Frame select", console_command, result.raw_lines, false, result.record_sequences);
        if (result.result_class == "error" || result.timed_out) {
            write_action_error(session,
                               out,
                               "frame_select",
                               command_error_message(result, "GDB rejected frame_select"),
                               "Frame select failed",
                               {{"command_evidence", ev.id}, {"frame", std::to_string(frame)}});
            return;
        }
        out << "{\"ok\":true,\"action\":\"frame_select\",\"frame\":" << frame
            << ",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "evaluate") {
        std::string expr = json_string_field(action, "expression");
        if (expr.empty()) {
            write_action_error(session, out, "evaluate", "missing expression");
            return;
        }
        std::string console_command = "p " + expr;
        auto result = session.command("-interpreter-exec console " + mi_quote(console_command),
                                      std::chrono::milliseconds(json_int_field(action, "timeout_ms", 5000)));
        auto ev = session.evidence_store().add("GdbCommand", "Evaluate", console_command, result.raw_lines, false, result.record_sequences);
        if (result.result_class == "error" || result.timed_out) {
            write_action_error(session,
                               out,
                               "evaluate",
                               command_error_message(result, "GDB rejected evaluate"),
                               "Evaluate failed",
                               {{"command_evidence", ev.id}, {"expression", expr}});
            return;
        }
        out << "{\"ok\":true,\"action\":\"evaluate\",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "breakpoint_set") {
        std::string location = json_string_field(action, "location");
        if (location.empty()) {
            write_action_error(session, out, "breakpoint_set", "missing location");
            return;
        }
        ProbeState::OnHitPolicy on_hit_policy;
        std::string on_hit_error;
        if (!parse_on_hit_policy(action, on_hit_policy, on_hit_error)) {
            auto error_ev = add_tool_error(session,
                                           "Breakpoint on-hit policy failed",
                                           "breakpoint_set",
                                           on_hit_error);
            out << "{\"ok\":false,\"action\":\"breakpoint_set\","
                << "\"error\":\"invalid on_hit policy\","
                << "\"evidence\":" << json_escape(error_ev.id) << "}\n";
            return;
        }

        auto insert = session.command("-break-insert " + mi_quote(location));
        add_command_evidence(session, "GdbCommand", "Breakpoint set", insert);
        std::string number = breakpoint_number_from(insert);
        if (insert.result_class == "error" || number.empty()) {
            auto ev = add_tool_error(session,
                                     "Breakpoint set failed",
                                     "breakpoint_set",
                                     number.empty() ? "GDB did not return a breakpoint number" : "GDB rejected the breakpoint");
            out << "{\"ok\":false,\"action\":\"breakpoint_set\",\"error\":\"failed to set breakpoint\","
                << "\"evidence\":" << json_escape(ev.id) << "}\n";
            return;
        }

        std::string condition = json_string_field(action, "condition");
        std::string condition_evidence;
        if (!condition.empty()) {
            auto cond = session.command("-break-condition " + number + " " + condition);
            auto ev = session.evidence_store().add("GdbCommand", "Breakpoint condition", cond.command, cond.raw_lines, false, cond.record_sequences);
            condition_evidence = ev.id;
            if (cond.result_class == "error") {
                auto error_ev = add_tool_error(session,
                                               "Breakpoint condition failed",
                                               "breakpoint_set",
                                               "GDB rejected the breakpoint condition");
                out << "{\"ok\":false,\"action\":\"breakpoint_set\",\"breakpoint\":" << json_escape(number)
                    << ",\"error\":\"failed to set breakpoint condition\","
                    << "\"condition_evidence\":" << json_escape(condition_evidence)
                    << ",\"evidence\":" << json_escape(error_ev.id) << "}\n";
                return;
            }
        }

        if (!number.empty()) {
            ProbeState::ProbeInfo probe;
            probe.number = number;
            probe.kind = "breakpoint";
            probe.location = location;
            probe.condition = condition;
            probe.comment = json_string_field(action, "comment");
            probe.purpose = json_string_field(action, "purpose");
            probe.on_hit_policy = std::move(on_hit_policy);
            probe_state.probes_by_number[number] = std::move(probe);
        }

        out << "{\"ok\":true,\"action\":\"breakpoint_set\",\"breakpoint\":" << json_escape(number);
        if (!condition_evidence.empty()) {
            out << ",\"condition_evidence\":" << json_escape(condition_evidence);
        }
        out << "}\n";
        return;
    }
    if (action_name == "watchpoint_set") {
        std::string expression = json_string_field(action, "expression");
        if (expression.empty()) {
            write_action_error(session, out, "watchpoint_set", "missing expression");
            return;
        }
        ProbeState::OnHitPolicy on_hit_policy;
        std::string on_hit_error;
        if (!parse_on_hit_policy(action, on_hit_policy, on_hit_error)) {
            auto error_ev = add_tool_error(session,
                                           "Watchpoint on-hit policy failed",
                                           "watchpoint_set",
                                           on_hit_error);
            out << "{\"ok\":false,\"action\":\"watchpoint_set\","
                << "\"error\":\"invalid on_hit policy\","
                << "\"evidence\":" << json_escape(error_ev.id) << "}\n";
            return;
        }

        auto result = session.command("-break-watch " + expression);
        auto ev = session.evidence_store().add("GdbCommand", "Watchpoint set", result.command, result.raw_lines, false, result.record_sequences);
        std::string number = breakpoint_number_from(result);
        if (result.result_class == "error" || number.empty()) {
            auto error_ev = add_tool_error(session,
                                           "Watchpoint set failed",
                                           "watchpoint_set",
                                           number.empty() ? "GDB did not return a watchpoint number" : "GDB rejected the watchpoint");
            out << "{\"ok\":false,\"action\":\"watchpoint_set\",\"error\":\"failed to set watchpoint\","
                << "\"command_evidence\":" << json_escape(ev.id)
                << ",\"evidence\":" << json_escape(error_ev.id) << "}\n";
            return;
        }

        std::string condition = json_string_field(action, "condition");
        std::string condition_evidence;
        if (!condition.empty()) {
            auto cond = session.command("-break-condition " + number + " " + condition);
            auto cond_ev = session.evidence_store().add("GdbCommand", "Watchpoint condition", cond.command, cond.raw_lines, false, cond.record_sequences);
            condition_evidence = cond_ev.id;
            if (cond.result_class == "error") {
                auto error_ev = add_tool_error(session,
                                               "Watchpoint condition failed",
                                               "watchpoint_set",
                                               "GDB rejected the watchpoint condition");
                out << "{\"ok\":false,\"action\":\"watchpoint_set\",\"watchpoint\":" << json_escape(number)
                    << ",\"error\":\"failed to set watchpoint condition\","
                    << "\"condition_evidence\":" << json_escape(condition_evidence)
                    << ",\"evidence\":" << json_escape(error_ev.id) << "}\n";
                return;
            }
        }

        ProbeState::ProbeInfo probe;
        probe.number = number;
        probe.kind = "watchpoint";
        probe.expression = expression;
        probe.condition = condition;
        probe.comment = json_string_field(action, "comment");
        probe.purpose = json_string_field(action, "purpose");
        probe.on_hit_policy = std::move(on_hit_policy);
        probe_state.probes_by_number[number] = std::move(probe);
        out << "{\"ok\":true,\"action\":\"watchpoint_set\",\"watchpoint\":" << json_escape(number)
                  << ",\"evidence\":" << json_escape(ev.id);
        if (!condition_evidence.empty()) {
            out << ",\"condition_evidence\":" << json_escape(condition_evidence);
        }
        out << "}\n";
        return;
    }
    if (action_name == "catchpoint_set") {
        std::string event = json_string_field(action, "event");
        if (event.empty()) {
            auto error_ev = add_tool_error(session,
                                           "Catchpoint set failed",
                                           "catchpoint_set",
                                           "missing event");
            out << "{\"ok\":false,\"action\":\"catchpoint_set\",\"error\":\"missing event\","
                << "\"evidence\":" << json_escape(error_ev.id) << "}\n";
            return;
        }
        if (event != "throw") {
            auto error_ev = add_tool_error(session,
                                           "Catchpoint set failed",
                                           "catchpoint_set",
                                           "unsupported catchpoint event: " + event);
            out << "{\"ok\":false,\"action\":\"catchpoint_set\",\"error\":\"unsupported catchpoint event\","
                << "\"event\":" << json_escape(event)
                << ",\"evidence\":" << json_escape(error_ev.id) << "}\n";
            return;
        }
        ProbeState::OnHitPolicy on_hit_policy;
        std::string on_hit_error;
        if (!parse_on_hit_policy(action, on_hit_policy, on_hit_error)) {
            auto error_ev = add_tool_error(session,
                                           "Catchpoint on-hit policy failed",
                                           "catchpoint_set",
                                           on_hit_error);
            out << "{\"ok\":false,\"action\":\"catchpoint_set\","
                << "\"error\":\"invalid on_hit policy\","
                << "\"evidence\":" << json_escape(error_ev.id) << "}\n";
            return;
        }

        auto result = session.command("-interpreter-exec console " + mi_quote("catch throw"));
        auto ev = session.evidence_store().add("GdbCommand", "Catchpoint set", result.command, result.raw_lines, false, result.record_sequences);
        std::string number = breakpoint_number_from(result);
        if (result.result_class == "error" || number.empty()) {
            auto error_ev = add_tool_error(session,
                                           "Catchpoint set failed",
                                           "catchpoint_set",
                                           number.empty() ? "GDB did not return a catchpoint number" : "GDB rejected the catchpoint");
            out << "{\"ok\":false,\"action\":\"catchpoint_set\",\"error\":\"failed to set catchpoint\","
                << "\"event\":" << json_escape(event)
                << ",\"command_evidence\":" << json_escape(ev.id)
                << ",\"evidence\":" << json_escape(error_ev.id) << "}\n";
            return;
        }

        ProbeState::ProbeInfo probe;
        probe.number = number;
        probe.kind = "catchpoint";
        probe.event = event;
        probe.location = "catch throw";
        probe.comment = json_string_field(action, "comment");
        probe.purpose = json_string_field(action, "purpose");
        probe.on_hit_policy = std::move(on_hit_policy);
        probe_state.probes_by_number[number] = std::move(probe);
        out << "{\"ok\":true,\"action\":\"catchpoint_set\",\"catchpoint\":" << json_escape(number)
            << ",\"event\":" << json_escape(event)
            << ",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "probe_list") {
        auto result = session.command("-break-list");
        auto ev = session.evidence_store().add("GdbCommand", "Probe list", result.command, result.raw_lines, false, result.record_sequences);

        std::ostringstream evidence_text;
        evidence_text << "{\n";
        evidence_text << "  \"probes\": " << probe_array_json(probe_state) << "\n";
        evidence_text << "}\n";
        auto metadata_ev = session.evidence_store().add_text("SessionEvent",
                                                             "Probe metadata snapshot",
                                                             "probe_list",
                                                             evidence_text.str());
        out << "{\"ok\":true,\"action\":\"probe_list\",\"evidence\":" << json_escape(ev.id)
            << ",\"metadata_evidence\":" << json_escape(metadata_ev.id)
            << ",\"probes\":" << probe_array_json(probe_state) << "}\n";
        return;
    }
    if (action_name == "probe_delete" || action_name == "probe_enable" || action_name == "probe_disable") {
        int number = json_int_field(action, "number", -1);
        if (number < 0) {
            write_action_error(session, out, action_name, "missing probe number");
            return;
        }

        std::string action = "probe_delete";
        std::string command = "-break-delete ";
        std::string title = "Probe delete";
        if (action_name == "probe_enable") {
            action = "probe_enable";
            command = "-break-enable ";
            title = "Probe enable";
        } else if (action_name == "probe_disable") {
            action = "probe_disable";
            command = "-break-disable ";
            title = "Probe disable";
        }

        auto result = session.command(command + std::to_string(number));
        auto ev = session.evidence_store().add("GdbCommand", title, result.command, result.raw_lines, false, result.record_sequences);
        if (result.result_class == "error" || result.timed_out) {
            write_action_error(session,
                               out,
                               action,
                               command_error_message(result, "GDB rejected probe operation"),
                               title + " failed",
                               {{"command_evidence", ev.id}, {"number", std::to_string(number)}});
            return;
        }
        auto probe_it = probe_state.probes_by_number.find(std::to_string(number));
        if (probe_it != probe_state.probes_by_number.end()) {
            if (action_name == "probe_delete") {
                probe_it->second.deleted = true;
                probe_it->second.enabled = false;
            } else if (action_name == "probe_enable") {
                probe_it->second.enabled = true;
            } else if (action_name == "probe_disable") {
                probe_it->second.enabled = false;
            }
        }
        out << "{\"ok\":true,\"action\":" << json_escape(action) << ",\"number\":" << number
                  << ",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "run") {
        int default_deadline_ms = outcome != nullptr ? outcome->run_timeout_ms : (task != nullptr ? task->run_timeout_ms : 30000);
        int deadline_ms = json_int_field(action, "deadline_ms", default_deadline_ms);
        std::string stdin_path = json_string_field(action, "stdin");
        CommandResult result;
        if (outcome != nullptr) {
            outcome->state = SessionState::Running;
            outcome->inferior_stdout_offset = 0;
            outcome->inferior_stderr_offset = 0;
        }

        if (task != nullptr) {
            DebugTask run_task = *task;
            if (!stdin_path.empty()) {
                fs::path input(stdin_path);
                run_task.stdin_path = input.is_absolute()
                                          ? input
                                          : fs::weakly_canonical(run_task.working_directory / input);
            }
            result = run_inferior(session, run_task, std::chrono::milliseconds(deadline_ms));
        } else if (stdin_path.empty()) {
            result = session.exec_control("-exec-run", std::chrono::milliseconds(deadline_ms));
        } else {
            result = session.exec_control("-interpreter-exec console " +
                                              mi_quote("run < " + shell_quote_for_report(stdin_path)),
                                          std::chrono::milliseconds(deadline_ms));
        }
        auto ev = session.evidence_store().add("StopEvent", "Run stop", result.command, result.raw_lines, false, result.record_sequences);
        if (outcome != nullptr) {
            update_outcome_from_stop(*outcome, result);
        }
        if (result.result_class == "error") {
            write_action_error(session,
                               out,
                               "run",
                               command_error_message(result, "GDB rejected run"),
                               "Run failed",
                               {{"command_evidence", ev.id}});
            return;
        }
        if (outcome != nullptr) {
            collect_stop_followup(session, *outcome, result);
        }
        handle_probe_stop(session, task, outcome, probe_state, result, out);
        out << "{\"ok\":true,\"action\":\"run\",\"stop_reason\":" << json_escape(result.stop_reason)
                  << ",\"signal\":" << json_escape(result.signal_name)
                  << ",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "continue") {
        int deadline_ms = json_int_field(action, "deadline_ms", 30000);
        if (outcome != nullptr) {
            outcome->state = SessionState::Running;
        }
        auto result = session.exec_control("-exec-continue", std::chrono::milliseconds(deadline_ms));
        auto ev = session.evidence_store().add("StopEvent", "Continue stop", result.command, result.raw_lines, false, result.record_sequences);
        if (outcome != nullptr) {
            update_outcome_from_stop(*outcome, result);
        }
        if (result.result_class == "error") {
            write_action_error(session,
                               out,
                               "continue",
                               command_error_message(result, "GDB rejected continue"),
                               "Continue failed",
                               {{"command_evidence", ev.id}});
            return;
        }
        if (outcome != nullptr) {
            collect_stop_followup(session, *outcome, result);
        }
        handle_probe_stop(session, task, outcome, probe_state, result, out);
        out << "{\"ok\":true,\"action\":\"continue\",\"stop_reason\":" << json_escape(result.stop_reason)
                  << ",\"signal\":" << json_escape(result.signal_name)
                  << ",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "raw_mi") {
        std::string command = json_string_field(action, "command");
        std::string risk = json_string_field(action, "risk");
        if (command.empty()) {
            write_action_error(session, out, "raw_mi", "missing command");
            return;
        }
        if (risk != "advanced") {
            write_action_error(session, out, "raw_mi", "raw_mi requires risk=advanced");
            return;
        }
        int timeout_ms = json_int_field(action, "timeout_ms", 5000);
        auto result = session.command(command, std::chrono::milliseconds(timeout_ms));
        auto ev = session.evidence_store().add("GdbCommand", "Raw MI", result.command, result.raw_lines, false, result.record_sequences);
        out << "{\"ok\":true,\"action\":\"raw_mi\",\"result_class\":" << json_escape(result.result_class)
                  << ",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "hypothesis_create") {
        std::string id = json_string_field(action, "id");
        if (id.empty()) {
            id = next_hypothesis_id(probe_state);
        }
        std::string title = json_string_field(action, "title");
        if (title.empty()) {
            title = "Untitled hypothesis";
        }
        std::string description = json_string_field(action, "description");
        fs::path file = hypothesis_file_for(session, id);

        std::ostringstream md;
        md << "# " << id << " " << title << "\n\n";
        md << "Status: EvidenceCollectionStarted\n\n";
        if (!description.empty()) {
            md << "## Description\n\n" << description << "\n\n";
        }
        write_text_file(file, md.str());
        ProbeState::HypothesisRecord record;
        record.id = id;
        record.title = title;
        record.description = description;
        probe_state.hypotheses_by_id[id] = std::move(record);
        write_hypothesis_index(session, probe_state);
        out << "{\"ok\":true,\"action\":\"hypothesis_create\",\"id\":" << json_escape(id)
                  << ",\"file\":" << json_escape(file.lexically_normal().string()) << "}\n";
        return;
    }
    if (action_name == "hypothesis_check") {
        std::string id = json_string_field(action, "hypothesis");
        std::string expression = json_string_field(action, "expression");
        if (id.empty() || expression.empty()) {
            write_action_error(session, out, "hypothesis_check", "hypothesis_check requires hypothesis and expression");
            return;
        }

        std::string description = json_string_field(action, "description");
        if (description.empty()) {
            description = expression;
        }
        std::string assertion = json_string_field(action, "assertion");
        if (assertion.empty()) {
            assertion = "none";
        }
        std::string expected = json_string_field(action, "expected");

        auto ev = collect_console(session, "Hypothesis check", "p " + expression);
        std::string observed = ev.summary;
        auto assertion_result = evaluate_hypothesis_assertion(assertion, observed, expected);
        std::string error_evidence_id;
        if (assertion_result.status == "unknown") {
            std::ostringstream message;
            message << "hypothesis_check assertion could not be evaluated";
            if (!assertion_result.reason.empty()) {
                message << ": " << assertion_result.reason;
            }
            auto error_ev = add_tool_error(session, "Hypothesis check assertion unknown", "hypothesis_check", message.str());
            error_evidence_id = error_ev.id;
        }
        fs::path file = hypothesis_file_for(session, id);

        std::ostringstream md;
        md << "## Check: " << description << "\n\n";
        md << "- Check ID: `C" << (probe_state.hypotheses_by_id[id].checks.size() + 1) << "`\n";
        md << "- Expression: `" << expression << "`\n";
        md << "- Evidence: `" << ev.id << "`\n";
        md << "- Assertion: `" << assertion << "`\n";
        if (!expected.empty()) {
            md << "- Expected: `" << expected << "`\n";
        }
        md << "- Status: `" << assertion_result.status << "`\n";
        if (!error_evidence_id.empty()) {
            md << "- Error evidence: `" << error_evidence_id << "`\n";
        }
        md << "\n### Observed\n\n";
        md << "```text\n" << observed << "\n```\n\n";
        append_text(file, md.str());
        auto &record = probe_state.hypotheses_by_id[id];
        if (record.id.empty()) {
            record.id = id;
            record.title = id;
        }
        ProbeState::HypothesisCheck check;
        std::ostringstream check_id;
        check_id << "C" << (record.checks.size() + 1);
        check.id = check_id.str();
        check.description = description;
        check.expression = expression;
        check.assertion = assertion;
        check.expected = expected;
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

        out << "{\"ok\":true,\"action\":\"hypothesis_check\",\"hypothesis\":" << json_escape(id)
                  << ",\"check_id\":" << json_escape(record.checks.back().id)
                  << ",\"description\":" << json_escape(description)
                  << ",\"expression\":" << json_escape(expression)
                  << ",\"assertion\":" << json_escape(assertion)
                  << ",\"expected\":" << json_escape(expected)
                  << ",\"observed\":" << json_escape(observed)
                  << ",\"status\":" << json_escape(assertion_result.status)
                  << ",\"evidence\":" << json_escape(ev.id)
                  << ",\"error_evidence\":"
                  << (error_evidence_id.empty() ? std::string("null") : json_escape(error_evidence_id))
                  << "}\n";
        return;
    }
    if (action_name == "hypothesis_conclude") {
        std::string id = json_string_field(action, "hypothesis");
        std::string conclusion = json_string_field(action, "conclusion");
        if (conclusion.empty()) {
            conclusion = "Inconclusive";
        }
        std::string inference = json_string_field(action, "inference");
        if (id.empty()) {
            write_action_error(session, out, "hypothesis_conclude", "hypothesis_conclude requires hypothesis");
            return;
        }
        fs::path file = hypothesis_file_for(session, id);
        std::ostringstream md;
        md << "## Agent Conclusion\n\n";
        md << "- Conclusion: `" << conclusion << "`\n";
        if (!inference.empty()) {
            md << "\n" << inference << "\n";
        }
        md << "\n";
        append_text(file, md.str());
        auto &record = probe_state.hypotheses_by_id[id];
        if (record.id.empty()) {
            record.id = id;
            record.title = id;
        }
        record.agent_conclusion = conclusion;
        record.agent_inference = inference;
        write_hypothesis_index(session, probe_state);
        out << "{\"ok\":true,\"action\":\"hypothesis_conclude\",\"hypothesis\":" << json_escape(id)
                  << ",\"conclusion\":" << json_escape(conclusion) << "}\n";
        return;
    }
    if (action_name == "save_action") {
        std::string name = json_string_field(action, "name");
        std::string failure_policy = normalize_replay_failure_policy(json_string_field(action, "failure_policy"));
        const Json *saved = json_field(action, "saved_action");
        std::string saved_action;
        if (saved != nullptr && saved->is_string()) {
            saved_action = saved->string_value;
        } else if (saved != nullptr && saved->is_object()) {
            saved_action = dump_json(*saved);
        }
        if (name.empty() || saved_action.empty()) {
            write_action_error(session, out, "save_action", "save_action requires name and saved_action");
            return;
        }

        fs::path replay_dir = session.assets_dir() / "replay";
        fs::create_directories(replay_dir);
        std::string replay_base = slugify(name);
        fs::path replay_file = replay_dir / (replay_base + ".jsonl");
        fs::path replay_plan = replay_dir / (replay_base + ".json");
        std::ofstream replay_out(replay_file, std::ios::app);
        if (!replay_out) {
            write_action_error(session, out, "save_action", "failed to write replay file");
            return;
        }
        replay_out << saved_action << '\n';
        replay_out.close();
        try {
            rebuild_replay_plan_from_jsonl(replay_file,
                                           replay_plan,
                                           name,
                                           task,
                                           session.session_id(),
                                           failure_policy);
        } catch (const std::exception &ex) {
            auto ev = session.evidence_store().add_text("ToolError",
                                                        "Replay plan write failed",
                                                        replay_plan.lexically_normal().string(),
                                                        ex.what());
            out << "{\"ok\":false,\"action\":\"save_action\",\"error\":\"failed to write replay plan\","
                << "\"evidence\":" << json_escape(ev.id) << "}\n";
            return;
        }
        out << "{\"ok\":true,\"action\":\"save_action\",\"file\":"
                  << json_escape(replay_file.lexically_normal().string()) << ",\"plan\":"
                  << json_escape(replay_plan.lexically_normal().string())
                  << ",\"failure_policy\":" << json_escape(failure_policy) << "}\n";
        return;
    }
    if (action_name == "replay") {
        std::string file = json_string_field(action, "file");
        std::string name = json_string_field(action, "name");
        bool force = action.bool_or("force", false);
        const Json *params = action.find("params");
        if (params != nullptr && params->is_object()) {
            force = params->bool_or("force", force);
        }
        std::string failure_policy_override = json_string_field(action, "failure_policy");
        fs::path replay_file;
        if (!file.empty()) {
            replay_file = file;
        } else if (!name.empty()) {
            fs::path replay_dir = session.assets_dir() / "replay";
            fs::path plan = replay_dir / (slugify(name) + ".json");
            fs::path jsonl = replay_dir / (slugify(name) + ".jsonl");
            replay_file = fs::exists(plan) ? plan : jsonl;
        } else {
            write_action_error(session, out, "replay", "replay requires file or name");
            return;
        }
        try {
            replay_action_file(session,
                               task,
                               outcome,
                               probe_state,
                               replay_file,
                               out,
                               force,
                               failure_policy_override);
        } catch (const std::exception &ex) {
            write_action_error(session,
                               out,
                               "replay",
                               ex.what(),
                               "Replay failed",
                               {{"file", replay_file.lexically_normal().string()}});
        }
        return;
    }
    auto ev = add_tool_error(session,
                             "Unsupported action",
                             action_name,
                             "unsupported action");
    out << "{\"ok\":false,\"action\":" << json_escape(action_name)
        << ",\"error\":\"unsupported action\","
        << "\"evidence\":" << json_escape(ev.id) << "}\n";
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

static std::string response_evidence_id(const Json &response) {
    std::string evidence = response.string_or("evidence");
    if (!evidence.empty()) {
        return evidence;
    }
    const Json *steps = response.find("steps");
    if (steps != nullptr && steps->is_array() && !steps->array_value.empty()) {
        const Json &last = steps->array_value.back();
        if (last.is_object()) {
            evidence = last.string_or("evidence");
            if (!evidence.empty()) {
                return evidence;
            }
            return last.string_or("error_evidence");
        }
    }
    return {};
}

static bool response_failed(const std::string &response_text,
                            std::string &error,
                            std::string &evidence_id) {
    std::istringstream in(response_text);
    std::string line;
    bool saw_json = false;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line.front() != '{') {
            continue;
        }
        Json response = parse_json(line);
        if (!response.is_object()) {
            continue;
        }
        saw_json = true;
        std::string response_evidence = response_evidence_id(response);
        if (!response_evidence.empty()) {
            evidence_id = response_evidence;
        }
        const Json *ok = response.find("ok");
        if (ok != nullptr && ok->is_bool() && !ok->bool_value) {
            error = response.string_or("error", "replayed action returned ok:false");
            return true;
        }
    }
    if (!saw_json && !trim(response_text).empty()) {
        error = "replayed action did not return JSON status";
        return true;
    }
    return false;
}

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

static ReplayStepRunResult replay_action_text(GdbSession &session,
                                              const DebugTask *task,
                                              SessionOutcome *outcome,
                                              ProbeState &probe_state,
                                              const std::string &plan_name,
                                              const std::string &step_id,
                                              int index,
                                              const std::string &line,
                                              const std::string &failure_policy,
                                              std::ostream &out) {
    ReplayStepRunResult result;
    result.index = index;
    result.step_id = step_id;
    result.failure_policy = failure_policy;
    std::string action_name = "unknown";
    try {
        Json parsed_action = parse_json(line);
        if (parsed_action.is_object()) {
            action_name = parsed_action.string_or("action", replay_action_display_name(parsed_action, index));
        }
    } catch (...) {
    }
    result.action_name = action_name;

    bool ignored_finish = false;
    std::ostringstream step_output;
    try {
        handle_action_line(session, task, outcome, probe_state, line, ignored_finish, step_output);
        ignored_finish = false;
        std::string error;
        std::string child_evidence;
        bool failed = response_failed(step_output.str(), error, child_evidence);
        result.status = failed ? "failed" : "success";
        result.error = failed ? error : "";
        result.action_evidence_id = child_evidence;
        if (failed && result.error_evidence_id.empty()) {
            std::ostringstream error_text;
            error_text << "{\n";
            error_text << "  \"plan\": " << json_escape(plan_name) << ",\n";
            error_text << "  \"step_id\": " << json_escape(step_id) << ",\n";
            error_text << "  \"index\": " << index << ",\n";
            error_text << "  \"action_name\": " << json_escape(action_name) << ",\n";
            error_text << "  \"failure_policy\": " << json_escape(failure_policy) << ",\n";
            error_text << "  \"error\": " << json_escape(result.error) << ",\n";
            error_text << "  \"response\": " << json_escape(step_output.str()) << "\n";
            error_text << "}\n";
            auto ev = session.evidence_store().add_text("ToolError", "Replay step reported failure " + step_id, action_name, error_text.str());
            result.error_evidence_id = ev.id;
        }
        std::ostringstream evidence_text;
        evidence_text << "{\n";
        evidence_text << "  \"plan\": " << json_escape(plan_name) << ",\n";
        evidence_text << "  \"step_id\": " << json_escape(step_id) << ",\n";
        evidence_text << "  \"index\": " << index << ",\n";
        evidence_text << "  \"action_name\": " << json_escape(action_name) << ",\n";
        evidence_text << "  \"status\": " << json_escape(result.status) << ",\n";
        evidence_text << "  \"failure_policy\": " << json_escape(failure_policy) << ",\n";
        evidence_text << "  \"action_json\": " << json_escape(line) << ",\n";
        evidence_text << "  \"action_evidence\": " << json_escape(result.action_evidence_id) << ",\n";
        evidence_text << "  \"error_evidence\": " << json_escape(result.error_evidence_id) << ",\n";
        evidence_text << "  \"error\": " << json_escape(result.error) << ",\n";
        evidence_text << "  \"response\": " << json_escape(step_output.str()) << "\n";
        evidence_text << "}\n";
        auto ev = session.evidence_store().add_text("ReplayStep", "Replay step " + step_id + " " + result.status, action_name, evidence_text.str());
        result.evidence_id = ev.id;
        out << step_output.str();
    } catch (const std::exception &ex) {
        result.status = "failed";
        result.error = ex.what();
        std::ostringstream evidence_text;
        evidence_text << "{\n";
        evidence_text << "  \"plan\": " << json_escape(plan_name) << ",\n";
        evidence_text << "  \"step_id\": " << json_escape(step_id) << ",\n";
        evidence_text << "  \"index\": " << index << ",\n";
        evidence_text << "  \"action_name\": " << json_escape(action_name) << ",\n";
        evidence_text << "  \"status\": \"failed\",\n";
        evidence_text << "  \"failure_policy\": " << json_escape(failure_policy) << ",\n";
        evidence_text << "  \"action\": " << json_escape(line) << ",\n";
        evidence_text << "  \"error\": " << json_escape(ex.what()) << "\n";
        evidence_text << "}\n";
        auto ev = session.evidence_store().add_text("ToolError", "Replay step failed " + step_id, action_name, evidence_text.str());
        result.evidence_id = ev.id;
        result.error_evidence_id = ev.id;
        out << "{\"ok\":false,\"action\":\"replay_step\",\"step_id\":" << json_escape(step_id)
            << ",\"error\":" << json_escape(ex.what()) << ",\"evidence\":" << json_escape(ev.id) << "}\n";
    }
    return result;
}

static ReplayRunResult replay_json_plan(GdbSession &session,
                                        const DebugTask *task,
                                        SessionOutcome *outcome,
                                        ProbeState &probe_state,
                                        const fs::path &path,
                                        const Json &plan,
                                        std::ostream &out,
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
        ReplayStepRunResult step_result = replay_action_text(session,
                                                             task,
                                                             outcome,
                                                             probe_state,
                                                             plan_name,
                                                             step_id,
                                                             index,
                                                             dump_json(*action),
                                                             step_policy,
                                                             out);
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

static void replay_action_file(GdbSession &session,
                               const DebugTask *task,
                               SessionOutcome *outcome,
                               ProbeState &probe_state,
                               const fs::path &path,
                               std::ostream &out,
                               bool force,
                               const std::string &failure_policy_override) {
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
        out << replay_result_json(path, result);
        return;
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
                                      out,
                                      force,
                                      failure_policy_override);
        } else {
            std::string policy = normalize_replay_failure_policy(failure_policy_override);
            result.failure_policy = policy;
            result.steps.push_back(replay_action_text(session,
                                                      task,
                                                      outcome,
                                                      probe_state,
                                                      path.stem().string(),
                                                      "a1",
                                                      1,
                                                      dump_json(plan),
                                                      policy,
                                                      out));
            result.ok = result.steps.back().status != "failed";
        }
        add_replay_run_evidence(session, path, result);
        out << replay_result_json(path, result);
        return;
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
        ReplayStepRunResult step_result = replay_action_text(session,
                                                             task,
                                                             outcome,
                                                             probe_state,
                                                             plan_name,
                                                             step_id.str(),
                                                             index,
                                                             line,
                                                             plan_policy,
                                                             out);
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
    out << replay_result_json(path, result);
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

    std::ostringstream replay_output;
    if (!live.opts.replay_before_run.empty()) {
        replay_action_file(*live.session, &live.task, &live.outcome, live.probe_state, live.opts.replay_before_run, replay_output);
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
        handle_probe_stop(*live.session, &live.task, &live.outcome, live.probe_state, run, replay_output);
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
        bool finished = false;
        std::ostringstream out;
        handle_action_line(*live.session, &live.task, &live.outcome, live.probe_state, dump_json(*payload), finished, out);
        if (finished) {
            std::string response = finish_live_session_response(session_id, live, "");
            sessions.erase(it);
            return response;
        }
        return out.str();
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
            replay_action_file(session, &task, &outcome, probe_state, opts.replay_before_run, std::cout);
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
            handle_probe_stop(session, &task, &outcome, probe_state, run, std::cout);
        }

        std::cout << "{\"ok\":true,\"session_id\":" << json_escape(opts.session_id)
                  << ",\"state\":" << json_escape(std::string(session_state_name(outcome.state)))
                  << ",\"stop_reason\":" << json_escape(outcome.stop_reason)
                  << ",\"signal\":" << json_escape(outcome.signal_name) << "}\n";

        bool finished = false;
        std::string line;
        while (!finished && std::getline(std::cin, line)) {
            handle_action_line(session, &task, &outcome, probe_state, line, finished, std::cout);
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
