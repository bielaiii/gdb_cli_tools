#include "cli.hpp"

#include "cli/action.hpp"
#include "cli/action_dispatch.hpp"
#include "common/string_utils.hpp"
#include "common/json.hpp"
#include "gdb/gdb_session.hpp"
#include "gdb/mi_utils.hpp"
#include "replay/record_store.hpp"
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

static std::string json_escape(const std::string &s);
static std::string json_string_array(const std::vector<std::string> &items);
static std::string json_string_map(const std::map<std::string, std::string> &items);
static int effective_run_timeout_ms(const CliOptions &opts, const DebugTask &task) {
    return opts.run_timeout_ms > 0 ? opts.run_timeout_ms : task.run_timeout_ms;
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
    RecordingState recording;
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
        ActionContext context{*live.session, &live.task, &live.outcome, live.probe_state, &live.recording};
        (void)replay_action_file(context, live.opts.replay_before_run);
    }

    if (live.task.core_dump) {
        live.outcome.core_mode = true;
        live.outcome.state = SessionState::Loading;
        auto load = live.session->load_core(live.task);
        live.session->evidence_store().add("SessionEvent", "Core load", load.command, load.raw_lines, false, load.record_sequences);
        if (load.result_class == "error") {
            live.outcome.stop_reason = "core_load_failed";
            live.outcome.state = SessionState::Error;
        } else {
            live.outcome.stop_reason = "core_loaded";
            live.outcome.state = SessionState::Stopped;
            collect_core_evidence(*live.session);
        }
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
        ActionContext context{*live.session, &live.task, &live.outcome, live.probe_state, &live.recording};
        (void)handle_probe_stop(context, run);
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
        ActionContext context{*live.session, &live.task, &live.outcome, live.probe_state, &live.recording};
        ActionOutput output = handle_action_json(context, *payload);
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
    RecordingState recording;

    try {
        outcome.state = SessionState::Starting;
        session.start();
        session.initialize(task);
        outcome.state = SessionState::Ready;
        collect_environment_info(session, task, outcome);

        if (!opts.replay_before_run.empty()) {
            ActionContext context{session, &task, &outcome, probe_state, &recording};
            std::cout << action_output_text(replay_action_file(context, opts.replay_before_run));
        }

        if (task.core_dump) {
            outcome.core_mode = true;
            outcome.state = SessionState::Loading;
            auto load = session.load_core(task);
            session.evidence_store().add("SessionEvent", "Core load", load.command, load.raw_lines, false, load.record_sequences);
            if (load.result_class == "error") {
                outcome.stop_reason = "core_load_failed";
                outcome.state = SessionState::Error;
            } else {
                outcome.stop_reason = "core_loaded";
                outcome.state = SessionState::Stopped;
                collect_core_evidence(session);
            }
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
            ActionContext context{session, &task, &outcome, probe_state, &recording};
            for (const auto &result : handle_probe_stop(context, run)) {
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
                    ActionContext context{session, &task, &outcome, probe_state, &recording};
                    output = handle_action_json(context, parse_json(line));
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
