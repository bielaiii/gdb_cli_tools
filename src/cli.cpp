#include "cli.hpp"

#include "common/string_utils.hpp"
#include "gdb/gdb_session.hpp"
#include "report/report.hpp"
#include "task/debug_task.hpp"
#include "workflow/crash_workflow.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

struct CliOptions {
    std::string command;
    fs::path task_file;
    fs::path assets = "report.assets";
    fs::path report = "report.md";
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

static bool contains_action(const std::string &line, const std::string &action) {
    return line.find("\"action\"") != std::string::npos && line.find("\"" + action + "\"") != std::string::npos;
}

static void handle_action_line(GdbSession &session, const std::string &line, bool &finished) {
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
    std::cout << "{\"ok\":false,\"error\":\"unsupported action\"}\n";
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
            if (outcome.segfault || outcome.run_timed_out || outcome.stop_reason == "interrupted_by_tool_deadline") {
                collect_light_evidence(session);
            }
        }

        std::cout << "{\"ok\":true,\"session_id\":\"" << opts.session_id
                  << "\",\"state\":\"stopped\",\"stop_reason\":\"" << outcome.stop_reason
                  << "\",\"signal\":\"" << outcome.signal_name << "\"}\n";

        bool finished = false;
        std::string line;
        while (!finished && std::getline(std::cin, line)) {
            handle_action_line(session, line, finished);
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

