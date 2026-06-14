#include "probe_runtime.hpp"

#include "../cli/action_context.hpp"
#include "../cli/session_executor.hpp"
#include "../common/json.hpp"
#include "../common/string_utils.hpp"
#include "../gdb/gdb_session.hpp"
#include "../gdb/mi_utils.hpp"
#include "crash_workflow.hpp"

#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>

namespace fs = std::filesystem;

namespace {

std::string json_escape(const std::string &s) {
    Json json;
    json.type = Json::Type::String;
    json.string_value = s;
    return dump_json(json);
}

} // namespace

static std::string watchpoint_number_from_stop_record(const CommandResult &result) {
    if (!result.breakpoint_number.empty()) {
        return result.breakpoint_number;
    }
    for (const auto &raw : result.raw_lines) {
        const std::string wpt_prefix = "wpt={";
        auto pos = raw.find(wpt_prefix);
        if (pos == std::string::npos) {
            continue;
        }
        auto number_pos = raw.find("number=\"", pos + wpt_prefix.size());
        if (number_pos == std::string::npos) {
            continue;
        }
        number_pos += std::string("number=\"").size();
        std::string number;
        bool escaped = false;
        for (; number_pos < raw.size(); ++number_pos) {
            char c = raw[number_pos];
            if (escaped) {
                number.push_back(c);
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
            number.push_back(c);
        }
        if (!number.empty()) {
            return number;
        }
    }
    return {};
}

static std::string json_action_array(const std::vector<ActionRequestPtr> &actions) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < actions.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << (actions[i] ? dump_json(action_request_to_json(*actions[i])) : std::string("null"));
    }
    out << "]";
    return out.str();
}

static std::string json_string_vector(const std::vector<std::string> &items) {
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

static std::string on_hit_policy_json(const ProbeState::OnHitPolicy &policy) {
    std::ostringstream out;
    out << "{";
    out << "\"configured\":" << (policy.configured ? "true" : "false") << ",";
    out << "\"actions\":" << json_action_array(policy.actions) << ",";
    out << "\"timeout_ms\":" << policy.timeout_ms << ",";
    out << "\"max_output_bytes\":" << policy.max_output_bytes << ",";
    out << "\"max_summary_lines\":" << policy.max_summary_lines << ",";
    out << "\"failure_policy\":" << json_escape(policy.failure_policy) << ",";
    out << "\"continue_after_hit\":" << (policy.continue_after_hit ? "true" : "false");
    out << "}";
    return out.str();
}

void write_probe_snapshot(GdbSession &session, const ProbeState &probe_state) {
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
        out << "      \"selector\": " << json_escape(probe.selector) << ",\n";
        out << "      \"location\": " << json_escape(probe.location) << ",\n";
        out << "      \"expression\": " << json_escape(probe.expression) << ",\n";
        out << "      \"condition\": " << json_escape(probe.condition) << ",\n";
        out << "      \"comment\": " << json_escape(probe.comment) << ",\n";
        out << "      \"purpose\": " << json_escape(probe.purpose) << ",\n";
        out << "      \"enabled\": " << (probe.enabled ? "true" : "false") << ",\n";
        out << "      \"deleted\": " << (probe.deleted ? "true" : "false") << ",\n";
        out << "      \"hit_count\": " << probe.hit_count << ",\n";
        out << "      \"last_stop_reason\": " << json_escape(probe.last_stop_reason) << ",\n";
        out << "      \"on_hit\": " << on_hit_policy_json(probe.on_hit_policy) << "\n";
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
    out << "\"selector\":" << json_escape(probe.selector) << ",";
    out << "\"location\":" << json_escape(probe.location) << ",";
    out << "\"expression\":" << json_escape(probe.expression) << ",";
    out << "\"condition\":" << json_escape(probe.condition) << ",";
    out << "\"comment\":" << json_escape(probe.comment) << ",";
    out << "\"purpose\":" << json_escape(probe.purpose) << ",";
    out << "\"enabled\":" << (probe.enabled ? "true" : "false") << ",";
    out << "\"deleted\":" << (probe.deleted ? "true" : "false") << ",";
    out << "\"hit_count\":" << probe.hit_count << ",";
    out << "\"last_stop_reason\":" << json_escape(probe.last_stop_reason) << ",";
    out << "\"on_hit\":" << on_hit_policy_json(probe.on_hit_policy);
    out << "}";
    return out.str();
}

std::string probe_array_json(const ProbeState &probe_state, bool include_deleted) {
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto &[_, probe] : probe_state.probes_by_number) {
        if (probe.deleted && !include_deleted) {
            continue;
        }
        if (!first) {
            out << ",";
        }
        first = false;
        out << probe_info_json(probe);
    }
    out << "]";
    return out.str();
}

