#include "action.hpp"

#include <sstream>
#include <type_traits>
#include <utility>

namespace {

Json json_string(std::string value) {
    Json json;
    json.type = Json::Type::String;
    json.string_value = std::move(value);
    return json;
}

Json json_bool(bool value) {
    Json json;
    json.type = Json::Type::Bool;
    json.bool_value = value;
    return json;
}

Json json_number(int value) {
    Json json;
    json.type = Json::Type::Number;
    json.number_value = value;
    return json;
}

Json json_array() {
    Json json;
    json.type = Json::Type::Array;
    return json;
}

Json json_object() {
    Json json;
    json.type = Json::Type::Object;
    return json;
}

std::string string_field(const Json &json, std::string_view key) {
    return json.string_or(key);
}

int int_field(const Json &json, std::string_view key, int fallback) {
    const Json *direct = json.find(key);
    if (direct != nullptr && direct->is_number()) {
        return static_cast<int>(direct->number_value);
    }
    const Json *params = json.find("params");
    if (params != nullptr && params->is_object()) {
        return params->int_or(key, fallback);
    }
    return fallback;
}

bool has_field(const Json &json, std::string_view key) {
    if (json.find(key) != nullptr) {
        return true;
    }
    const Json *params = json.find("params");
    return params != nullptr && params->is_object() && params->find(key) != nullptr;
}

std::string selector_field(const Json &json, std::string_view key) {
    const Json *field = json.find(key);
    if (field == nullptr || field->is_null()) {
        return {};
    }
    if (field->is_string()) {
        return field->string_value;
    }
    if (field->is_number()) {
        long long value = static_cast<long long>(field->number_value);
        if (field->number_value == static_cast<double>(value)) {
            return std::to_string(value);
        }
    }
    return {};
}

const Json *on_hit_json_from(const Json &action) {
    const Json *on_hit = action.find("on_hit");
    if (on_hit == nullptr) {
        const Json *params = action.find("params");
        if (params != nullptr && params->is_object()) {
            on_hit = params->find("on_hit");
        }
    }
    return on_hit;
}

bool valid_on_hit_failure_policy(const std::string &policy) {
    return policy == "continue_on_error" || policy == "stop_on_error";
}

ActionRequestPtr parse_on_hit_action(const Json &item, std::string &error) {
    if (!item.is_object()) {
        error = "on_hit actions must be JSON objects";
        return nullptr;
    }
    std::string action_name = item.string_or("action");
    if (action_name == "raw_mi") {
        error = "on_hit raw_mi action is not allowed";
        return nullptr;
    }
    auto parsed = std::make_shared<ActionRequest>(parse_action_request(item));
    return parsed;
}

bool parse_on_hit_actions_array(const Json &actions_json,
                                std::vector<ActionRequestPtr> &actions,
                                std::string &error) {
    if (!actions_json.is_array()) {
        error = "on_hit actions must be an array";
        return false;
    }
    for (const auto &item : actions_json.array_value) {
        ActionRequestPtr parsed = parse_on_hit_action(item, error);
        if (!parsed) {
            return false;
        }
        actions.push_back(std::move(parsed));
    }
    return true;
}

OnHitPolicyRequest parse_on_hit_policy(const Json &action, std::string &error) {
    OnHitPolicyRequest policy;
    const Json *on_hit = on_hit_json_from(action);
    if (on_hit == nullptr || on_hit->is_null()) {
        return policy;
    }

    policy.configured = true;
    if (on_hit->is_array()) {
        parse_on_hit_actions_array(*on_hit, policy.actions, error);
        policy.error = error;
        return policy;
    }
    if (!on_hit->is_object()) {
        error = "on_hit must be an array or policy object";
        policy.error = error;
        return policy;
    }

    const Json *actions = on_hit->find("actions");
    if (actions != nullptr && !parse_on_hit_actions_array(*actions, policy.actions, error)) {
        policy.error = error;
        return policy;
    }
    if (const Json *timeout = on_hit->find("timeout_ms"); timeout != nullptr) {
        if (!timeout->is_number() || timeout->number_value <= 0) {
            error = "on_hit.timeout_ms must be a positive number";
            policy.error = error;
            return policy;
        }
        policy.timeout_ms = static_cast<int>(timeout->number_value);
    }
    if (const Json *max_output = on_hit->find("max_output_bytes"); max_output != nullptr) {
        if (!max_output->is_number() || max_output->number_value <= 0) {
            error = "on_hit.max_output_bytes must be a positive number";
            policy.error = error;
            return policy;
        }
        policy.max_output_bytes = static_cast<int>(max_output->number_value);
    }
    if (const Json *max_lines = on_hit->find("max_summary_lines"); max_lines != nullptr) {
        if (!max_lines->is_number() || max_lines->number_value <= 0) {
            error = "on_hit.max_summary_lines must be a positive number";
            policy.error = error;
            return policy;
        }
        policy.max_summary_lines = static_cast<int>(max_lines->number_value);
    }
    std::string failure_policy = on_hit->string_or("failure_policy");
    if (!failure_policy.empty()) {
        if (!valid_on_hit_failure_policy(failure_policy)) {
            error = "on_hit.failure_policy must be continue_on_error or stop_on_error";
            policy.error = error;
            return policy;
        }
        policy.failure_policy = failure_policy;
    }
    policy.continue_after_hit = on_hit->bool_or("continue_after_hit", false);
    return policy;
}

Json field_value_to_json(const ResultField::Value &value);

Json on_hit_policy_to_json(const OnHitPolicyRequest &policy) {
    if (!policy.configured) {
        return {};
    }
    Json json = json_object();
    Json actions = json_array();
    for (const auto &action : policy.actions) {
        if (action) {
            actions.array_value.push_back(action_request_to_json(*action));
        }
    }
    json.object_value["actions"] = std::move(actions);
    json.object_value["timeout_ms"] = json_number(policy.timeout_ms);
    json.object_value["max_output_bytes"] = json_number(policy.max_output_bytes);
    json.object_value["max_summary_lines"] = json_number(policy.max_summary_lines);
    json.object_value["failure_policy"] = json_string(policy.failure_policy);
    json.object_value["continue_after_hit"] = json_bool(policy.continue_after_hit);
    return json;
}

Json result_object_to_json(const ResultObject &object) {
    Json json = json_object();
    for (const auto &field : object.fields) {
        json.object_value[field.key] = field_value_to_json(field.value);
    }
    return json;
}

Json result_array_to_json(const ResultArray &array) {
    Json json = json_array();
    for (const auto &item : array.items) {
        json.array_value.push_back(result_object_to_json(item));
    }
    return json;
}

Json field_value_to_json(const ResultField::Value &value) {
    if (std::holds_alternative<std::nullptr_t>(value)) {
        return {};
    }
    if (const auto *text = std::get_if<std::string>(&value)) {
        return json_string(*text);
    }
    if (const auto *number = std::get_if<int>(&value)) {
        return json_number(*number);
    }
    if (const auto *flag = std::get_if<bool>(&value)) {
        return json_bool(*flag);
    }
    if (const auto *object = std::get_if<ResultObjectPtr>(&value)) {
        return *object ? result_object_to_json(**object) : Json{};
    }
    if (const auto *array = std::get_if<ResultArrayPtr>(&value)) {
        return *array ? result_array_to_json(**array) : Json{};
    }
    return {};
}

void set_field(std::vector<ResultField> &fields, const std::string &key, ResultField::Value value) {
    fields.push_back(ResultField{key, std::move(value)});
}

template <typename Payload>
const Payload *payload_if(const ActionRequest &request) {
    return std::get_if<Payload>(&request.payload);
}

} // namespace

