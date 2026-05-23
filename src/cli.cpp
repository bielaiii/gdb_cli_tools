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
    int run_timeout_ms = 30000;
};

struct ProbeState {
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
                               SessionOutcome *outcome,
                               ProbeState &probe_state,
                               const std::string &line,
                               bool &finished,
                               std::ostream &out);
static void replay_action_file(GdbSession &session, ProbeState &probe_state, const fs::path &path, std::ostream &out);
static std::string json_escape(const std::string &s);

static void collect_stop_followup(GdbSession &session, const CommandResult &result) {
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
    session.evidence_store().add(kind, title, result.command, result.raw_lines);
}

static void update_outcome_from_stop(SessionOutcome &outcome, const CommandResult &result) {
    outcome.stop_reason = result.stop_reason.empty() ? outcome.stop_reason : result.stop_reason;
    outcome.signal_name = result.signal_name.empty() ? outcome.signal_name : result.signal_name;
    outcome.segfault = outcome.segfault || result.signal_name == "SIGSEGV";
    outcome.run_timed_out = outcome.run_timed_out || result.timed_out;
}

static std::string json_string_field(const Json &action, const std::string &key) {
    return action.string_or(key);
}

static int json_int_field(const Json &action, const std::string &key, int fallback) {
    return action.int_or(key, fallback);
}

