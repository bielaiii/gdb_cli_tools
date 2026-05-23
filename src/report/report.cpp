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
    md << "- Core Dump: `" << (task.core_dump ? task.core_dump->string() : std::string("")) << "`\n\n";

    md << "## Session Summary\n\n";
    md << "- Mode: " << (outcome.core_mode ? "Core Dump" : "Run") << "\n";
    md << "- Stop reason: `" << outcome.stop_reason << "`\n";
    md << "- Signal: `" << outcome.signal_name << "`\n";
    md << "- Segfault observed: " << (outcome.segfault ? "yes" : "no") << "\n";
    md << "- Run deadline reached: " << (outcome.run_timed_out ? "yes" : "no") << "\n\n";

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
        md << "- Raw MI: `" << display_path(ev.raw_file) << "`\n\n";
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
        std::vector<fs::path> files;
        for (const auto &entry : fs::directory_iterator(hypotheses_dir)) {
            if (entry.is_regular_file()) {
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

    md << "## Limitations\n\n";
    md << "- This MVP uses GDB/MI without PTY support.\n";
    md << "- Interactive stdin for the inferior is not implemented; task stdin defaults to non-interactive execution.\n";
    md << "- The tool records observations and evidence; final root-cause judgment belongs to the AI Agent.\n";
    md << "- Full breakpoint/watchpoint replay and hypothesis workflows are planned after this segfault MVP.\n\n";

    md << "## Raw Evidence Index\n\n";
    md << "- Session MI log: `" << display_path(assets / "logs" / "session.mi.raw.log") << "`\n";
    for (const auto &ev : evidence) {
        md << "- " << ev.id << ": `" << display_path(ev.raw_file) << "`\n";
    }

    write_text_file(report, md.str());
}