ActionResult ActionResult::success(ActionKind kind, std::string action) {
    ActionResult result;
    result.ok = true;
    result.kind = kind;
    result.action = std::move(action);
    return result;
}

ActionResult ActionResult::failure(ActionKind kind, std::string action, std::string error) {
    ActionResult result;
    result.ok = false;
    result.kind = kind;
    result.action = std::move(action);
    result.error = std::move(error);
    return result;
}

const char *action_kind_name(ActionKind kind) {
    switch (kind) {
        case ActionKind::FinishSession: return "finish_session";
        case ActionKind::Backtrace: return "backtrace";
        case ActionKind::Locals: return "locals";
        case ActionKind::Registers: return "registers";
        case ActionKind::Threads: return "threads";
        case ActionKind::ArgsInfo: return "args_info";
        case ActionKind::FrameSelect: return "frame_select";
        case ActionKind::Evaluate: return "evaluate";
        case ActionKind::BreakpointSet: return "breakpoint_set";
        case ActionKind::WatchpointSet: return "watchpoint_set";
        case ActionKind::CatchpointSet: return "catchpoint_set";
        case ActionKind::ProbeList: return "probe_list";
        case ActionKind::ProbeEnable: return "probe_enable";
        case ActionKind::ProbeDisable: return "probe_disable";
        case ActionKind::ProbeDelete: return "probe_delete";
        case ActionKind::Run: return "run";
        case ActionKind::Continue: return "continue";
        case ActionKind::SaveAction: return "save_action";
        case ActionKind::Replay: return "replay";
        case ActionKind::HypothesisCreate: return "hypothesis_create";
        case ActionKind::HypothesisCheck: return "hypothesis_check";
        case ActionKind::HypothesisConclude: return "hypothesis_conclude";
        case ActionKind::RawMi: return "raw_mi";
        case ActionKind::RecordStart: return "record_start";
        case ActionKind::RecordStatus: return "record_status";
        case ActionKind::RecordStop: return "record_stop";
        case ActionKind::RecordDiscard: return "record_discard";
        case ActionKind::Unknown: return "";
    }
    return "";
}

