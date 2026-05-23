#include "cli.hpp"

#include "common/string_utils.hpp"
#include "gdb/gdb_session.hpp"
#include "gdb/mi_utils.hpp"
#include "report/report.hpp"
#include "task/debug_task.hpp"
#include "workflow/crash_workflow.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <cctype>
#include <stdexcept>
#include <string>

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

static std::string extract_json_string(const std::string &line, const std::string &key) {
    std::string needle = "\"" + key + "\"";
    auto pos = line.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    pos = line.find(':', pos);
    if (pos == std::string::npos) {
        return {};
    }
    pos = line.find('"', pos);
    if (pos == std::string::npos) {
        return {};
    }
    ++pos;
    std::string out;
    bool escaped = false;
    for (; pos < line.size(); ++pos) {
        char c = line[pos];
        if (escaped) {
            out.push_back(c);
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
        out.push_back(c);
    }
    return out;
}

static int extract_json_int(const std::string &line, const std::string &key, int default_value) {
    std::string needle = "\"" + key + "\"";
    auto pos = line.find(needle);
    if (pos == std::string::npos) {
        return default_value;
    }
    pos = line.find(':', pos);
    if (pos == std::string::npos) {
        return default_value;
    }
    ++pos;
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    bool negative = false;
    if (pos < line.size() && line[pos] == '-') {
        negative = true;
        ++pos;
    }
    int value = 0;
    bool found = false;
    while (pos < line.size() && std::isdigit(static_cast<unsigned char>(line[pos]))) {
        found = true;
        value = value * 10 + (line[pos] - '0');
        ++pos;
    }
    if (!found) {
        return default_value;
    }
    return negative ? -value : value;
}

static bool contains_action(const std::string &line, const std::string &action) {
    return extract_json_string(line, "action") == action;
}

static void handle_action_line(GdbSession &session, SessionOutcome *outcome, const std::string &line, bool &finished);
static void replay_action_file(GdbSession &session, const fs::path &path);

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

static void handle_action_line(GdbSession &session, SessionOutcome *outcome, const std::string &line, bool &finished) {
    if (trim(line).empty()) {
        return;
    }
    if (contains_action(line, "finish_session") || contains_action(line, "finish")) {
        finished = true;
        return;
    }
    if (contains_action(line, "backtrace")) {
        auto ev = collect_console(session, "Backtrace", "bt", true);
        std::cout << "{\"ok\":true,\"action\":\"backtrace\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (contains_action(line, "locals")) {
        auto ev = collect_console(session, "Local variables", "info locals");
        std::cout << "{\"ok\":true,\"action\":\"locals\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (contains_action(line, "registers")) {
        auto ev = collect_console(session, "Registers", "info registers");
        std::cout << "{\"ok\":true,\"action\":\"registers\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (contains_action(line, "threads")) {
        auto ev = collect_console(session, "Threads", "info threads");
        std::cout << "{\"ok\":true,\"action\":\"threads\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (contains_action(line, "frame_select")) {
        int frame = extract_json_int(line, "frame", 0);
        auto ev = collect_console(session, "Frame select", "frame " + std::to_string(frame));
        std::cout << "{\"ok\":true,\"action\":\"frame_select\",\"frame\":" << frame
                  << ",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (contains_action(line, "evaluate")) {
        std::string expr = extract_json_string(line, "expression");
        if (expr.empty()) {
            std::cout << "{\"ok\":false,\"error\":\"missing expression\"}\n";
            return;
        }
        auto ev = collect_console(session, "Evaluate", "p " + expr);
        std::cout << "{\"ok\":true,\"action\":\"evaluate\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (contains_action(line, "breakpoint_set")) {
        std::string location = extract_json_string(line, "location");
        if (location.empty()) {
            std::cout << "{\"ok\":false,\"error\":\"missing location\"}\n";
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

        std::string condition = extract_json_string(line, "condition");
        std::string condition_evidence;
        if (!condition.empty() && !number.empty()) {
            auto cond = session.command("-break-condition " + number + " " + condition);
            auto ev = session.evidence_store().add("GdbCommand", "Breakpoint condition", cond.command, cond.raw_lines);
            condition_evidence = ev.id;
        }

        std::cout << "{\"ok\":true,\"action\":\"breakpoint_set\",\"breakpoint\":\"" << number << "\"";
        if (!condition_evidence.empty()) {
            std::cout << ",\"condition_evidence\":\"" << condition_evidence << "\"";
        }
        std::cout << "}\n";
        return;
    }
    if (contains_action(line, "watchpoint_set")) {
        std::string expression = extract_json_string(line, "expression");
        if (expression.empty()) {
            std::cout << "{\"ok\":false,\"error\":\"missing expression\"}\n";
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
        std::cout << "{\"ok\":true,\"action\":\"watchpoint_set\",\"watchpoint\":\"" << number
                  << "\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (contains_action(line, "probe_delete") ||
        contains_action(line, "probe_enable") ||
        contains_action(line, "probe_disable")) {
        int number = extract_json_int(line, "number", -1);
        if (number < 0) {
            std::cout << "{\"ok\":false,\"error\":\"missing probe number\"}\n";
            return;
        }

        std::string action = "probe_delete";
        std::string command = "-break-delete ";
        std::string title = "Probe delete";
        if (contains_action(line, "probe_enable")) {
            action = "probe_enable";
            command = "-break-enable ";
            title = "Probe enable";
        } else if (contains_action(line, "probe_disable")) {
            action = "probe_disable";
            command = "-break-disable ";
            title = "Probe disable";
        }

        auto result = session.command(command + std::to_string(number));
        auto ev = session.evidence_store().add("GdbCommand", title, result.command, result.raw_lines);
        std::cout << "{\"ok\":true,\"action\":\"" << action << "\",\"number\":" << number
                  << ",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (contains_action(line, "continue")) {
        int deadline_ms = extract_json_int(line, "deadline_ms", 30000);
        auto result = session.exec_control("-exec-continue", std::chrono::milliseconds(deadline_ms));
        auto ev = session.evidence_store().add("StopEvent", "Continue stop", result.command, result.raw_lines);
        if (outcome != nullptr) {
            update_outcome_from_stop(*outcome, result);
        }
        collect_stop_followup(session, result);
        std::cout << "{\"ok\":true,\"action\":\"continue\",\"stop_reason\":\"" << result.stop_reason
                  << "\",\"signal\":\"" << result.signal_name
                  << "\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (contains_action(line, "save_action")) {
        std::string name = extract_json_string(line, "name");
        std::string saved_action = extract_json_string(line, "saved_action");
        if (name.empty() || saved_action.empty()) {
            std::cout << "{\"ok\":false,\"error\":\"save_action requires name and saved_action\"}\n";
            return;
        }

        fs::path replay_dir = session.assets_dir() / "replay";
        fs::create_directories(replay_dir);
        fs::path replay_file = replay_dir / (slugify(name) + ".jsonl");
        std::ofstream out(replay_file, std::ios::app);
        if (!out) {
            std::cout << "{\"ok\":false,\"error\":\"failed to write replay file\"}\n";
            return;
        }
        out << saved_action << '\n';
        std::cout << "{\"ok\":true,\"action\":\"save_action\",\"file\":\""
                  << replay_file.lexically_normal().string() << "\"}\n";
        return;
    }
    if (contains_action(line, "replay")) {
        std::string file = extract_json_string(line, "file");
        std::string name = extract_json_string(line, "name");
        fs::path replay_file;
        if (!file.empty()) {
            replay_file = file;
        } else if (!name.empty()) {
            replay_file = session.assets_dir() / "replay" / (slugify(name) + ".jsonl");
        } else {
            std::cout << "{\"ok\":false,\"error\":\"replay requires file or name\"}\n";
            return;
        }
        replay_action_file(session, replay_file);
        std::cout << "{\"ok\":true,\"action\":\"replay\",\"file\":\""
                  << replay_file.lexically_normal().string() << "\"}\n";
        return;
    }
    std::cout << "{\"ok\":false,\"error\":\"unsupported action\"}\n";
}

static void replay_action_file(GdbSession &session, const fs::path &path) {
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
        handle_action_line(session, nullptr, line, ignored_finish);
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

static int run_serve(const CliOptions &opts, const DebugTask &task) {
    validate_task(task);
    fs::create_directories(opts.assets);

    GdbSession session(opts.assets, task.working_directory);
    SessionOutcome outcome;

    try {
        session.start();
        session.initialize(task);

        if (!opts.replay_before_run.empty()) {
            replay_action_file(session, opts.replay_before_run);
        }

        if (task.core_dump) {
            outcome.core_mode = true;
            auto load = session.load_core(task);
            session.evidence_store().add("SessionEvent", "Core load", load.command, load.raw_lines);
            outcome.stop_reason = "core_loaded";
            collect_light_evidence(session);
        } else {
            auto run = session.exec_control("-exec-run", std::chrono::milliseconds(opts.run_timeout_ms));
            session.evidence_store().add("StopEvent", "Initial run stop", run.command, run.raw_lines);
            outcome.stop_reason = run.stop_reason.empty() ? "unknown" : run.stop_reason;
            outcome.signal_name = run.signal_name;
            outcome.segfault = run.signal_name == "SIGSEGV";
            outcome.run_timed_out = run.timed_out;
            collect_stop_followup(session, run);
        }

        std::cout << "{\"ok\":true,\"session_id\":\"" << opts.session_id
                  << "\",\"state\":\"stopped\",\"stop_reason\":\"" << outcome.stop_reason
                  << "\",\"signal\":\"" << outcome.signal_name << "\"}\n";

        bool finished = false;
        std::string line;
        while (!finished && std::getline(std::cin, line)) {
            handle_action_line(session, &outcome, line, finished);
        }

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
