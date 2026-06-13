#include "report.hpp"

#include "../common/json.hpp"
#include "../common/string_utils.hpp"

#include <sstream>
#include <algorithm>
#include <exception>
#include <fstream>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

static std::string display_path(const fs::path &path) {
    return path.lexically_normal().string();
}

static std::string read_text_or_empty(const fs::path &path) {
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

static std::string json_string_value(const Json &json, const std::string &key) {
    const Json *value = json.find(key);
    if (value == nullptr || !value->is_string()) {
        return {};
    }
    return value->string_value;
}

static std::string short_report_text(std::string value) {
    value = trim(std::move(value));
    constexpr size_t kLimit = 1000;
    if (value.size() > kLimit) {
        value.resize(kLimit);
        value += "\n... truncated in report; see hypothesis index and evidence summary ...";
    }
    return value;
}

static std::string table_cell(std::string value) {
    value = short_report_text(std::move(value));
    replace_all(value, "|", "\\|");
    replace_all(value, "\r", " ");
    replace_all(value, "\n", "<br>");
    return value.empty() ? "-" : value;
}

static std::string inline_report_text(std::string value) {
    value = short_report_text(std::move(value));
    replace_all(value, "\r", " ");
    replace_all(value, "\n", " ");
    return value;
}

static Json parse_json_or_null(const std::string &text) {
    try {
        return parse_json(text);
    } catch (const std::exception &) {
        return {};
    }
}

static std::string json_bool_text(const Json &json, const std::string &key) {
    const Json *value = json.find(key);
    if (value == nullptr || !value->is_bool()) {
        return {};
    }
    return value->bool_value ? "true" : "false";
}

static std::string json_int_text(const Json &json, const std::string &key) {
    const Json *value = json.find(key);
    if (value == nullptr || !value->is_number()) {
        return {};
    }
    return std::to_string(static_cast<int>(value->number_value));
}

static std::string json_array_strings_text(const Json &json, const std::string &key) {
    const Json *value = json.find(key);
    if (value == nullptr || !value->is_array()) {
        return {};
    }
    std::ostringstream out;
    bool first = true;
    for (const auto &item : value->array_value) {
        if (!item.is_string()) {
            continue;
        }
        if (!first) {
            out << ", ";
        }
        first = false;
        out << item.string_value;
    }
    return out.str();
}

static std::string json_object_string_value(const Json &json,
                                            const std::string &object_key,
                                            const std::string &value_key) {
    const Json *object = json.find(object_key);
    if (object == nullptr || !object->is_object()) {
        return {};
    }
    return json_string_value(*object, value_key);
}

static std::string evidence_summary_for(const std::string &evidence_id,
                                        const std::map<std::string, const Evidence *> &evidence_by_id) {
    auto it = evidence_by_id.find(evidence_id);
    if (it == evidence_by_id.end() || it->second == nullptr) {
        return {};
    }
    return inline_report_text(it->second->summary);
}

struct ReplayStepSummary {
    std::string run_evidence;
    std::string plan;
    std::string index;
    std::string step_id;
    std::string action_name;
    std::string status;
    std::string failure_policy;
    std::string step_evidence;
    std::string action_evidence;
    std::string error_evidence;
    std::string skip_reason;
};

struct ReplayRunSummary {
    std::string evidence;
    std::string plan;
    std::string file;
    std::string ok;
    std::string force;
    std::string task_metadata_match;
    std::string failure_policy;
    std::string warning_evidence;
    std::string error_evidence;
    std::vector<ReplayStepSummary> steps;
};

struct ReplayWarningSummary {
    std::string evidence;
    std::string plan;
    std::string file;
    std::string force;
    std::string task_metadata_match;
    std::string warning;
};

static ReplayRunSummary replay_run_summary_from_evidence(const Evidence &ev) {
    ReplayRunSummary summary;
    summary.evidence = ev.id;
    Json payload = parse_json_or_null(ev.summary);
    if (!payload.is_object()) {
        return summary;
    }
    summary.file = json_string_value(payload, "file");
    const Json *result = payload.find("result");
    if (result == nullptr || !result->is_object()) {
        return summary;
    }
    summary.plan = json_string_value(*result, "plan");
    if (summary.file.empty()) {
        summary.file = json_string_value(*result, "file");
    }
    summary.ok = json_bool_text(*result, "ok");
    summary.force = json_bool_text(*result, "force");
    summary.task_metadata_match = json_bool_text(*result, "task_metadata_match");
    summary.failure_policy = json_string_value(*result, "failure_policy");
    summary.warning_evidence = json_string_value(*result, "warning_evidence");
    summary.error_evidence = json_string_value(*result, "error_evidence");
    const Json *steps = result->find("steps");
    if (steps != nullptr && steps->is_array()) {
        for (const auto &step : steps->array_value) {
            if (!step.is_object()) {
                continue;
            }
            ReplayStepSummary step_summary;
            step_summary.run_evidence = ev.id;
            step_summary.plan = summary.plan;
            step_summary.index = json_int_text(step, "index");
            step_summary.step_id = json_string_value(step, "step_id");
            step_summary.action_name = json_string_value(step, "action_name");
            step_summary.status = json_string_value(step, "status");
            step_summary.failure_policy = json_string_value(step, "failure_policy");
            step_summary.step_evidence = json_string_value(step, "evidence");
            step_summary.action_evidence = json_string_value(step, "action_evidence");
            step_summary.error_evidence = json_string_value(step, "error_evidence");
            step_summary.skip_reason = json_string_value(step, "skip_reason");
            summary.steps.push_back(std::move(step_summary));
        }
    }
    return summary;
}

static ReplayStepSummary replay_step_summary_from_evidence(const Evidence &ev) {
    ReplayStepSummary summary;
    summary.step_evidence = ev.id;
    Json payload = parse_json_or_null(ev.summary);
    if (!payload.is_object()) {
        return summary;
    }
    summary.plan = json_string_value(payload, "plan");
    summary.index = json_int_text(payload, "index");
    summary.step_id = json_string_value(payload, "step_id");
    summary.action_name = json_string_value(payload, "action_name");
    summary.status = json_string_value(payload, "status");
    summary.failure_policy = json_string_value(payload, "failure_policy");
    summary.action_evidence = json_string_value(payload, "action_evidence");
    summary.error_evidence = json_string_value(payload, "error_evidence");
    summary.skip_reason = json_string_value(payload, "skip_reason");
    return summary;
}

static ReplayWarningSummary replay_warning_summary_from_evidence(const Evidence &ev) {
    ReplayWarningSummary summary;
    summary.evidence = ev.id;
    Json payload = parse_json_or_null(ev.summary);
    if (!payload.is_object()) {
        return summary;
    }
    summary.plan = json_string_value(payload, "plan");
    summary.file = json_string_value(payload, "file");
    summary.force = json_bool_text(payload, "force");
    summary.task_metadata_match = json_bool_text(payload, "task_metadata_match");
    summary.warning = json_string_value(payload, "warning");
    return summary;
}

static void write_replay_execution_audit(std::ostringstream &md, const std::vector<Evidence> &evidence) {
    std::vector<ReplayRunSummary> runs;
    std::vector<ReplayStepSummary> steps;
    std::vector<ReplayWarningSummary> warnings;
    for (const auto &ev : evidence) {
        if (ev.kind == "ReplayRun") {
            runs.push_back(replay_run_summary_from_evidence(ev));
        } else if (ev.kind == "ReplayStep") {
            steps.push_back(replay_step_summary_from_evidence(ev));
        } else if (ev.kind == "ReplayWarning") {
            warnings.push_back(replay_warning_summary_from_evidence(ev));
        }
    }
    if (runs.empty() && warnings.empty()) {
        return;
    }

    md << "## Replay Execution Audit\n\n";
    md << "Replay execution is summarized from structured `ReplayRun`, `ReplayStep`, and `ReplayWarning` evidence.\n\n";

    if (!runs.empty()) {
        md << "### Replay Runs\n\n";
        md << "| Run Evidence | Plan | File | OK | Force | Task Match | Policy | Warning Evidence | Error Evidence |\n";
        md << "| --- | --- | --- | --- | --- | --- | --- | --- | --- |\n";
        for (const auto &run : runs) {
            md << "| `" << run.evidence
               << "` | `" << run.plan
               << "` | " << table_cell(run.file)
               << " | `" << run.ok
               << "` | `" << run.force
               << "` | `" << run.task_metadata_match
               << "` | `" << run.failure_policy
               << "` | " << (run.warning_evidence.empty() ? "`-`" : ("`" + run.warning_evidence + "`"))
               << " | " << (run.error_evidence.empty() ? "`-`" : ("`" + run.error_evidence + "`"))
               << " |\n";
        }
        md << "\n";

        if (steps.empty()) {
            for (const auto &run : runs) {
                for (const auto &step : run.steps) {
                    steps.push_back(step);
                }
            }
        } else {
            std::map<std::string, std::string> run_evidence_by_plan;
            std::map<std::string, bool> duplicate_plan;
            for (const auto &run : runs) {
                if (run.plan.empty()) {
                    continue;
                }
                if (run_evidence_by_plan.count(run.plan) > 0) {
                    duplicate_plan[run.plan] = true;
                } else {
                    run_evidence_by_plan[run.plan] = run.evidence;
                }
            }
            for (auto &step : steps) {
                if (!step.run_evidence.empty()) {
                    continue;
                }
                auto run_it = run_evidence_by_plan.find(step.plan);
                if (run_it != run_evidence_by_plan.end() && !duplicate_plan[step.plan]) {
                    step.run_evidence = run_it->second;
                } else {
                    step.run_evidence = step.plan;
                }
            }
        }

        md << "### Replay Steps\n\n";
        md << "| Run | Step | Action | Status | Policy | Step Evidence | Action Evidence | Error Evidence | Skip Reason |\n";
        md << "| --- | --- | --- | --- | --- | --- | --- | --- | --- |\n";
        for (const auto &step : steps) {
            md << "| `" << step.run_evidence
               << "` | `" << step.step_id
               << "` | `" << step.action_name
               << "` | `" << step.status
               << "` | `" << step.failure_policy
               << "` | " << (step.step_evidence.empty() ? "`-`" : ("`" + step.step_evidence + "`"))
               << " | " << (step.action_evidence.empty() ? "`-`" : ("`" + step.action_evidence + "`"))
               << " | " << (step.error_evidence.empty() ? "`-`" : ("`" + step.error_evidence + "`"))
               << " | " << table_cell(step.skip_reason) << " |\n";
        }
        md << "\n";
    }

    if (!warnings.empty()) {
        md << "### Replay Warnings\n\n";
        md << "| Evidence | Plan | File | Force | Task Match | Warning |\n";
        md << "| --- | --- | --- | --- | --- | --- |\n";
        for (const auto &warning : warnings) {
            md << "| `" << warning.evidence
               << "` | `" << warning.plan
               << "` | " << table_cell(warning.file)
               << " | `" << warning.force
               << "` | `" << warning.task_metadata_match
               << "` | " << table_cell(warning.warning) << " |\n";
        }
        md << "\n";
    }
}

static bool write_hypotheses_from_index(std::ostringstream &md,
                                        const fs::path &assets,
                                        const fs::path &index,
                                        const std::map<std::string, const Evidence *> &evidence_by_id) {
    std::string text = read_text_or_empty(index);
    if (text.empty()) {
        return false;
    }

    Json root;
    try {
        root = parse_json(text);
    } catch (const std::exception &ex) {
        md << "- Structured index: `" << display_path(index) << "` could not be parsed: `" << ex.what() << "`\n\n";
        return false;
    }

    const Json *hypotheses = root.find("hypotheses");
    if (hypotheses == nullptr || !hypotheses->is_array()) {
        md << "- Structured index: `" << display_path(index) << "` does not contain a hypotheses array.\n\n";
        return false;
    }

    md << "- Structured index: `" << display_path(index) << "`\n";
    md << "- Tool observation, assertion status, agent inference, and final agent conclusion are separate fields.\n\n";

    if (hypotheses->array_value.empty()) {
        md << "No hypothesis records were written.\n\n";
        return true;
    }

    for (const auto &hypothesis : hypotheses->array_value) {
        if (!hypothesis.is_object()) {
            continue;
        }
        std::string id = json_string_value(hypothesis, "id");
        std::string title = json_string_value(hypothesis, "title");
        std::string description = json_string_value(hypothesis, "description");
        std::string tool_status = json_string_value(hypothesis, "tool_status");
        std::string agent_inference = json_string_value(hypothesis, "agent_inference");
        std::string agent_conclusion = json_string_value(hypothesis, "agent_conclusion");
        fs::path hypothesis_file = assets / "hypotheses" / (id + ".md");

        md << "### " << (id.empty() ? "unknown" : id);
        if (!title.empty()) {
            md << " " << title;
        }
        md << "\n\n";
        md << "- Tool status: `" << tool_status << "`\n";
        md << "- Hypothesis file: `" << display_path(hypothesis_file) << "`\n";
        if (!description.empty()) {
            md << "- Description: " << description << "\n";
        }

        const Json *checks = hypothesis.find("checks");
        if (checks != nullptr && checks->is_array() && !checks->array_value.empty()) {
            std::vector<const Json *> attention_checks;
            md << "\n| Check | Description | Expression | Assertion | Expected | Status | Evidence | Error Evidence | Observed Summary |\n";
            md << "| --- | --- | --- | --- | --- | --- | --- | --- | --- |\n";
            for (const auto &check : checks->array_value) {
                if (!check.is_object()) {
                    continue;
                }
                std::string error_evidence = json_string_value(check, "error_evidence");
                std::string status = json_string_value(check, "status");
                if (!error_evidence.empty() || status == "unknown") {
                    attention_checks.push_back(&check);
                }
                md << "| `" << json_string_value(check, "id")
                   << "` | " << table_cell(json_string_value(check, "description"))
                   << " | `" << json_string_value(check, "expression")
                   << "` | `" << json_string_value(check, "assertion")
                   << "` | `" << json_string_value(check, "expected")
                   << "` | `" << status
                   << "` | `" << json_string_value(check, "evidence")
                   << "` | " << (error_evidence.empty() ? "`-`" : ("`" + error_evidence + "`"))
                   << " | " << table_cell(json_string_value(check, "observed")) << " |\n";
            }
            md << "\n";
            if (!attention_checks.empty()) {
                md << "Checks needing attention:\n\n";
                for (const Json *check : attention_checks) {
                    std::string error_evidence = json_string_value(*check, "error_evidence");
                    md << "- `" << json_string_value(*check, "id") << "` status `"
                       << json_string_value(*check, "status") << "`";
                    if (!error_evidence.empty()) {
                        md << ", error evidence `" << error_evidence << "`";
                        std::string error_summary = evidence_summary_for(error_evidence, evidence_by_id);
                        if (!error_summary.empty()) {
                            md << ", error summary: " << error_summary;
                        }
                    }
                    md << ", evidence `" << json_string_value(*check, "evidence") << "`\n";
                }
                md << "\n";
            }
            for (const auto &check : checks->array_value) {
                if (!check.is_object()) {
                    continue;
                }
                md << "Observed `" << json_string_value(check, "id") << "`:\n\n";
                md << "```text\n" << short_report_text(json_string_value(check, "observed")) << "\n```\n\n";
            }
        } else {
            md << "- Checks: none\n\n";
        }

        if (!agent_inference.empty()) {
            md << "Agent inference:\n\n" << agent_inference << "\n\n";
        }
        if (!agent_conclusion.empty()) {
            md << "Final agent conclusion: `" << agent_conclusion << "`\n\n";
        }
    }

    return true;
}

static void write_hypothesis_file_list(std::ostringstream &md, const fs::path &hypotheses_dir) {
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

static void write_replay_plan_file_table(std::ostringstream &md, const std::vector<fs::path> &files) {
    if (files.empty()) {
        md << "No replay plans were written.\n\n";
        return;
    }

    md << "| Plan File | Name | Tags | Source Session | Failure Policy | Task Fingerprint |\n";
    md << "| --- | --- | --- | --- | --- | --- |\n";
    for (const auto &file : files) {
        Json plan = parse_json_or_null(read_text_or_empty(file));
        std::string name;
        std::string tags;
        std::string source_session;
        std::string failure_policy;
        std::string fingerprint;
        if (plan.is_object()) {
            name = json_string_value(plan, "name");
            tags = json_array_strings_text(plan, "tags");
            source_session = json_string_value(plan, "source_session_id");
            failure_policy = json_string_value(plan, "failure_policy");
            fingerprint = json_object_string_value(plan, "task", "fingerprint");
        }
        md << "| `" << display_path(file)
           << "` | `" << name
           << "` | " << table_cell(tags)
           << " | `" << source_session
           << "` | `" << failure_policy
           << "` | `" << fingerprint
           << "` |\n";
    }
    md << "\n";
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

    std::map<std::string, const Evidence *> evidence_by_id;
    for (const auto &ev : evidence) {
        evidence_by_id[ev.id] = &ev;
    }

    std::vector<const Evidence *> tool_errors;
    for (const auto &ev : evidence) {
        if (ev.kind == "ToolError") {
            tool_errors.push_back(&ev);
        }
    }
    if (!tool_errors.empty()) {
        md << "## Tool Errors\n\n";
        md << "| Evidence | Action | Error | Command Evidence | Summary |\n";
        md << "| --- | --- | --- | --- | --- |\n";
        for (const auto *ev : tool_errors) {
            Json payload = parse_json_or_null(ev->summary);
            std::string action = payload.is_object() ? json_string_value(payload, "action") : ev->command;
            std::string error = payload.is_object() ? json_string_value(payload, "error") : "";
            std::string command_evidence = payload.is_object() ? json_string_value(payload, "command_evidence") : "";
            md << "| `" << ev->id << "` | `" << action << "` | "
               << table_cell(error.empty() ? ev->title : error)
               << " | " << (command_evidence.empty() ? "`-`" : ("`" + command_evidence + "`"))
               << " | " << table_cell(ev->summary) << " |\n";
        }
        md << "\n";
    }

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
        bool rendered_index = false;
        if (fs::exists(index)) {
            rendered_index = write_hypotheses_from_index(md, assets, index, evidence_by_id);
        }
        if (!rendered_index) {
            write_hypothesis_file_list(md, hypotheses_dir);
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

        std::vector<const Evidence *> probe_events;
        for (const auto &ev : evidence) {
            if (ev.kind == "BreakpointHit" || ev.kind == "WatchpointHit" ||
                ev.kind == "CatchpointHit" || ev.kind == "OnHitAction") {
                probe_events.push_back(&ev);
            }
        }
        if (!probe_events.empty()) {
            md << "### Probe Hit And On-Hit Evidence\n\n";
            for (const auto *ev : probe_events) {
                md << "- `" << ev->id << "` `" << ev->kind << "` " << ev->title
                   << " (summary: `" << display_path(ev->summary_file) << "`)\n";
            }
            md << "\n";
        }
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
        write_replay_plan_file_table(md, files);
    }

    write_replay_execution_audit(md, evidence);

    md << "## Limitations\n\n";
    md << "- This MVP uses GDB/MI without PTY support.\n";
    md << "- Interactive stdin for the inferior is not implemented; task stdin is configured as `" << task.stdin_path.string() << "`.\n";
    md << "- Inferior stdout/stderr are redirected to files; non-empty new output is captured as `InferiorOutput` evidence after stop events.\n";
    md << "- The tool records observations and evidence; final root-cause judgment belongs to the AI Agent.\n";
    md << "- Evidence summaries are intentionally lossy; raw files and the session MI log remain authoritative.\n";
    md << "- Hypothesis observed values are lossy summaries of command output; inspect linked raw evidence before final conclusions.\n\n";

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