ActionKind action_kind_from_name(const std::string &name) {
    if (name == "finish_session" || name == "finish") return ActionKind::FinishSession;
    if (name == "backtrace") return ActionKind::Backtrace;
    if (name == "locals") return ActionKind::Locals;
    if (name == "registers") return ActionKind::Registers;
    if (name == "threads") return ActionKind::Threads;
    if (name == "args_info") return ActionKind::ArgsInfo;
    if (name == "frame_select") return ActionKind::FrameSelect;
    if (name == "evaluate") return ActionKind::Evaluate;
    if (name == "breakpoint_set") return ActionKind::BreakpointSet;
    if (name == "watchpoint_set") return ActionKind::WatchpointSet;
    if (name == "catchpoint_set") return ActionKind::CatchpointSet;
    if (name == "probe_list") return ActionKind::ProbeList;
    if (name == "probe_enable") return ActionKind::ProbeEnable;
    if (name == "probe_disable") return ActionKind::ProbeDisable;
    if (name == "probe_delete") return ActionKind::ProbeDelete;
    if (name == "run") return ActionKind::Run;
    if (name == "continue") return ActionKind::Continue;
    if (name == "save_action") return ActionKind::SaveAction;
    if (name == "replay") return ActionKind::Replay;
    if (name == "hypothesis_create") return ActionKind::HypothesisCreate;
    if (name == "hypothesis_check") return ActionKind::HypothesisCheck;
    if (name == "hypothesis_conclude") return ActionKind::HypothesisConclude;
    if (name == "raw_mi") return ActionKind::RawMi;
    if (name == "record_start") return ActionKind::RecordStart;
    if (name == "record_status") return ActionKind::RecordStatus;
    if (name == "record_stop") return ActionKind::RecordStop;
    if (name == "record_discard") return ActionKind::RecordDiscard;
    return ActionKind::Unknown;
}

