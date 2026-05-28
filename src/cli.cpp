#include "cli.hpp"

#include "common/string_utils.hpp"
#include "common/json.hpp"
#include "gdb/gdb_session.hpp"
#include "gdb/mi_utils.hpp"
#include "report/report.hpp"
#include "task/debug_task.hpp"
#include "workflow/crash_workflow.hpp"

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
        std::vector<std::string> on_hit_actions;
    };

    struct HypothesisCheck {
        std::string id;
        std::string description;
        std::string expression;
        std::string assertion;
        std::string expected;
        std::string result;
        std::string evidence_id;
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
    std::map<std::string, std::vector<std::string>> on_hit_actions_by_breakpoint;
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
                               std::ostream &out);
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
        out << "      \"on_hit\": " << json_action_array(probe.on_hit_actions) << "\n";
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
    out << "\"on_hit\":" << json_action_array(probe.on_hit_actions);
    out << "}";
    return out.str();
}

static std::string probe_array_json(const ProbeState &probe_state) {
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto &[_, probe] : probe_state.probes_by_number) {
        if (!first) {
            out << ",";
        }
        first = false;
        out << probe_info_json(probe);
    }
    out << "]";
    return out.str();
}

static std::vector<std::string> on_hit_actions_from(const Json &action) {
    std::vector<std::string> actions;
    const Json *on_hit = action.find("on_hit");
    if (on_hit == nullptr) {
        const Json *params = action.find("params");
        if (params != nullptr) {
            on_hit = params->find("on_hit");
        }
    }
    if (on_hit == nullptr || !on_hit->is_array()) {
        return actions;
    }
    for (const auto &item : on_hit->array_value) {
        if (item.is_object()) {
            actions.push_back(dump_json(item));
        }
    }
    return actions;
}

static void run_on_hit_actions(GdbSession &session,
                               const DebugTask *task,
                               SessionOutcome *outcome,
                               ProbeState &probe_state,
                               const CommandResult &result,
                               std::ostream &out) {
    if (result.breakpoint_number.empty()) {
        return;
    }
    auto it = probe_state.on_hit_actions_by_breakpoint.find(result.breakpoint_number);
    if (it == probe_state.on_hit_actions_by_breakpoint.end()) {
        return;
    }
    bool ignored_finish = false;
    for (const auto &action : it->second) {
        handle_action_line(session, task, outcome, probe_state, action, ignored_finish, out);
        ignored_finish = false;
    }
}