static std::string truncate_on_hit_response(std::string text,
                                            int max_output_bytes,
                                            int max_summary_lines) {
    if (max_summary_lines > 0) {
        int lines = 0;
        size_t pos = 0;
        while (pos < text.size()) {
            if (text[pos] == '\n') {
                ++lines;
                if (lines >= max_summary_lines) {
                    text.resize(pos + 1);
                    text += "... truncated by on_hit.max_summary_lines ...\n";
                    break;
                }
            }
            ++pos;
        }
    }
    if (max_output_bytes > 0 && text.size() > static_cast<size_t>(max_output_bytes)) {
        text.resize(static_cast<size_t>(max_output_bytes));
        text += "\n... truncated by on_hit.max_output_bytes ...\n";
    }
    return text;
}

static ProbeState::ProbeHitSnapshot prepare_probe_hit(ProbeState &probe_state,
                                                      const CommandResult &result) {
    ProbeState::ProbeHitSnapshot hit;
    hit.number = result.stop_reason == "watchpoint-trigger"
                     ? watchpoint_number_from_stop_record(result)
                     : result.breakpoint_number;
    hit.stop_reason = result.stop_reason;
    hit.signal_name = result.signal_name;
    hit.kind = result.stop_reason == "watchpoint-trigger" ? "watchpoint" : "breakpoint";
    if (hit.number.empty() && result.stop_reason == "watchpoint-trigger") {
        std::string only_active_watchpoint;
        for (const auto &[number, probe] : probe_state.probes_by_number) {
            if (probe.kind != "watchpoint" || probe.deleted || !probe.enabled) {
                continue;
            }
            if (!only_active_watchpoint.empty()) {
                only_active_watchpoint.clear();
                break;
            }
            only_active_watchpoint = number;
        }
        hit.number = only_active_watchpoint;
    }
    if (result.breakpoint_number.empty()) {
        if (hit.number.empty()) {
            return hit;
        }
    }

    auto it = probe_state.probes_by_number.find(hit.number);
    if (it != probe_state.probes_by_number.end() && !it->second.kind.empty()) {
        hit.kind = it->second.kind;
    }

    if (it != probe_state.probes_by_number.end()) {
        ProbeState::ProbeInfo &probe = it->second;
        ++probe.hit_count;
        probe.last_stop_reason = result.stop_reason;
        hit.known_probe = true;
        hit.event = probe.event;
        hit.selector = probe.selector;
        hit.location = probe.location;
        hit.expression = probe.expression;
        hit.condition = probe.condition;
        hit.comment = probe.comment;
        hit.purpose = probe.purpose;
        hit.hit_count = probe.hit_count;
        hit.on_hit_policy = probe.on_hit_policy;
    }
    return hit;
}

static std::vector<std::string> evidence_ids_since(const GdbSession &session, size_t first_index) {
    std::vector<std::string> ids;
    const auto &all = session.evidence_store().all();
    for (size_t i = first_index; i < all.size(); ++i) {
        ids.push_back(all[i].id);
    }
    return ids;
}