ActionRequest parse_action_request(const Json &json) {
    ActionRequest request;
    request.action = json.string_or("action");
    request.kind = action_kind_from_name(request.action);
    std::string on_hit_error;

    switch (request.kind) {
        case ActionKind::FinishSession: {
            FinishPayload payload;
            payload.agent_inference = string_field(json, "agent_inference");
            payload.final_conclusion = string_field(json, "final_conclusion");
            if (payload.final_conclusion.empty()) {
                payload.final_conclusion = string_field(json, "final_agent_conclusion");
            }
            request.payload = std::move(payload);
            break;
        }
        case ActionKind::Backtrace:
        case ActionKind::Locals:
        case ActionKind::Registers:
        case ActionKind::Threads:
        case ActionKind::ArgsInfo:
            request.payload = TimeoutPayload{int_field(json, "timeout_ms", 5000),
                                             has_field(json, "timeout_ms")};
            break;
        case ActionKind::FrameSelect:
            request.payload = FrameSelectPayload{int_field(json, "frame", 0),
                                                 int_field(json, "timeout_ms", 5000),
                                                 has_field(json, "timeout_ms")};
            break;
        case ActionKind::Evaluate:
            request.payload = EvaluatePayload{string_field(json, "expression"),
                                              int_field(json, "timeout_ms", 5000),
                                              has_field(json, "timeout_ms")};
            break;
        case ActionKind::BreakpointSet: {
            BreakpointSetPayload payload;
            payload.location = string_field(json, "location");
            payload.condition = string_field(json, "condition");
            payload.comment = string_field(json, "comment");
            payload.purpose = string_field(json, "purpose");
            payload.on_hit = parse_on_hit_policy(json, on_hit_error);
            request.payload = std::move(payload);
            break;
        }
        case ActionKind::WatchpointSet: {
            WatchpointSetPayload payload;
            payload.expression = string_field(json, "expression");
            payload.condition = string_field(json, "condition");
            payload.comment = string_field(json, "comment");
            payload.purpose = string_field(json, "purpose");
            payload.on_hit = parse_on_hit_policy(json, on_hit_error);
            request.payload = std::move(payload);
            break;
        }
        case ActionKind::CatchpointSet: {
            CatchpointSetPayload payload;
            payload.event = string_field(json, "event");
            if (payload.event == "syscall") {
                const Json *name_field = json.find("name");
                const Json *syscall_field = json.find("syscall");
                if (name_field != nullptr && !name_field->is_null()) {
                    payload.selector = selector_field(json, "name");
                } else if (syscall_field != nullptr && !syscall_field->is_null()) {
                    payload.selector = selector_field(json, "syscall");
                }
            }
            payload.comment = string_field(json, "comment");
            payload.purpose = string_field(json, "purpose");
            payload.on_hit = parse_on_hit_policy(json, on_hit_error);
            request.payload = std::move(payload);
            break;
        }
        case ActionKind::ProbeEnable:
        case ActionKind::ProbeDisable:
        case ActionKind::ProbeDelete:
            request.payload = ProbeMutationPayload{int_field(json, "number", -1)};
            break;
        case ActionKind::Run:
            request.payload = RunPayload{int_field(json, "deadline_ms", -1),
                                         has_field(json, "deadline_ms"),
                                         string_field(json, "stdin")};
            break;
        case ActionKind::Continue:
            request.payload = ContinuePayload{int_field(json, "deadline_ms", 30000),
                                              has_field(json, "deadline_ms")};
            break;
        case ActionKind::RawMi:
            request.payload = RawMiPayload{string_field(json, "command"),
                                           string_field(json, "risk"),
                                           int_field(json, "timeout_ms", 5000),
                                           has_field(json, "timeout_ms")};
            break;
        case ActionKind::HypothesisCreate:
            request.payload = HypothesisCreatePayload{string_field(json, "id"),
                                                      string_field(json, "title"),
                                                      string_field(json, "description")};
            break;
        case ActionKind::HypothesisCheck: {
            HypothesisCheckPayload payload;
            payload.hypothesis = string_field(json, "hypothesis");
            payload.expression = string_field(json, "expression");
            payload.description = string_field(json, "description");
            payload.assertion = string_field(json, "assertion");
            if (payload.assertion.empty()) {
                payload.assertion = "none";
            }
            payload.expected = string_field(json, "expected");
            request.payload = std::move(payload);
            break;
        }
        case ActionKind::HypothesisConclude: {
            HypothesisConcludePayload payload;
            payload.hypothesis = string_field(json, "hypothesis");
            payload.conclusion = string_field(json, "conclusion");
            if (payload.conclusion.empty()) {
                payload.conclusion = "Inconclusive";
            }
            payload.inference = string_field(json, "inference");
            request.payload = std::move(payload);
            break;
        }
        case ActionKind::SaveAction: {
            SaveActionPayload payload;
            payload.name = string_field(json, "name");
            payload.failure_policy = string_field(json, "failure_policy");
            if (const Json *saved = json.find("saved_action"); saved != nullptr) {
                if (saved->is_object()) {
                    payload.saved_action = std::make_shared<ActionRequest>(parse_action_request(*saved));
                } else if (saved->is_string()) {
                    try {
                        Json saved_json = parse_json(saved->string_value);
                        if (saved_json.is_object()) {
                            payload.saved_action = std::make_shared<ActionRequest>(parse_action_request(saved_json));
                        }
                    } catch (const std::exception &) {
                    }
                }
            }
            request.payload = std::move(payload);
            break;
        }
        case ActionKind::Replay: {
            ReplayPayload payload;
            payload.file = string_field(json, "file");
            payload.name = string_field(json, "name");
            payload.force = json.bool_or("force", false);
            if (const Json *params = json.find("params"); params != nullptr && params->is_object()) {
                payload.force = params->bool_or("force", payload.force);
            }
            payload.failure_policy = string_field(json, "failure_policy");
            request.payload = std::move(payload);
            break;
        }
        case ActionKind::RecordStart: {
            RecordStartPayload payload;
            payload.name = string_field(json, "name");
            payload.failure_policy = string_field(json, "failure_policy");
            payload.include_raw_mi = json.bool_or("include_raw_mi", false);
            request.payload = std::move(payload);
            break;
        }
        case ActionKind::RecordStatus:
        case ActionKind::RecordStop:
        case ActionKind::RecordDiscard:
            request.payload = NoPayload{};
            break;
        case ActionKind::ProbeList:
            request.payload = NoPayload{};
            break;
        case ActionKind::Unknown:
            request.payload = NoPayload{};
            break;
    }
    return request;
}

