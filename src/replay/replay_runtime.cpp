#include "replay_runtime.hpp"

#include "replay_plan.hpp"
#include "../cli/action_context.hpp"
#include "../cli/action_dispatch.hpp"
#include "../common/json.hpp"
#include "../common/string_utils.hpp"
#include "../gdb/gdb_session.hpp"
#include "../task/debug_task.hpp"
#include "../workflow/crash_workflow.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string json_escape(const std::string &s) {
    Json json;
    json.type = Json::Type::String;
    json.string_value = s;
    return dump_json(json);
}

} // namespace

static std::vector<ActionRequest> load_replay_jsonl_actions(const fs::path &path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open replay file: " + path.string());
    }

    std::vector<ActionRequest> actions;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        Json action = parse_json(line);
        if (!action.is_object()) {
            throw std::runtime_error("replay JSONL action must be an object: " + path.string());
        }
        actions.push_back(parse_action_request(action));
    }
    return actions;
}

void rebuild_replay_plan_from_jsonl(const fs::path &jsonl_file,
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

struct ReplayActionStep {
    int index = 0;
    std::string step_id;
    std::string name;
    bool enabled = true;
    std::string failure_policy = kReplayPolicyContinue;
    ActionRequest action;
};

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

static ResultObject replay_step_result_object(const ReplayStepRunResult &step) {
    ResultObject object;
    result_object_set_int(object, "index", step.index);
    result_object_set_string(object, "step_id", step.step_id);
    result_object_set_string(object, "action_name", step.action_name);
    result_object_set_string(object, "status", step.status);
    result_object_set_string(object, "failure_policy", step.failure_policy);
    result_object_set_string(object, "evidence", step.evidence_id);
    result_object_set_string(object, "action_evidence", step.action_evidence_id);
    result_object_set_string(object, "error_evidence", step.error_evidence_id);
    result_object_set_string(object, "error", step.error);
    result_object_set_string(object, "skip_reason", step.skip_reason);
    return object;
}

static ActionResult replay_result_action(const fs::path &path, const ReplayRunResult &run) {
    ActionResult result = run.ok
                              ? ActionResult::success(ActionKind::Replay, "replay")
                              : ActionResult::failure(ActionKind::Replay, "replay", run.error);
    action_result_set_string(result, "file", path.lexically_normal().string());
    action_result_set_string(result, "plan", run.plan_name);
    action_result_set_string(result, "schema", run.schema);
    action_result_set_int(result, "schema_version", run.schema_version);
    action_result_set_bool(result, "force", run.force);
    action_result_set_bool(result, "task_metadata_match", run.task_metadata_match);
    action_result_set_string(result, "failure_policy", run.failure_policy);
    action_result_set_string(result, "warning", run.warning);
    action_result_set_string(result, "warning_evidence", run.warning_evidence_id);
    action_result_set_string(result, "error_evidence", run.error_evidence_id);
    action_result_set_string(result, "run_evidence", run.run_evidence_id);
    ResultArray steps;
    for (const auto &step : run.steps) {
        steps.items.push_back(replay_step_result_object(step));
    }
    action_result_set_array(result, "steps", std::move(steps));
    return result;
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

static ReplayStepRunResult replay_action_step(GdbSession &session,
                                              const DebugTask *task,
                                              SessionOutcome *outcome,
                                              ProbeState &probe_state,
                                              const std::string &plan_name,
                                              const ReplayActionStep &step,
                                              std::vector<ActionResult> &prelude) {
    ReplayStepRunResult result;
    result.index = step.index;
    result.step_id = step.step_id;
    result.failure_policy = step.failure_policy;
    std::string action_name = step.action.action.empty() ? step.name : step.action.action;
    if (action_name.empty()) {
        action_name = "unknown";
    }
    result.action_name = action_name;

    try {
        ActionContext action_context{session, task, outcome, probe_state};
        ActionOutput action_output = handle_action_request(action_context, step.action);
        std::string step_output = action_output_text(action_output);
        prelude.insert(prelude.end(), action_output.prelude.begin(), action_output.prelude.end());
        prelude.push_back(action_output.final);
        const ActionResult &action_result = action_output.final;
        bool failed = !action_result.ok;
        result.status = failed ? "failed" : "success";
        result.error = failed ? action_result.error : "";
        result.action_evidence_id = action_result.evidence_id.empty()
                                      ? action_result.command_evidence_id
                                      : action_result.evidence_id;
        if (failed && result.error_evidence_id.empty()) {
            std::ostringstream error_text;
            error_text << "{\n";
            error_text << "  \"plan\": " << json_escape(plan_name) << ",\n";
            error_text << "  \"step_id\": " << json_escape(step.step_id) << ",\n";
            error_text << "  \"index\": " << step.index << ",\n";
            error_text << "  \"action_name\": " << json_escape(action_name) << ",\n";
            error_text << "  \"failure_policy\": " << json_escape(step.failure_policy) << ",\n";
            error_text << "  \"error\": " << json_escape(result.error) << ",\n";
            error_text << "  \"response\": " << json_escape(step_output) << "\n";
            error_text << "}\n";
            auto ev = session.evidence_store().add_text("ToolError", "Replay step reported failure " + step.step_id, action_name, error_text.str());
            result.error_evidence_id = ev.id;
        }
        std::ostringstream evidence_text;
        evidence_text << "{\n";
        evidence_text << "  \"plan\": " << json_escape(plan_name) << ",\n";
        evidence_text << "  \"step_id\": " << json_escape(step.step_id) << ",\n";
        evidence_text << "  \"index\": " << step.index << ",\n";
        evidence_text << "  \"action_name\": " << json_escape(action_name) << ",\n";
        evidence_text << "  \"status\": " << json_escape(result.status) << ",\n";
        evidence_text << "  \"failure_policy\": " << json_escape(step.failure_policy) << ",\n";
        evidence_text << "  \"action_json\": " << json_escape(dump_json(action_request_to_json(step.action))) << ",\n";
        evidence_text << "  \"action_evidence\": " << json_escape(result.action_evidence_id) << ",\n";
        evidence_text << "  \"error_evidence\": " << json_escape(result.error_evidence_id) << ",\n";
        evidence_text << "  \"error\": " << json_escape(result.error) << ",\n";
        evidence_text << "  \"response\": " << json_escape(step_output) << "\n";
        evidence_text << "}\n";
        auto ev = session.evidence_store().add_text("ReplayStep", "Replay step " + step.step_id + " " + result.status, action_name, evidence_text.str());
        result.evidence_id = ev.id;
    } catch (const std::exception &ex) {
        result.status = "failed";
        result.error = ex.what();
        std::ostringstream evidence_text;
        evidence_text << "{\n";
        evidence_text << "  \"plan\": " << json_escape(plan_name) << ",\n";
        evidence_text << "  \"step_id\": " << json_escape(step.step_id) << ",\n";
        evidence_text << "  \"index\": " << step.index << ",\n";
        evidence_text << "  \"action_name\": " << json_escape(action_name) << ",\n";
        evidence_text << "  \"status\": \"failed\",\n";
        evidence_text << "  \"failure_policy\": " << json_escape(step.failure_policy) << ",\n";
        evidence_text << "  \"action\": " << json_escape(dump_json(action_request_to_json(step.action))) << ",\n";
        evidence_text << "  \"error\": " << json_escape(ex.what()) << "\n";
        evidence_text << "}\n";
        auto ev = session.evidence_store().add_text("ToolError", "Replay step failed " + step.step_id, action_name, evidence_text.str());
        result.evidence_id = ev.id;
        result.error_evidence_id = ev.id;
        ActionResult emitted = ActionResult::failure(ActionKind::Replay, "replay_step", ex.what());
        emitted.evidence_id = ev.id;
        action_result_set_string(emitted, "step_id", step.step_id);
        prelude.push_back(std::move(emitted));
    }
    return result;
}

static ReplayRunResult replay_json_plan(GdbSession &session,
                                        const DebugTask *task,
                                        SessionOutcome *outcome,
                                        ProbeState &probe_state,
                                        const fs::path &path,
                                        const Json &plan,
                                        std::vector<ActionResult> &prelude,
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
        ReplayActionStep replay_step;
        replay_step.index = index;
        replay_step.step_id = step_id;
        replay_step.name = action_name;
        replay_step.failure_policy = step_policy;
        replay_step.action = parse_action_request(*action);
        ReplayStepRunResult step_result = replay_action_step(session,
                                                             task,
                                                             outcome,
                                                             probe_state,
                                                             plan_name,
                                                             replay_step,
                                                             prelude);
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

ActionOutput replay_action_file(ActionContext &context,
                                const fs::path &path,
                                bool force,
                                const std::string &failure_policy_override) {
    GdbSession &session = context.session;
    const DebugTask *task = context.task;
    SessionOutcome *outcome = context.outcome;
    ProbeState &probe_state = context.probe_state;
    ActionOutput output;
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
        output.final = replay_result_action(path, result);
        return output;
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
                                      output.prelude,
                                      force,
                                      failure_policy_override);
        } else {
            std::string policy = normalize_replay_failure_policy(failure_policy_override);
            result.failure_policy = policy;
            ReplayActionStep step;
            step.index = 1;
            step.step_id = "a1";
            step.name = replay_action_display_name(plan, 1);
            step.failure_policy = policy;
            step.action = parse_action_request(plan);
            result.steps.push_back(replay_action_step(session,
                                                      task,
                                                      outcome,
                                                      probe_state,
                                                      path.stem().string(),
                                                      step,
                                                      output.prelude));
            result.ok = result.steps.back().status != "failed";
        }
        add_replay_run_evidence(session, path, result);
        output.final = replay_result_action(path, result);
        return output;
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
        Json action = parse_json(line);
        if (!action.is_object()) {
            throw std::runtime_error("replay JSONL action must be an object: " + path.string());
        }
        ReplayActionStep step;
        step.index = index;
        step.step_id = step_id.str();
        step.name = replay_action_display_name(action, index);
        step.failure_policy = plan_policy;
        step.action = parse_action_request(action);
        ReplayStepRunResult step_result = replay_action_step(session,
                                                             task,
                                                             outcome,
                                                             probe_state,
                                                             plan_name,
                                                             step,
                                                             output.prelude);
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
    output.final = replay_result_action(path, result);
    return output;
}