static std::string on_hit_action_result_json(const ProbeState::OnHitActionResult &result) {
    std::ostringstream out;
    out << "{";
    out << "\"index\":" << result.index << ",";
    out << "\"action_name\":" << json_escape(result.action_name) << ",";
    out << "\"status\":" << json_escape(result.status) << ",";
    out << "\"failure_policy\":" << json_escape(result.failure_policy) << ",";
    out << "\"evidence\":" << json_escape(result.evidence_id) << ",";
    out << "\"action_evidence_ids\":" << json_string_vector(result.action_evidence_ids) << ",";
    out << "\"error_evidence\":" << json_escape(result.error_evidence_id) << ",";
    out << "\"error\":" << json_escape(result.error) << ",";
    out << "\"skip_reason\":" << json_escape(result.skip_reason);
    out << "}";
    return out.str();
}

static std::string on_hit_action_results_json(const std::vector<ProbeState::OnHitActionResult> &results) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < results.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << on_hit_action_result_json(results[i]);
    }
    out << "]";
    return out.str();
}

static std::vector<std::string> on_hit_result_evidence_ids(const std::vector<ProbeState::OnHitActionResult> &results) {
    std::vector<std::string> ids;
    for (const auto &result : results) {
        if (!result.evidence_id.empty()) {
            ids.push_back(result.evidence_id);
        }
        for (const auto &id : result.action_evidence_ids) {
            ids.push_back(id);
        }
    }
    return ids;
}

static std::vector<std::string> on_hit_result_error_ids(const std::vector<ProbeState::OnHitActionResult> &results) {
    std::vector<std::string> ids;
    for (const auto &result : results) {
        if (!result.error_evidence_id.empty()) {
            ids.push_back(result.error_evidence_id);
        }
    }
    return ids;
}

static ProbeState::OnHitActionResult add_on_hit_wrapper_evidence(
    GdbSession &session,
    const ProbeState::ProbeHitSnapshot &hit,
    const ProbeState::OnHitPolicy &policy,
    ProbeState::OnHitActionResult result,
    const ActionRequest &action_request,
    const std::string &response_text) {
    std::ostringstream evidence_text;
    evidence_text << "{\n";
    evidence_text << "  \"probe_number\": " << json_escape(hit.number) << ",\n";
    evidence_text << "  \"probe_kind\": " << json_escape(hit.kind) << ",\n";
    evidence_text << "  \"hit_count\": " << hit.hit_count << ",\n";
    evidence_text << "  \"index\": " << result.index << ",\n";
    evidence_text << "  \"action_name\": " << json_escape(result.action_name) << ",\n";
    evidence_text << "  \"status\": " << json_escape(result.status) << ",\n";
    evidence_text << "  \"failure_policy\": " << json_escape(result.failure_policy) << ",\n";
    evidence_text << "  \"action\": " << dump_json(action_request_to_json(action_request)) << ",\n";
    evidence_text << "  \"action_evidence_ids\": " << json_string_vector(result.action_evidence_ids) << ",\n";
    evidence_text << "  \"error_evidence\": " << json_escape(result.error_evidence_id) << ",\n";
    evidence_text << "  \"error\": " << json_escape(result.error) << ",\n";
    evidence_text << "  \"skip_reason\": " << json_escape(result.skip_reason) << ",\n";
    evidence_text << "  \"response\": "
                  << json_escape(truncate_on_hit_response(response_text,
                                                          policy.max_output_bytes,
                                                          policy.max_summary_lines))
                  << "\n";
    evidence_text << "}\n";
    auto ev = session.evidence_store().add_text("OnHitAction",
                                                "On-hit action " + std::to_string(result.index) +
                                                    " for " + hit.kind + " " + hit.number,
                                                result.action_name,
                                                evidence_text.str());
    result.evidence_id = ev.id;
    return result;
}