Json action_request_to_json(const ActionRequest &request) {
    Json json = json_object();
    json.object_value["action"] = json_string(request.action.empty() ? action_kind_name(request.kind) : request.action);
    std::visit([&json](const auto &payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, TimeoutPayload>) {
            if (payload.timeout_ms_set || payload.timeout_ms != 5000) json.object_value["timeout_ms"] = json_number(payload.timeout_ms);
        } else if constexpr (std::is_same_v<T, FrameSelectPayload>) {
            json.object_value["frame"] = json_number(payload.frame);
            if (payload.timeout_ms_set || payload.timeout_ms != 5000) json.object_value["timeout_ms"] = json_number(payload.timeout_ms);
        } else if constexpr (std::is_same_v<T, EvaluatePayload>) {
            json.object_value["expression"] = json_string(payload.expression);
            if (payload.timeout_ms_set || payload.timeout_ms != 5000) json.object_value["timeout_ms"] = json_number(payload.timeout_ms);
        } else if constexpr (std::is_same_v<T, RunPayload>) {
            if (payload.deadline_ms_set || payload.deadline_ms > 0) json.object_value["deadline_ms"] = json_number(payload.deadline_ms);
            if (!payload.stdin_path.empty()) json.object_value["stdin"] = json_string(payload.stdin_path);
        } else if constexpr (std::is_same_v<T, ContinuePayload>) {
            if (payload.deadline_ms_set || payload.deadline_ms != 30000) json.object_value["deadline_ms"] = json_number(payload.deadline_ms);
        } else if constexpr (std::is_same_v<T, RawMiPayload>) {
            json.object_value["command"] = json_string(payload.command);
            json.object_value["risk"] = json_string(payload.risk);
            if (payload.timeout_ms_set || payload.timeout_ms != 5000) json.object_value["timeout_ms"] = json_number(payload.timeout_ms);
        } else if constexpr (std::is_same_v<T, BreakpointSetPayload>) {
            json.object_value["location"] = json_string(payload.location);
            if (!payload.condition.empty()) json.object_value["condition"] = json_string(payload.condition);
            if (!payload.comment.empty()) json.object_value["comment"] = json_string(payload.comment);
            if (!payload.purpose.empty()) json.object_value["purpose"] = json_string(payload.purpose);
            if (payload.on_hit.configured) json.object_value["on_hit"] = on_hit_policy_to_json(payload.on_hit);
        } else if constexpr (std::is_same_v<T, WatchpointSetPayload>) {
            json.object_value["expression"] = json_string(payload.expression);
            if (!payload.condition.empty()) json.object_value["condition"] = json_string(payload.condition);
            if (!payload.comment.empty()) json.object_value["comment"] = json_string(payload.comment);
            if (!payload.purpose.empty()) json.object_value["purpose"] = json_string(payload.purpose);
            if (payload.on_hit.configured) json.object_value["on_hit"] = on_hit_policy_to_json(payload.on_hit);
        } else if constexpr (std::is_same_v<T, CatchpointSetPayload>) {
            json.object_value["event"] = json_string(payload.event);
            if (!payload.selector.empty()) json.object_value["selector"] = json_string(payload.selector);
            if (!payload.comment.empty()) json.object_value["comment"] = json_string(payload.comment);
            if (!payload.purpose.empty()) json.object_value["purpose"] = json_string(payload.purpose);
            if (payload.on_hit.configured) json.object_value["on_hit"] = on_hit_policy_to_json(payload.on_hit);
        } else if constexpr (std::is_same_v<T, ProbeMutationPayload>) {
            json.object_value["number"] = json_number(payload.number);
        } else if constexpr (std::is_same_v<T, HypothesisCreatePayload>) {
            if (!payload.id.empty()) json.object_value["id"] = json_string(payload.id);
            if (!payload.title.empty()) json.object_value["title"] = json_string(payload.title);
            if (!payload.description.empty()) json.object_value["description"] = json_string(payload.description);
        } else if constexpr (std::is_same_v<T, HypothesisCheckPayload>) {
            json.object_value["hypothesis"] = json_string(payload.hypothesis);
            json.object_value["expression"] = json_string(payload.expression);
            if (!payload.description.empty()) json.object_value["description"] = json_string(payload.description);
            if (!payload.assertion.empty() && payload.assertion != "none") json.object_value["assertion"] = json_string(payload.assertion);
            if (!payload.expected.empty()) json.object_value["expected"] = json_string(payload.expected);
        } else if constexpr (std::is_same_v<T, HypothesisConcludePayload>) {
            json.object_value["hypothesis"] = json_string(payload.hypothesis);
            if (!payload.conclusion.empty()) json.object_value["conclusion"] = json_string(payload.conclusion);
            if (!payload.inference.empty()) json.object_value["inference"] = json_string(payload.inference);
        } else if constexpr (std::is_same_v<T, SaveActionPayload>) {
            json.object_value["name"] = json_string(payload.name);
            if (!payload.failure_policy.empty()) json.object_value["failure_policy"] = json_string(payload.failure_policy);
            if (payload.saved_action) json.object_value["saved_action"] = action_request_to_json(*payload.saved_action);
        } else if constexpr (std::is_same_v<T, ReplayPayload>) {
            if (!payload.file.empty()) json.object_value["file"] = json_string(payload.file);
            if (!payload.name.empty()) json.object_value["name"] = json_string(payload.name);
            if (payload.force) json.object_value["force"] = json_bool(payload.force);
            if (!payload.failure_policy.empty()) json.object_value["failure_policy"] = json_string(payload.failure_policy);
        } else if constexpr (std::is_same_v<T, RecordStartPayload>) {
            if (!payload.name.empty()) json.object_value["name"] = json_string(payload.name);
            if (!payload.failure_policy.empty()) json.object_value["failure_policy"] = json_string(payload.failure_policy);
            if (payload.include_raw_mi) json.object_value["include_raw_mi"] = json_bool(payload.include_raw_mi);
        } else if constexpr (std::is_same_v<T, FinishPayload>) {
            if (!payload.agent_inference.empty()) json.object_value["agent_inference"] = json_string(payload.agent_inference);
            if (!payload.final_conclusion.empty()) json.object_value["final_conclusion"] = json_string(payload.final_conclusion);
        }
    }, request.payload);
    return json;
}