static void record_probe_hit(GdbSession &session,
                             ProbeState &probe_state,
                             const CommandResult &result) {
    if (result.breakpoint_number.empty()) {
        return;
    }

    auto it = probe_state.probes_by_number.find(result.breakpoint_number);
    std::string kind = result.stop_reason == "watchpoint-trigger" ? "watchpoint" : "breakpoint";
    if (it != probe_state.probes_by_number.end() && !it->second.kind.empty()) {
        kind = it->second.kind;
    }

    ProbeState::ProbeInfo *probe = nullptr;
    if (it != probe_state.probes_by_number.end()) {
        probe = &it->second;
        ++probe->hit_count;
        probe->last_stop_reason = result.stop_reason;
    }

    std::ostringstream text;
    text << "{\n";
    text << "  \"number\": " << json_escape(result.breakpoint_number) << ",\n";
    text << "  \"kind\": " << json_escape(kind) << ",\n";
    text << "  \"stop_reason\": " << json_escape(result.stop_reason) << ",\n";
    text << "  \"signal\": " << json_escape(result.signal_name);
    if (probe != nullptr) {
        text << ",\n";
        text << "  \"location\": " << json_escape(probe->location) << ",\n";
        text << "  \"expression\": " << json_escape(probe->expression) << ",\n";
        text << "  \"event\": " << json_escape(probe->event) << ",\n";
        text << "  \"condition\": " << json_escape(probe->condition) << ",\n";
        text << "  \"comment\": " << json_escape(probe->comment) << ",\n";
        text << "  \"purpose\": " << json_escape(probe->purpose) << ",\n";
        text << "  \"hit_count\": " << probe->hit_count;
    }
    text << "\n}\n";

    std::string evidence_kind = "BreakpointHit";
    if (kind == "watchpoint") {
        evidence_kind = "WatchpointHit";
    } else if (kind == "catchpoint") {
        evidence_kind = "CatchpointHit";
    }
    session.evidence_store().add_text(evidence_kind,
                                      evidence_kind + " " + result.breakpoint_number,
                                      result.stop_reason,
                                      text.str());
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
            out << "          \"description\": " << json_escape(check.description) << ",\n";
            out << "          \"expression\": " << json_escape(check.expression) << ",\n";
            out << "          \"assertion\": " << json_escape(check.assertion) << ",\n";
            out << "          \"expected\": " << json_escape(check.expected) << ",\n";
            out << "          \"result\": " << json_escape(check.result) << ",\n";
            out << "          \"evidence\": " << json_escape(check.evidence_id) << "\n";
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

static std::string replay_action_display_name(const Json &action, int index) {
    std::string name = action.string_or("name");
    if (!name.empty()) {
        return name;
    }
    name = action.string_or("action");
    if (!name.empty()) {
        return name;
    }
    std::ostringstream fallback;
    fallback << "step-" << index;
    return fallback.str();
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

static void write_replay_plan(const fs::path &path,
                              const std::string &name,
                              const std::vector<std::string> &actions) {
    std::ostringstream plan;
    plan << "{\n";
    plan << "  \"schema\": \"gdb-agent-replay-plan-v1\",\n";
    plan << "  \"id\": " << json_escape("replay-" + slugify(name)) << ",\n";
    plan << "  \"name\": " << json_escape(name) << ",\n";
    plan << "  \"actions\": [\n";
    for (size_t i = 0; i < actions.size(); ++i) {
        Json action = parse_json(actions[i]);
        int index = static_cast<int>(i + 1);
        std::ostringstream id;
        id << 'a' << index;
        plan << "    {\n";
        plan << "      \"id\": " << json_escape(id.str()) << ",\n";
        plan << "      \"name\": " << json_escape(replay_action_display_name(action, index)) << ",\n";
        plan << "      \"enabled\": true,\n";
        plan << "      \"tags\": [],\n";
        plan << "      \"action\": " << dump_json(action) << "\n";
        plan << "    }";
        if (i + 1 != actions.size()) {
            plan << ",";
        }
        plan << "\n";
    }
    plan << "  ]\n";
    plan << "}\n";
    write_text_file(path, plan.str());
}

static void rebuild_replay_plan_from_jsonl(const fs::path &jsonl_file,
                                           const fs::path &plan_file,
                                           const std::string &name) {
    write_replay_plan(plan_file, name, load_replay_jsonl_actions(jsonl_file));
}

static void append_text(const fs::path &path, const std::string &text) {
    std::ofstream out(path, std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to append file: " + path.string());
    }
    out << text;
}

static bool assertion_passed(const std::string &assertion,
                             const std::string &summary,
                             const std::string &expected) {
    if (assertion.empty() || assertion == "none") {
        return true;
    }
    if (assertion == "contains") {
        return summary.find(expected) != std::string::npos;
    }
    if (assertion == "not_contains") {
        return summary.find(expected) == std::string::npos;
    }
    if (assertion == "is_null") {
        return summary.find("0x0") != std::string::npos || summary.find("= 0") != std::string::npos;
    }
    if (assertion == "non_null") {
        return summary.find("0x0") == std::string::npos && summary.find("= 0") == std::string::npos;
    }
    return false;
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
        out << "{\"ok\":false,\"error\":" << json_escape(std::string("invalid json: ") + ex.what()) << "}\n";
        return;
    }
    std::string action_name = action.string_or("action");
    if (action_name.empty()) {
        out << "{\"ok\":false,\"error\":\"missing action\"}\n";
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
        auto ev = collect_console(session, "Backtrace", "bt", true);
        out << "{\"ok\":true,\"action\":\"backtrace\",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "locals") {
        auto ev = collect_console(session, "Local variables", "info locals");
        out << "{\"ok\":true,\"action\":\"locals\",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "args_info") {
        auto ev = collect_console(session, "Frame arguments", "info args");
        out << "{\"ok\":true,\"action\":\"args_info\",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "registers") {
        auto ev = collect_console(session, "Registers", "info registers");
        out << "{\"ok\":true,\"action\":\"registers\",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "threads") {
        auto ev = collect_console(session, "Threads", "info threads");
        out << "{\"ok\":true,\"action\":\"threads\",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "frame_select") {
        int frame = json_int_field(action, "frame", 0);
        auto ev = collect_console(session, "Frame select", "frame " + std::to_string(frame));
        out << "{\"ok\":true,\"action\":\"frame_select\",\"frame\":" << frame
                  << ",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "evaluate") {
        std::string expr = json_string_field(action, "expression");
        if (expr.empty()) {
            out << "{\"ok\":false,\"error\":\"missing expression\"}\n";
            return;
        }
        auto ev = collect_console(session, "Evaluate", "p " + expr);
        out << "{\"ok\":true,\"action\":\"evaluate\",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "breakpoint_set") {
        std::string location = json_string_field(action, "location");
        if (location.empty()) {
            out << "{\"ok\":false,\"error\":\"missing location\"}\n";
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

        auto on_hit = on_hit_actions_from(action);
        if (!number.empty() && !on_hit.empty()) {
            probe_state.on_hit_actions_by_breakpoint[number] = std::move(on_hit);
        }
        if (!number.empty()) {
            ProbeState::ProbeInfo probe;
            probe.number = number;
            probe.kind = "breakpoint";
            probe.location = location;
            probe.condition = condition;
            probe.comment = json_string_field(action, "comment");
            probe.purpose = json_string_field(action, "purpose");
            auto stored_on_hit = on_hit_actions_from(action);
            probe.on_hit_actions = std::move(stored_on_hit);
            probe_state.probes_by_number[number] = std::move(probe);
            if (!probe_state.probes_by_number[number].on_hit_actions.empty()) {
                probe_state.on_hit_actions_by_breakpoint[number] = probe_state.probes_by_number[number].on_hit_actions;
            }
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
            out << "{\"ok\":false,\"error\":\"missing expression\"}\n";
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
        probe.on_hit_actions = on_hit_actions_from(action);
        probe_state.probes_by_number[number] = std::move(probe);
        if (!probe_state.probes_by_number[number].on_hit_actions.empty()) {
            probe_state.on_hit_actions_by_breakpoint[number] = probe_state.probes_by_number[number].on_hit_actions;
        }
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
        probe.on_hit_actions = on_hit_actions_from(action);
        probe_state.probes_by_number[number] = std::move(probe);
        if (!probe_state.probes_by_number[number].on_hit_actions.empty()) {
            probe_state.on_hit_actions_by_breakpoint[number] = probe_state.probes_by_number[number].on_hit_actions;
        }
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
            out << "{\"ok\":false,\"error\":\"missing probe number\"}\n";
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
        record_probe_hit(session, probe_state, result);
        if (outcome != nullptr) {
            collect_stop_followup(session, *outcome, result);
        }
        run_on_hit_actions(session, task, outcome, probe_state, result, out);
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
        record_probe_hit(session, probe_state, result);
        if (outcome != nullptr) {
            collect_stop_followup(session, *outcome, result);
        }
        run_on_hit_actions(session, task, outcome, probe_state, result, out);
        out << "{\"ok\":true,\"action\":\"continue\",\"stop_reason\":" << json_escape(result.stop_reason)
                  << ",\"signal\":" << json_escape(result.signal_name)
                  << ",\"evidence\":" << json_escape(ev.id) << "}\n";
        return;
    }
    if (action_name == "raw_mi") {
        std::string command = json_string_field(action, "command");
        std::string risk = json_string_field(action, "risk");
        if (command.empty()) {
            out << "{\"ok\":false,\"error\":\"missing command\"}\n";
            return;
        }
        if (risk != "advanced") {
            out << "{\"ok\":false,\"error\":\"raw_mi requires risk=advanced\"}\n";
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
            out << "{\"ok\":false,\"error\":\"hypothesis_check requires hypothesis and expression\"}\n";
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
        bool passed = assertion_passed(assertion, ev.summary, expected);
        fs::path file = hypothesis_file_for(session, id);

        std::ostringstream md;
        md << "## Check: " << description << "\n\n";
        md << "- Expression: `" << expression << "`\n";
        md << "- Evidence: `" << ev.id << "`\n";
        md << "- Assertion: `" << assertion << "`\n";
        if (!expected.empty()) {
            md << "- Expected: `" << expected << "`\n";
        }
        md << "- Result: `" << (passed ? "passed" : "failed") << "`\n\n";
        md << "```text\n" << ev.summary << "\n```\n\n";
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
        check.result = passed ? "passed" : "failed";
        check.evidence_id = ev.id;
        record.checks.push_back(std::move(check));
        record.tool_status = passed ? "EvidenceSupportsCheck" : "EvidenceContradictsCheck";
        write_hypothesis_index(session, probe_state);

        out << "{\"ok\":true,\"action\":\"hypothesis_check\",\"hypothesis\":" << json_escape(id)
                  << ",\"assertion\":" << json_escape(passed ? "passed" : "failed")
                  << ",\"evidence\":" << json_escape(ev.id) << "}\n";
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
            out << "{\"ok\":false,\"error\":\"hypothesis_conclude requires hypothesis\"}\n";
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
        const Json *saved = json_field(action, "saved_action");
        std::string saved_action;
        if (saved != nullptr && saved->is_string()) {
            saved_action = saved->string_value;
        } else if (saved != nullptr && saved->is_object()) {
            saved_action = dump_json(*saved);
        }
        if (name.empty() || saved_action.empty()) {
            out << "{\"ok\":false,\"error\":\"save_action requires name and saved_action\"}\n";
            return;
        }

        fs::path replay_dir = session.assets_dir() / "replay";
        fs::create_directories(replay_dir);
        std::string replay_base = slugify(name);
        fs::path replay_file = replay_dir / (replay_base + ".jsonl");
        fs::path replay_plan = replay_dir / (replay_base + ".json");
        std::ofstream replay_out(replay_file, std::ios::app);
        if (!replay_out) {
            out << "{\"ok\":false,\"error\":\"failed to write replay file\"}\n";
            return;
        }
        replay_out << saved_action << '\n';
        replay_out.close();
        try {
            rebuild_replay_plan_from_jsonl(replay_file, replay_plan, name);
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
                  << json_escape(replay_plan.lexically_normal().string()) << "}\n";
        return;
    }
    if (action_name == "replay") {
        std::string file = json_string_field(action, "file");
        std::string name = json_string_field(action, "name");
        fs::path replay_file;
        if (!file.empty()) {
            replay_file = file;
        } else if (!name.empty()) {
            fs::path replay_dir = session.assets_dir() / "replay";
            fs::path plan = replay_dir / (slugify(name) + ".json");
            fs::path jsonl = replay_dir / (slugify(name) + ".jsonl");
            replay_file = fs::exists(plan) ? plan : jsonl;
        } else {
            out << "{\"ok\":false,\"error\":\"replay requires file or name\"}\n";
            return;
        }
        replay_action_file(session, task, outcome, probe_state, replay_file, out);
        out << "{\"ok\":true,\"action\":\"replay\",\"file\":"
                  << json_escape(replay_file.lexically_normal().string()) << "}\n";
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

static void replay_action_text(GdbSession &session,
                               const DebugTask *task,
                               SessionOutcome *outcome,
                               ProbeState &probe_state,
                               const std::string &plan_name,
                               const std::string &step_id,
                               int index,
                               const std::string &line,
                               std::ostream &out) {
    bool ignored_finish = false;
    std::ostringstream step_output;
    try {
        handle_action_line(session, task, outcome, probe_state, line, ignored_finish, step_output);
        ignored_finish = false;
        std::ostringstream evidence_text;
        evidence_text << "{\n";
        evidence_text << "  \"plan\": " << json_escape(plan_name) << ",\n";
        evidence_text << "  \"step_id\": " << json_escape(step_id) << ",\n";
        evidence_text << "  \"index\": " << index << ",\n";
        evidence_text << "  \"action_json\": " << json_escape(line) << ",\n";
        evidence_text << "  \"response\": " << json_escape(step_output.str()) << "\n";
        evidence_text << "}\n";
        session.evidence_store().add_text("ReplayStep", "Replay step " + step_id, line, evidence_text.str());
        out << step_output.str();
    } catch (const std::exception &ex) {
        std::ostringstream evidence_text;
        evidence_text << "{\n";
        evidence_text << "  \"plan\": " << json_escape(plan_name) << ",\n";
        evidence_text << "  \"step_id\": " << json_escape(step_id) << ",\n";
        evidence_text << "  \"index\": " << index << ",\n";
        evidence_text << "  \"action\": " << json_escape(line) << ",\n";
        evidence_text << "  \"error\": " << json_escape(ex.what()) << "\n";
        evidence_text << "}\n";
        auto ev = session.evidence_store().add_text("ToolError", "Replay step failed " + step_id, line, evidence_text.str());
        out << "{\"ok\":false,\"action\":\"replay_step\",\"step_id\":" << json_escape(step_id)
            << ",\"error\":" << json_escape(ex.what()) << ",\"evidence\":" << json_escape(ev.id) << "}\n";
    }
}

static void replay_json_plan(GdbSession &session,
                             const DebugTask *task,
                             SessionOutcome *outcome,
                             ProbeState &probe_state,
                             const fs::path &path,
                             const Json &plan,
                             std::ostream &out) {
    std::string plan_name = plan.string_or("name", path.stem().string());
    const Json *actions = plan.find("actions");
    if (actions == nullptr || !actions->is_array()) {
        throw std::runtime_error("replay plan missing actions array: " + path.string());
    }

    int index = 0;
    for (const auto &step : actions->array_value) {
        ++index;
        if (!step.is_object()) {
            continue;
        }
        if (!step.bool_or("enabled", true)) {
            continue;
        }
        std::string step_id = step.string_or("id");
        if (step_id.empty()) {
            std::ostringstream fallback;
            fallback << 'a' << index;
            step_id = fallback.str();
        }
        const Json *action = step.find("action");
        if (action == nullptr || !action->is_object()) {
            std::ostringstream evidence_text;
            evidence_text << "Replay step " << step_id << " has no action object.\n";
            auto ev = session.evidence_store().add_text("ToolError", "Replay step missing action " + step_id, plan_name, evidence_text.str());
            out << "{\"ok\":false,\"action\":\"replay_step\",\"step_id\":" << json_escape(step_id)
                << ",\"error\":\"missing action object\",\"evidence\":" << json_escape(ev.id) << "}\n";
            continue;
        }
        replay_action_text(session, task, outcome, probe_state, plan_name, step_id, index, dump_json(*action), out);
    }
}

static void replay_action_file(GdbSession &session,
                               const DebugTask *task,
                               SessionOutcome *outcome,
                               ProbeState &probe_state,
                               const fs::path &path,
                               std::ostream &out) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open replay file: " + path.string());
    }

    std::ostringstream content_stream;
    content_stream << in.rdbuf();
    std::string content = trim(content_stream.str());
    if (content.empty()) {
        return;
    }

    if (!content.empty() && content.front() == '{') {
        Json plan = parse_json(content);
        const Json *actions = plan.find("actions");
        if (actions != nullptr && actions->is_array()) {
            replay_json_plan(session, task, outcome, probe_state, path, plan, out);
        } else {
            replay_action_text(session, task, outcome, probe_state, path.stem().string(), "a1", 1, dump_json(plan), out);
        }
        return;
    }

    std::istringstream lines(content);
    std::string line;
    int index = 0;
    std::string plan_name = path.stem().string();
    while (std::getline(lines, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        ++index;
        std::ostringstream step_id;
        step_id << 'a' << index;
        replay_action_text(session, task, outcome, probe_state, plan_name, step_id.str(), index, line, out);
    }
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
    summary << "{\n";
    summary << "  \"session_id\": " << json_escape(opts.session_id) << ",\n";
    summary << "  \"state\": " << json_escape(std::string(session_state_name(outcome.state))) << ",\n";
    summary << "  \"mode\": " << json_escape(outcome.core_mode ? "core" : "run") << ",\n";
    summary << "  \"stop_reason\": " << json_escape(outcome.stop_reason) << ",\n";
    summary << "  \"signal\": " << json_escape(outcome.signal_name) << ",\n";
    summary << "  \"segfault\": " << (outcome.segfault ? "true" : "false") << ",\n";
    summary << "  \"run_timed_out\": " << (outcome.run_timed_out ? "true" : "false") << ",\n";
    summary << "  \"run_timeout_ms\": " << outcome.run_timeout_ms << ",\n";
    summary << "  \"stdin\": " << json_escape(task.stdin_path.string()) << ",\n";
    summary << "  \"stdout\": " << json_escape(outcome.inferior_stdout) << ",\n";
    summary << "  \"stderr\": " << json_escape(outcome.inferior_stderr) << ",\n";
    summary << "  \"evidence_count\": " << session.evidence_store().all().size() << "\n";
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

static std::string read_text_file_arg(const std::string &arg) {
    fs::path path(arg);
    if (!fs::exists(path)) {
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
        record_probe_hit(*live.session, live.probe_state, run);
        collect_stop_followup(*live.session, live.outcome, run);
        run_on_hit_actions(*live.session, &live.task, &live.outcome, live.probe_state, run, replay_output);
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
            throw std::runtime_error("usage: gdb-agent save-action SESSION_ID JSON_OR_FILE --name NAME [--socket path]");
        }
        std::string session_id = argv[2];
        Json saved_action = parse_json(read_text_file_arg(argv[3]));
        std::string name = option_value(argc, argv, "--name");
        if (name.empty()) {
            throw std::runtime_error("save-action requires --name");
        }
        request << "{"
                << "\"op\":\"action\","
                << "\"session_id\":" << json_escape(session_id) << ","
                << "\"payload\":{"
                << "\"action\":\"save_action\","
                << "\"name\":" << json_escape(name) << ","
                << "\"saved_action\":" << dump_json(saved_action)
                << "}}";
    } else if (cmd == "replay") {
        if (argc < 4) {
            throw std::runtime_error("usage: gdb-agent replay SESSION_ID NAME [--socket path] or gdb-agent replay SESSION_ID --file PATH [--socket path]");
        }
        std::string session_id = argv[2];
        std::string file = option_value(argc, argv, "--file");
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
            record_probe_hit(session, probe_state, run);
            collect_stop_followup(session, outcome, run);
            run_on_hit_actions(session, &task, &outcome, probe_state, run, std::cout);
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