static std::vector<std::string> on_hit_actions_from(const Json &action) {
    std::vector<std::string> actions;
    const Json *on_hit = action.find("on_hit");
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
        handle_action_line(session, nullptr, probe_state, action, ignored_finish, out);
        ignored_finish = false;
    }
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

    if (action_name == "finish_session" || action_name == "finish") {
        finished = true;
        return;
    }
    if (action_name == "backtrace") {
        auto ev = collect_console(session, "Backtrace", "bt", true);
        out << "{\"ok\":true,\"action\":\"backtrace\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (action_name == "locals") {
        auto ev = collect_console(session, "Local variables", "info locals");
        out << "{\"ok\":true,\"action\":\"locals\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (action_name == "registers") {
        auto ev = collect_console(session, "Registers", "info registers");
        out << "{\"ok\":true,\"action\":\"registers\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (action_name == "threads") {
        auto ev = collect_console(session, "Threads", "info threads");
        out << "{\"ok\":true,\"action\":\"threads\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (action_name == "frame_select") {
        int frame = json_int_field(action, "frame", 0);
        auto ev = collect_console(session, "Frame select", "frame " + std::to_string(frame));
        out << "{\"ok\":true,\"action\":\"frame_select\",\"frame\":" << frame
                  << ",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (action_name == "evaluate") {
        std::string expr = json_string_field(action, "expression");
        if (expr.empty()) {
            out << "{\"ok\":false,\"error\":\"missing expression\"}\n";
            return;
        }
        auto ev = collect_console(session, "Evaluate", "p " + expr);
        out << "{\"ok\":true,\"action\":\"evaluate\",\"evidence\":\"" << ev.id << "\"}\n";
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
        std::string number;
        for (const auto &raw : insert.raw_lines) {
            number = field_value(raw, "number");
            if (!number.empty()) {
                break;
            }
        }

        std::string condition = json_string_field(action, "condition");
        std::string condition_evidence;
        if (!condition.empty() && !number.empty()) {
            auto cond = session.command("-break-condition " + number + " " + condition);
            auto ev = session.evidence_store().add("GdbCommand", "Breakpoint condition", cond.command, cond.raw_lines);
            condition_evidence = ev.id;
        }

        auto on_hit = on_hit_actions_from(action);
        if (!number.empty() && !on_hit.empty()) {
            probe_state.on_hit_actions_by_breakpoint[number] = std::move(on_hit);
        }

        out << "{\"ok\":true,\"action\":\"breakpoint_set\",\"breakpoint\":\"" << number << "\"";
        if (!condition_evidence.empty()) {
            out << ",\"condition_evidence\":\"" << condition_evidence << "\"";
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
        auto ev = session.evidence_store().add("GdbCommand", "Watchpoint set", result.command, result.raw_lines);
        std::string number;
        for (const auto &raw : result.raw_lines) {
            number = field_value(raw, "number");
            if (!number.empty()) {
                break;
            }
        }
        out << "{\"ok\":true,\"action\":\"watchpoint_set\",\"watchpoint\":\"" << number
                  << "\",\"evidence\":\"" << ev.id << "\"}\n";
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
        auto ev = session.evidence_store().add("GdbCommand", title, result.command, result.raw_lines);
        out << "{\"ok\":true,\"action\":\"" << action << "\",\"number\":" << number
                  << ",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (action_name == "continue") {
        int deadline_ms = json_int_field(action, "deadline_ms", 30000);
        auto result = session.exec_control("-exec-continue", std::chrono::milliseconds(deadline_ms));
        auto ev = session.evidence_store().add("StopEvent", "Continue stop", result.command, result.raw_lines);
        if (outcome != nullptr) {
            update_outcome_from_stop(*outcome, result);
        }
        collect_stop_followup(session, result);
        run_on_hit_actions(session, probe_state, result, out);
        out << "{\"ok\":true,\"action\":\"continue\",\"stop_reason\":\"" << result.stop_reason
                  << "\",\"signal\":\"" << result.signal_name
                  << "\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (action_name == "hypothesis_create") {
        std::string id = action.string_or("id");
        if (id.empty()) {
            id = next_hypothesis_id(probe_state);
        }
        std::string title = action.string_or("title", "Untitled hypothesis");
        std::string description = action.string_or("description");
        fs::path file = hypothesis_file_for(session, id);

        std::ostringstream md;
        md << "# " << id << " " << title << "\n\n";
        md << "Status: EvidenceCollectionStarted\n\n";
        if (!description.empty()) {
            md << "## Description\n\n" << description << "\n\n";
        }
        write_text_file(file, md.str());
        out << "{\"ok\":true,\"action\":\"hypothesis_create\",\"id\":\"" << id
                  << "\",\"file\":\"" << file.lexically_normal().string() << "\"}\n";
        return;
    }
    if (action_name == "hypothesis_check") {
        std::string id = action.string_or("hypothesis");
        std::string expression = action.string_or("expression");
        if (id.empty() || expression.empty()) {
            out << "{\"ok\":false,\"error\":\"hypothesis_check requires hypothesis and expression\"}\n";
            return;
        }

        std::string description = action.string_or("description", expression);
        std::string assertion = action.string_or("assertion", "none");
        std::string expected = action.string_or("expected");

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

        out << "{\"ok\":true,\"action\":\"hypothesis_check\",\"hypothesis\":\"" << id
                  << "\",\"assertion\":\"" << (passed ? "passed" : "failed")
                  << "\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (action_name == "hypothesis_conclude") {
        std::string id = action.string_or("hypothesis");
        std::string conclusion = action.string_or("conclusion", "Inconclusive");
        std::string inference = action.string_or("inference");
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
        out << "{\"ok\":true,\"action\":\"hypothesis_conclude\",\"hypothesis\":\"" << id
                  << "\",\"conclusion\":\"" << conclusion << "\"}\n";
        return;
    }
    if (action_name == "save_action") {
        std::string name = json_string_field(action, "name");
        const Json *saved = action.find("saved_action");
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
        fs::path replay_file = replay_dir / (slugify(name) + ".jsonl");
        std::ofstream replay_out(replay_file, std::ios::app);
        if (!replay_out) {
            out << "{\"ok\":false,\"error\":\"failed to write replay file\"}\n";
            return;
        }
        replay_out << saved_action << '\n';
        out << "{\"ok\":true,\"action\":\"save_action\",\"file\":\""
                  << replay_file.lexically_normal().string() << "\"}\n";
        return;
    }
    if (action_name == "replay") {
        std::string file = json_string_field(action, "file");
        std::string name = json_string_field(action, "name");
        fs::path replay_file;
        if (!file.empty()) {
            replay_file = file;
        } else if (!name.empty()) {
            replay_file = session.assets_dir() / "replay" / (slugify(name) + ".jsonl");
        } else {
            out << "{\"ok\":false,\"error\":\"replay requires file or name\"}\n";
            return;
        }
        replay_action_file(session, probe_state, replay_file, out);
        out << "{\"ok\":true,\"action\":\"replay\",\"file\":\""
                  << replay_file.lexically_normal().string() << "\"}\n";
        return;
    }
    out << "{\"ok\":false,\"error\":\"unsupported action\"}\n";
}

