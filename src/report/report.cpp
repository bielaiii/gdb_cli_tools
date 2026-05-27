#include "report.hpp"

#include "../common/string_utils.hpp"

#include <sstream>
#include <algorithm>
#include <string_view>

namespace fs = std::filesystem;

static std::string display_path(const fs::path &path) {
    return path.lexically_normal().string();
}

void write_report(const fs::path &report,
                  const fs::path &assets,
                  const DebugTask &task,
                  const SessionOutcome &outcome,
                  const std::vector<Evidence> &evidence) {
    std::ostringstream md;
    md << "# GDB Verification Report\n\n";
    md << "## Task\n\n";
    md << "- Problem: " << task.problem << "\n";
    md << "- Executable: `" << task.executable.string() << "`\n";
    md << "- Working Directory: `" << task.working_directory.string() << "`\n";
    md << "- Args: `" << task.args_raw << "`\n";
    md << "- Argv:";
    if (task.args.empty()) {
        md << " `(empty)`";
    } else {
        for (const auto &arg : task.args) {
            md << " `" << arg << "`";
        }
    }
    md << "\n";
    md << "- Stdin: `" << task.stdin_path.string() << "`\n";
    md << "- Stdout log: `" << outcome.inferior_stdout << "`\n";
    md << "- Stderr log: `" << outcome.inferior_stderr << "`\n";
    md << "- Run timeout: `" << outcome.run_timeout_ms << " ms`\n";
    if (!task.env.empty()) {
        md << "- Environment overrides:";
        for (const auto &[key, value] : task.env) {
            md << " `" << key << "=" << value << "`";
        }
        md << "\n";
    }
    md << "- Core Dump: `" << (task.core_dump ? task.core_dump->string() : std::string("")) << "`\n\n";

    md << "## Session Summary\n\n";
    md << "- Mode: " << (outcome.core_mode ? "Core Dump" : "Run") << "\n";
    md << "- State: `" << session_state_name(outcome.state) << "`\n";
    md << "- Stop reason: `" << outcome.stop_reason << "`\n";
    md << "- Signal: `" << outcome.signal_name << "`\n";
    md << "- Segfault observed: " << (outcome.segfault ? "yes" : "no") << "\n";
    md << "- Run deadline reached: " << (outcome.run_timed_out ? "yes" : "no") << "\n\n";

    md << "## Environment\n\n";
    md << "- GDB version and debug target metadata are recorded as `EnvironmentInfo` evidence.\n";
    md << "- Executable metadata is captured in `Debug target metadata` evidence.\n";
    if (task.core_dump) {
        md << "- Core dump metadata is captured in `Debug target metadata` evidence.\n";
    }
    md << "\n";

    md << "## Tool Observations\n\n";
    if (outcome.segfault) {
        md << "- The inferior stopped with `SIGSEGV`; light crash evidence was collected.\n";
    } else if (outcome.run_timed_out) {
        md << "- The inferior did not stop before the run deadline; the tool interrupted it for evidence collection.\n";
    } else if (outcome.core_mode) {
        md << "- The core dump was loaded and static crash evidence was collected.\n";
    } else {
        md << "- No SIGSEGV was observed during the initial run.\n";
    }
    md << "\n";

    md << "## Evidence Summary\n\n";
    for (const auto &ev : evidence) {
        md << "### " << ev.id << " " << ev.title << "\n\n";
        md << "- Kind: `" << ev.kind << "`\n";
        md << "- Command: `" << ev.command << "`\n";
        md << "- Human view: `" << display_path(ev.view_file) << "`\n";
        md << "- Summary: `" << display_path(ev.summary_file) << "`\n";
        md << "- Raw MI: `" << display_path(ev.raw_file) << "`\n";
        md << "- Raw SHA-256: `" << ev.raw_sha256 << "`\n";
        md << "- Captured at: `" << ev.captured_at << "`\n";
        md << "- Raw bytes: `" << ev.raw_bytes << "`, kept summary bytes: `" << ev.kept_bytes << "`\n";
        md << "- Truncated: `" << (ev.truncated ? "true" : "false")
           << "`, lossy summary: `" << (ev.lossy_summary ? "true" : "false") << "`\n";
        md << "- Record attribution: included `" << ev.included_records.size()
           << "`, related `" << ev.related_records.size()
           << "`, concurrent `" << ev.concurrent_records.size() << "`\n\n";
        std::string_view summary_view(ev.summary);
        md << "```text\n" << summary_view.substr(0, 4000);
        if (ev.summary.size() > 4000) {
            md << "\n... truncated in report; see summary file ...";
        }
        md << "\n```\n\n";
    }

    fs::path hypotheses_dir = assets / "hypotheses";
    if (fs::exists(hypotheses_dir)) {
        md << "## Hypotheses\n\n";
        fs::path index = hypotheses_dir / "index.json";
        if (fs::exists(index)) {
            md << "- Structured index: `" << display_path(index) << "`\n";
        }
        std::vector<fs::path> files;
        for (const auto &entry : fs::directory_iterator(hypotheses_dir)) {
            if (entry.is_regular_file() && entry.path().filename() != "index.json") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        if (files.empty()) {
            md << "No hypothesis records were written.\n\n";
        } else {
            for (const auto &file : files) {
                md << "- `" << display_path(file) << "`\n";
            }
            md << "\n";
        }
    }

    md << "## Agent Inference\n\n";
    if (outcome.agent_inference.empty()) {
        md << "No agent inference was provided at finish time.\n\n";
    } else {
        md << outcome.agent_inference << "\n\n";
    }

    md << "## Final Agent Conclusion\n\n";
    if (outcome.final_agent_conclusion.empty()) {
        md << "No final agent conclusion was provided at finish time.\n\n";
    } else {
        md << outcome.final_agent_conclusion << "\n\n";
    }

    fs::path probes_file = assets / "probes.json";
    if (fs::exists(probes_file)) {
        md << "## Probes\n\n";
        md << "- Probe store: `" << display_path(probes_file) << "`\n\n";
    }

    fs::path replay_dir = assets / "replay";
    if (fs::exists(replay_dir)) {
        md << "## Replay Plans\n\n";
        std::vector<fs::path> files;
        for (const auto &entry : fs::directory_iterator(replay_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        if (files.empty()) {
            md << "No replay plans were written.\n\n";
        } else {
            for (const auto &file : files) {
                md << "- `" << display_path(file) << "`\n";
            }
            md << "\n";
        }
    }

    md << "## Limitations\n\n";
    md << "- This MVP uses GDB/MI without PTY support.\n";
    md << "- Interactive stdin for the inferior is not implemented; task stdin is configured as `" << task.stdin_path.string() << "`.\n";
    md << "- Inferior stdout/stderr are redirected to files; non-empty new output is captured as `InferiorOutput` evidence after stop events.\n";
    md << "- The tool records observations and evidence; final root-cause judgment belongs to the AI Agent.\n";
    md << "- Evidence summaries are intentionally lossy; raw files and the session MI log remain authoritative.\n\n";

    md << "## Raw Evidence Index\n\n";
    md << "- Session MI log: `" << display_path(assets / "logs" / "session.mi.raw.log") << "`\n";
    md << "- Session snapshot: `" << display_path(assets / "session_snapshot.json") << "`\n";
    md << "- Session summary: `" << display_path(assets / "session_summary.json") << "`\n";
    md << "- Evidence index: `" << display_path(assets / "evidence" / "index.json") << "`\n";
    for (const auto &ev : evidence) {
        md << "- " << ev.id << ": `" << display_path(ev.raw_file) << "` sha256 `" << ev.raw_sha256 << "`\n";
    }

    write_text_file(report, md.str());
}