Json action_result_to_json(const ActionResult &result) {
    Json json = json_object();
    json.object_value["ok"] = json_bool(result.ok);
    if (!result.action.empty()) {
        json.object_value["action"] = json_string(result.action);
    }
    if (!result.error.empty()) {
        json.object_value["error"] = json_string(result.error);
    }
    if (!result.evidence_id.empty()) {
        json.object_value["evidence"] = json_string(result.evidence_id);
    }
    if (!result.command_evidence_id.empty()) {
        json.object_value["command_evidence"] = json_string(result.command_evidence_id);
    }
    for (const auto &field : result.fields) {
        json.object_value[field.key] = field_value_to_json(field.value);
    }
    return json;
}

std::string action_result_line(const ActionResult &result) {
    return dump_json(action_result_to_json(result)) + "\n";
}

std::string action_output_text(const ActionOutput &output) {
    std::string out;
    for (const auto &result : output.prelude) {
        out += action_result_line(result);
    }
    out += action_result_line(output.final);
    return out;
}

void action_result_set_string(ActionResult &result, const std::string &key, const std::string &value) {
    set_field(result.fields, key, value);
}

void action_result_set_int(ActionResult &result, const std::string &key, int value) {
    set_field(result.fields, key, value);
}

void action_result_set_bool(ActionResult &result, const std::string &key, bool value) {
    set_field(result.fields, key, value);
}