static void replay_action_file(GdbSession &session, ProbeState &probe_state, const fs::path &path, std::ostream &out) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open replay file: " + path.string());
    }

    bool ignored_finish = false;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        handle_action_line(session, nullptr, probe_state, line, ignored_finish, out);
        ignored_finish = false;
    }
}

static int run_check(const DebugTask &task) {
    validate_task(task);
    std::cout << "ok\n";
    std::cout << "executable: " << shell_quote_for_report(task.executable.string()) << "\n";
    std::cout << "working directory: " << shell_quote_for_report(task.working_directory.string()) << "\n";
    std::cout << "args: " << task.args_raw << "\n";
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
    task_json << "  \"core_dump\": "
              << (task.core_dump ? json_escape(task.core_dump->string()) : std::string("null")) << "\n";
    task_json << "}\n";
    write_text_file(opts.assets / "task.normalized.json", task_json.str());

    std::ostringstream summary;
    summary << "{\n";
    summary << "  \"session_id\": " << json_escape(opts.session_id) << ",\n";
    summary << "  \"mode\": " << json_escape(outcome.core_mode ? "core" : "run") << ",\n";
    summary << "  \"stop_reason\": " << json_escape(outcome.stop_reason) << ",\n";
    summary << "  \"signal\": " << json_escape(outcome.signal_name) << ",\n";
    summary << "  \"segfault\": " << (outcome.segfault ? "true" : "false") << ",\n";
    summary << "  \"run_timed_out\": " << (outcome.run_timed_out ? "true" : "false") << ",\n";
    summary << "  \"evidence_count\": " << session.evidence_store().all().size() << "\n";
    summary << "}\n";
    write_text_file(opts.assets / "session_summary.json", summary.str());
}