static std::vector<ProbeState::OnHitActionResult> run_on_hit_actions(
    GdbSession &session,
    const DebugTask *task,
    SessionOutcome *outcome,
    ProbeState &probe_state,
    const ProbeState::ProbeHitSnapshot &hit,
    std::vector<ActionResult> &prelude) {
    std::vector<ProbeState::OnHitActionResult> results;
    const auto &policy = hit.on_hit_policy;
    if (hit.number.empty() || policy.actions.empty()) {
        return results;
    }

    bool stop_remaining = false;
    for (size_t i = 0; i < policy.actions.size(); ++i) {
        ProbeState::OnHitActionResult result;
        result.index = static_cast<int>(i + 1);
        result.failure_policy = policy.failure_policy;
        const ActionRequestPtr &configured_action = policy.actions[i];
        result.action_name = configured_action ? configured_action->action : "";
        if (result.action_name.empty()) {
            result.action_name = "unknown";
        }

        if (stop_remaining) {
            result.status = "skipped";
            result.skip_reason = "previous on_hit action failed with stop_on_error";
            if (configured_action) {
                result = add_on_hit_wrapper_evidence(session, hit, policy, std::move(result), *configured_action, "");
            }
            results.push_back(std::move(result));
            continue;
        }

        if (!configured_action) {
            result.status = "failed";
            result.error = "missing on_hit action";
            results.push_back(std::move(result));
            continue;
        }
        ActionRequest action_request = with_timeout_defaults(*configured_action, policy.timeout_ms);
        size_t evidence_start = session.evidence_store().all().size();
        ActionContext action_context{session, task, outcome, probe_state};
        ActionOutput action_output = execute_session_operation(
            action_context,
            action_request,
            SessionOperationOptions{SessionOperationOrigin::OnHitAction});
        std::string response_text = action_output_text(action_output);
        prelude.insert(prelude.end(), action_output.prelude.begin(), action_output.prelude.end());
        prelude.push_back(action_output.final);
        const ActionResult &action_result = action_output.final;
        result.action_evidence_ids = evidence_ids_since(session, evidence_start);
        if (!action_result.ok) {
            result.status = "failed";
            result.error = action_result.error.empty() ? "on_hit action returned ok:false" : action_result.error;
            result.error_evidence_id = action_result.evidence_id;
            if (result.error_evidence_id.empty() && !result.action_evidence_ids.empty()) {
                result.error_evidence_id = result.action_evidence_ids.back();
            }
            if (policy.failure_policy == "stop_on_error") {
                stop_remaining = true;
            }
        } else {
            result.status = "success";
        }
        result = add_on_hit_wrapper_evidence(session, hit, policy, std::move(result), action_request, response_text);
        results.push_back(std::move(result));
    }

    if (policy.continue_after_hit && !stop_remaining) {
        ProbeState::OnHitActionResult result;
        result.index = static_cast<int>(results.size() + 1);
        result.action_name = "continue_after_hit";
        result.failure_policy = policy.failure_policy;
        ActionRequest action_request;
        action_request.kind = ActionKind::Continue;
        action_request.action = "continue";
        action_request.payload = ContinuePayload{policy.timeout_ms, true};
        size_t evidence_start = session.evidence_store().all().size();
        ActionContext action_context{session, task, outcome, probe_state};
        ActionOutput action_output = execute_session_operation(
            action_context,
            action_request,
            SessionOperationOptions{SessionOperationOrigin::OnHitAction});
        std::string response_text = action_output_text(action_output);
        prelude.insert(prelude.end(), action_output.prelude.begin(), action_output.prelude.end());
        prelude.push_back(action_output.final);
        const ActionResult &action_result = action_output.final;
        result.action_evidence_ids = evidence_ids_since(session, evidence_start);
        if (!action_result.ok) {
            result.status = "failed";
            result.error = action_result.error.empty() ? "continue_after_hit returned ok:false" : action_result.error;
            result.error_evidence_id = action_result.evidence_id;
            if (result.error_evidence_id.empty() && !result.action_evidence_ids.empty()) {
                result.error_evidence_id = result.action_evidence_ids.back();
            }
        } else {
            result.status = "success";
        }
        result = add_on_hit_wrapper_evidence(session, hit, policy, std::move(result), action_request, response_text);
        results.push_back(std::move(result));
    }

    return results;
}

