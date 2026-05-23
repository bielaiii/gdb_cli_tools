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
#include <stdexcept>
#include <sstream>
#include <string>
#include <map>
#include <vector>

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
                               bool &finished);
static void replay_action_file(GdbSession &session, ProbeState &probe_state, const fs::path &path);

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
                               const CommandResult &result) {
    if (result.breakpoint_number.empty()) {
        return;
    }
    auto it = probe_state.on_hit_actions_by_breakpoint.find(result.breakpoint_number);
    if (it == probe_state.on_hit_actions_by_breakpoint.end()) {
        return;
    }
    bool ignored_finish = false;
    for (const auto &action : it->second) {
        handle_action_line(session, nullptr, probe_state, action, ignored_finish);
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
                               bool &finished) {
    if (trim(line).empty()) {
        return;
    }
    Json action;
    try {
        action = parse_json(line);
    } catch (const std::exception &ex) {
        std::cout << "{\"ok\":false,\"error\":\"invalid json: " << ex.what() << "\"}\n";
        return;
    }
    std::string action_name = action.string_or("action");

    if (action_name == "finish_session" || action_name == "finish") {
        finished = true;
        return;
    }
    if (action_name == "backtrace") {
        auto ev = collect_console(session, "Backtrace", "bt", true);
        std::cout << "{\"ok\":true,\"action\":\"backtrace\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (action_name == "locals") {
        auto ev = collect_console(session, "Local variables", "info locals");
        std::cout << "{\"ok\":true,\"action\":\"locals\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (action_name == "registers") {
        auto ev = collect_console(session, "Registers", "info registers");
        std::cout << "{\"ok\":true,\"action\":\"registers\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (action_name == "threads") {
        auto ev = collect_console(session, "Threads", "info threads");
        std::cout << "{\"ok\":true,\"action\":\"threads\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (action_name == "frame_select") {
        int frame = json_int_field(action, "frame", 0);
        auto ev = collect_console(session, "Frame select", "frame " + std::to_string(frame));
        std::cout << "{\"ok\":true,\"action\":\"frame_select\",\"frame\":" << frame
                  << ",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (action_name == "evaluate") {
        std::string expr = json_string_field(action, "expression");
        if (expr.empty()) {
            std::cout << "{\"ok\":false,\"error\":\"missing expression\"}\n";
            return;
        }
        auto ev = collect_console(session, "Evaluate", "p " + expr);
        std::cout << "{\"ok\":true,\"action\":\"evaluate\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (action_name == "breakpoint_set") {
        std::string location = json_string_field(action, "location");
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

        std::cout << "{\"ok\":true,\"action\":\"breakpoint_set\",\"breakpoint\":\"" << number << "\"";
        if (!condition_evidence.empty()) {
            std::cout << ",\"condition_evidence\":\"" << condition_evidence << "\"";
        }
        std::cout << "}\n";
        return;
    }
    if (action_name == "watchpoint_set") {
        std::string expression = json_string_field(action, "expression");
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
    if (action_name == "probe_delete" || action_name == "probe_enable" || action_name == "probe_disable") {
        int number = json_int_field(action, "number", -1);
        if (number < 0) {
            std::cout << "{\"ok\":false,\"error\":\"missing probe number\"}\n";
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
        std::cout << "{\"ok\":true,\"action\":\"" << action << "\",\"number\":" << number
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
        run_on_hit_actions(session, probe_state, result);
        std::cout << "{\"ok\":true,\"action\":\"continue\",\"stop_reason\":\"" << result.stop_reason
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
        std::cout << "{\"ok\":true,\"action\":\"hypothesis_create\",\"id\":\"" << id
                  << "\",\"file\":\"" << file.lexically_normal().string() << "\"}\n";
        return;
    }
    if (action_name == "hypothesis_check") {
        std::string id = action.string_or("hypothesis");
        std::string expression = action.string_or("expression");
        if (id.empty() || expression.empty()) {
            std::cout << "{\"ok\":false,\"error\":\"hypothesis_check requires hypothesis and expression\"}\n";
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

        std::cout << "{\"ok\":true,\"action\":\"hypothesis_check\",\"hypothesis\":\"" << id
                  << "\",\"assertion\":\"" << (passed ? "passed" : "failed")
                  << "\",\"evidence\":\"" << ev.id << "\"}\n";
        return;
    }
    if (action_name == "hypothesis_conclude") {
        std::string id = action.string_or("hypothesis");
        std::string conclusion = action.string_or("conclusion", "Inconclusive");
        std::string inference = action.string_or("inference");
        if (id.empty()) {
            std::cout << "{\"ok\":false,\"error\":\"hypothesis_conclude requires hypothesis\"}\n";
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
        std::cout << "{\"ok\":true,\"action\":\"hypothesis_conclude\",\"hypothesis\":\"" << id
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
    if (action_name == "replay") {
        std::string file = json_string_field(action, "file");
        std::string name = json_string_field(action, "name");
        fs::path replay_file;
        if (!file.empty()) {
            replay_file = file;
        } else if (!name.empty()) {
            replay_file = session.assets_dir() / "replay" / (slugify(name) + ".jsonl");
        } else {
            std::cout << "{\"ok\":false,\"error\":\"replay requires file or name\"}\n";
            return;
        }
        replay_action_file(session, probe_state, replay_file);
        std::cout << "{\"ok\":true,\"action\":\"replay\",\"file\":\""
                  << replay_file.lexically_normal().string() << "\"}\n";
        return;
    }
    std::cout << "{\"ok\":false,\"error\":\"unsupported action\"}\n";
}

static void replay_action_file(GdbSession &session, ProbeState &probe_state, const fs::path &path) {
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
        handle_action_line(session, nullptr, probe_state, line, ignored_finish);
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
            replay_action_file(session, probe_state, opts.replay_before_run);
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
            run_on_hit_actions(session, probe_state, run);
        }

        std::cout << "{\"ok\":true,\"session_id\":\"" << opts.session_id
                  << "\",\"state\":\"stopped\",\"stop_reason\":\"" << outcome.stop_reason
                  << "\",\"signal\":\"" << outcome.signal_name << "\"}\n";

        bool finished = false;
        std::string line;
        while (!finished && std::getline(std::cin, line)) {
            handle_action_line(session, &outcome, probe_state, line, finished);
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