void action_result_set_null(ActionResult &result, const std::string &key) {
    set_field(result.fields, key, nullptr);
}

void action_result_set_object(ActionResult &result, const std::string &key, ResultObject value) {
    set_field(result.fields, key, std::make_shared<ResultObject>(std::move(value)));
}

void action_result_set_array(ActionResult &result, const std::string &key, ResultArray value) {
    set_field(result.fields, key, std::make_shared<ResultArray>(std::move(value)));
}

void result_object_set_string(ResultObject &object, const std::string &key, const std::string &value) {
    set_field(object.fields, key, value);
}

void result_object_set_int(ResultObject &object, const std::string &key, int value) {
    set_field(object.fields, key, value);
}

void result_object_set_bool(ResultObject &object, const std::string &key, bool value) {
    set_field(object.fields, key, value);
}

void result_object_set_null(ResultObject &object, const std::string &key) {
    set_field(object.fields, key, nullptr);
}

void result_object_set_object(ResultObject &object, const std::string &key, ResultObject value) {
    set_field(object.fields, key, std::make_shared<ResultObject>(std::move(value)));
}

void result_object_set_array(ResultObject &object, const std::string &key, ResultArray value) {
    set_field(object.fields, key, std::make_shared<ResultArray>(std::move(value)));
}

ActionRequest with_timeout_defaults(ActionRequest request, int timeout_ms) {
    std::visit([timeout_ms](auto &payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, TimeoutPayload>) {
            if (!payload.timeout_ms_set) {
                payload.timeout_ms = timeout_ms;
                payload.timeout_ms_set = true;
            }
        } else if constexpr (std::is_same_v<T, FrameSelectPayload>) {
            if (!payload.timeout_ms_set) {
                payload.timeout_ms = timeout_ms;
                payload.timeout_ms_set = true;
            }
        } else if constexpr (std::is_same_v<T, EvaluatePayload>) {
            if (!payload.timeout_ms_set) {
                payload.timeout_ms = timeout_ms;
                payload.timeout_ms_set = true;
            }
        } else if constexpr (std::is_same_v<T, RunPayload>) {
            if (!payload.deadline_ms_set || payload.deadline_ms <= 0) {
                payload.deadline_ms = timeout_ms;
                payload.deadline_ms_set = true;
            }
        } else if constexpr (std::is_same_v<T, ContinuePayload>) {
            if (!payload.deadline_ms_set) {
                payload.deadline_ms = timeout_ms;
                payload.deadline_ms_set = true;
            }
        } else if constexpr (std::is_same_v<T, RawMiPayload>) {
            if (!payload.timeout_ms_set) {
                payload.timeout_ms = timeout_ms;
                payload.timeout_ms_set = true;
            }
        }
    }, request.payload);
    return request;
}