static std::string option_value(int argc, char **argv, const std::string &name, const std::string &fallback = "") {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) {
            return argv[i + 1];
        }
    }
    return fallback;
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
    live.session->start();
    live.session->initialize(live.task);

    std::ostringstream replay_output;
    if (!live.opts.replay_before_run.empty()) {
        replay_action_file(*live.session, live.probe_state, live.opts.replay_before_run, replay_output);
    }

    if (live.task.core_dump) {
        live.outcome.core_mode = true;
        auto load = live.session->load_core(live.task);
        live.session->evidence_store().add("SessionEvent", "Core load", load.command, load.raw_lines);
        live.outcome.stop_reason = "core_loaded";
        collect_core_evidence(*live.session);
    } else {
        auto run = live.session->exec_control("-exec-run", std::chrono::milliseconds(live.opts.run_timeout_ms));
        live.session->evidence_store().add("StopEvent", "Initial run stop", run.command, run.raw_lines);
        live.outcome.stop_reason = run.stop_reason.empty() ? "unknown" : run.stop_reason;
        live.outcome.signal_name = run.signal_name;
        live.outcome.segfault = run.signal_name == "SIGSEGV";
        live.outcome.run_timed_out = run.timed_out;
        collect_stop_followup(*live.session, run);
        run_on_hit_actions(*live.session, live.probe_state, run, replay_output);
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

static std::string finish_live_session_response(const std::string &session_id, LiveSession &live, const std::string &report) {
    if (!report.empty()) {
        live.opts.report = report;
    }
    write_session_files(live.opts, live.task, live.outcome, *live.session);
    write_report(live.opts.report, live.opts.assets, live.task, live.outcome, live.session->evidence_store().all());
    live.session->shutdown();

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
        live.opts.run_timeout_ms = request.int_or("run_timeout_ms", 30000);
        live.task = load_task(live.opts.task_file);
        start_live_session(live);
        std::ostringstream out;
        out << "{\"ok\":true,\"session_id\":" << json_escape(session_id)
            << ",\"state\":\"stopped\",\"stop_reason\":" << json_escape(live.outcome.stop_reason)
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
        handle_action_line(*live.session, &live.outcome, live.probe_state, dump_json(*payload), finished, out);
        if (finished) {
            std::string response = finish_live_session_response(session_id, live, "");
            sessions.erase(it);
            return response;
        }
        return out.str();
    }

    if (op == "finish") {
        std::string report = request.string_or("report");
        std::string response = finish_live_session_response(session_id, live, report);
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
    std::cout << "{\"ok\":true,\"daemon\":\"listening\",\"socket\":\"" << socket_path.string() << "\"}\n";
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
        int timeout = std::stoi(option_value(argc, argv, "--run-timeout-ms", "30000"));
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
            throw std::runtime_error("usage: gdb-agent action SESSION_ID JSON [--socket path]");
        }
        std::string session_id = argv[2];
        Json payload = parse_json(argv[3]);
        request << "{"
                << "\"op\":\"action\","
                << "\"session_id\":" << json_escape(session_id) << ","
                << "\"payload\":" << dump_json(payload)
                << "}";
    } else if (cmd == "finish") {
        if (argc < 3) {
            throw std::runtime_error("usage: gdb-agent finish SESSION_ID [--socket path] [--out report.md]");
        }
        std::string session_id = argv[2];
        std::string report = option_value(argc, argv, "--out", "");
        request << "{"
                << "\"op\":\"finish\","
                << "\"session_id\":" << json_escape(session_id) << ","
                << "\"report\":" << json_escape(report)
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
    ProbeState probe_state;

    try {
        session.start();
        session.initialize(task);

        if (!opts.replay_before_run.empty()) {
            replay_action_file(session, probe_state, opts.replay_before_run, std::cout);
        }

        if (task.core_dump) {
            outcome.core_mode = true;
            auto load = session.load_core(task);
            session.evidence_store().add("SessionEvent", "Core load", load.command, load.raw_lines);
            outcome.stop_reason = "core_loaded";
            collect_core_evidence(session);
        } else {
            auto run = session.exec_control("-exec-run", std::chrono::milliseconds(opts.run_timeout_ms));
            session.evidence_store().add("StopEvent", "Initial run stop", run.command, run.raw_lines);
            outcome.stop_reason = run.stop_reason.empty() ? "unknown" : run.stop_reason;
            outcome.signal_name = run.signal_name;
            outcome.segfault = run.signal_name == "SIGSEGV";
            outcome.run_timed_out = run.timed_out;
            collect_stop_followup(session, run);
            run_on_hit_actions(session, probe_state, run, std::cout);
        }

        std::cout << "{\"ok\":true,\"session_id\":\"" << opts.session_id
                  << "\",\"state\":\"stopped\",\"stop_reason\":\"" << outcome.stop_reason
                  << "\",\"signal\":\"" << outcome.signal_name << "\"}\n";

        bool finished = false;
        std::string line;
        while (!finished && std::getline(std::cin, line)) {
            handle_action_line(session, &outcome, probe_state, line, finished, std::cout);
        }

        write_session_files(opts, task, outcome, session);
        write_report(opts.report, opts.assets, task, outcome, session.evidence_store().all());
        std::cout << "{\"ok\":true,\"report\":\"" << opts.report.string()
                  << "\",\"assets\":\"" << opts.assets.string() << "\"}\n";
        session.shutdown();
        return outcome.segfault || outcome.core_mode ? 0 : 2;
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
            if (command == "create" || command == "action" || command == "finish" ||
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