static void record_probe_hit(GdbSession &session,
                             const ProbeState::ProbeHitSnapshot &hit,
                             const std::vector<ProbeState::OnHitActionResult> &on_hit_results) {
    if (hit.number.empty() && hit.kind != "watchpoint") {
        return;
    }
    std::ostringstream text;
    text << "{\n";
    text << "  \"number\": " << json_escape(hit.number) << ",\n";
    text << "  \"kind\": " << json_escape(hit.kind) << ",\n";
    text << "  \"stop_reason\": " << json_escape(hit.stop_reason) << ",\n";
    text << "  \"signal\": " << json_escape(hit.signal_name) << ",\n";
    text << "  \"known_probe\": " << (hit.known_probe ? "true" : "false");
    if (hit.known_probe) {
        text << ",\n";
        text << "  \"location\": " << json_escape(hit.location) << ",\n";
        text << "  \"expression\": " << json_escape(hit.expression) << ",\n";
        text << "  \"event\": " << json_escape(hit.event) << ",\n";
        text << "  \"selector\": " << json_escape(hit.selector) << ",\n";
        text << "  \"condition\": " << json_escape(hit.condition) << ",\n";
        text << "  \"comment\": " << json_escape(hit.comment) << ",\n";
        text << "  \"purpose\": " << json_escape(hit.purpose) << ",\n";
        text << "  \"hit_count\": " << hit.hit_count << ",\n";
        text << "  \"on_hit_policy\": " << on_hit_policy_json(hit.on_hit_policy) << ",\n";
        text << "  \"on_hit_results\": " << on_hit_action_results_json(on_hit_results) << ",\n";
        text << "  \"on_hit_evidence_ids\": " << json_string_vector(on_hit_result_evidence_ids(on_hit_results)) << ",\n";
        text << "  \"on_hit_error_ids\": " << json_string_vector(on_hit_result_error_ids(on_hit_results));
    } else if (hit.kind == "watchpoint") {
        text << ",\n";
        text << "  \"attribution\": \"watchpoint stop observed but no unique active probe could be associated\"";
    }
    text << "\n}\n";

    std::string evidence_kind = "BreakpointHit";
    if (hit.kind == "watchpoint") {
        evidence_kind = "WatchpointHit";
    } else if (hit.kind == "catchpoint") {
        evidence_kind = "CatchpointHit";
    }
    session.evidence_store().add_text(evidence_kind,
                                      hit.number.empty() ? evidence_kind + " unattributed" : evidence_kind + " " + hit.number,
                                      hit.stop_reason,
                                      text.str());
}

std::vector<ActionResult> handle_probe_stop(ActionContext &context, const CommandResult &result) {
    GdbSession &session = context.session;
    const DebugTask *task = context.task;
    SessionOutcome *outcome = context.outcome;
    ProbeState &probe_state = context.probe_state;
    std::vector<ActionResult> prelude;
    auto hit = prepare_probe_hit(probe_state, result);
    if (outcome != nullptr && !hit.number.empty()) {
        outcome->state = SessionState::Stopped;
        outcome->stop_reason = hit.stop_reason;
        outcome->signal_name = hit.signal_name;
    }
    auto on_hit_results = run_on_hit_actions(session, task, outcome, probe_state, hit, prelude);
    record_probe_hit(session, hit, on_hit_results);
    return prelude;
}


static ResultObject probe_info_result_object(const ProbeState::ProbeInfo &probe) {
    ResultObject object;
    result_object_set_string(object, "number", probe.number);
    result_object_set_string(object, "kind", probe.kind);
    result_object_set_string(object, "event", probe.event);
    result_object_set_string(object, "selector", probe.selector);
    result_object_set_string(object, "location", probe.location);
    result_object_set_string(object, "expression", probe.expression);
    result_object_set_string(object, "condition", probe.condition);
    result_object_set_string(object, "comment", probe.comment);
    result_object_set_string(object, "purpose", probe.purpose);
    result_object_set_bool(object, "enabled", probe.enabled);
    result_object_set_bool(object, "deleted", probe.deleted);
    result_object_set_int(object, "hit_count", probe.hit_count);
    result_object_set_string(object, "last_stop_reason", probe.last_stop_reason);
    return object;
}

ResultArray probe_array_result(const ProbeState &probe_state, bool include_deleted) {
    ResultArray array;
    for (const auto &[_, probe] : probe_state.probes_by_number) {
        if (probe.deleted && !include_deleted) {
            continue;
        }
        array.items.push_back(probe_info_result_object(probe));
    }
    return array;
}
