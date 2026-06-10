#include "action.hpp"

#include <sstream>
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

} // namespace

ActionResult ActionResult::success(ActionKind kind, std::string action) {
    ActionResult result;
    result.ok = true;
    result.kind = kind;
    result.action = std::move(action);
    result.fields.type = Json::Type::Object;
    return result;
}

ActionResult ActionResult::failure(ActionKind kind, std::string action, std::string error) {
    ActionResult result;
    result.ok = false;
    result.kind = kind;
    result.action = std::move(action);
    result.error = std::move(error);
    result.fields.type = Json::Type::Object;
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
    return ActionKind::Unknown;
}

ActionRequest parse_action_request(const Json &json) {
    ActionRequest request;
    request.action = json.string_or("action");
    request.kind = action_kind_from_name(request.action);
    request.expression = string_field(json, "expression");
    request.location = string_field(json, "location");
    request.event = string_field(json, "event");
    request.condition = string_field(json, "condition");
    request.comment = string_field(json, "comment");
    request.purpose = string_field(json, "purpose");
    request.command = string_field(json, "command");
    request.risk = string_field(json, "risk");
    request.stdin_path = string_field(json, "stdin");
    request.hypothesis = string_field(json, "hypothesis");
    request.id = string_field(json, "id");
    request.title = string_field(json, "title");
    request.description = string_field(json, "description");
    request.assertion = string_field(json, "assertion");
    request.expected = string_field(json, "expected");
    request.conclusion = string_field(json, "conclusion");
    request.inference = string_field(json, "inference");
    request.name = string_field(json, "name");
    request.file = string_field(json, "file");
    request.failure_policy = string_field(json, "failure_policy");
    request.timeout_ms = int_field(json, "timeout_ms", 5000);
    request.deadline_ms = int_field(json, "deadline_ms", -1);
    request.frame = int_field(json, "frame", 0);
    request.number = int_field(json, "number", -1);
    request.force = json.bool_or("force", false);
    if (const Json *params = json.find("params"); params != nullptr && params->is_object()) {
        request.force = params->bool_or("force", request.force);
    }
    if (request.event == "syscall") {
        const Json *name_field = json.find("name");
        const Json *syscall_field = json.find("syscall");
        if (name_field != nullptr && !name_field->is_null()) {
            request.selector = selector_field(json, "name");
        } else if (syscall_field != nullptr && !syscall_field->is_null()) {
            request.selector = selector_field(json, "syscall");
        }
    }
    if (const Json *on_hit = json.find("on_hit"); on_hit != nullptr && !on_hit->is_null()) {
        request.has_on_hit = true;
        request.on_hit = *on_hit;
    }
    if (const Json *saved = json.find("saved_action"); saved != nullptr && !saved->is_null()) {
        request.has_saved_action = true;
        request.saved_action = *saved;
    }
    return request;
}

Json action_result_to_json(const ActionResult &result) {
    Json json;
    json.type = Json::Type::Object;
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
    if (result.fields.is_object()) {
        for (const auto &[key, value] : result.fields.object_value) {
            json.object_value[key] = value;
        }
    }
    return json;
}

std::string action_result_line(const ActionResult &result) {
    std::string out;
    for (const auto &response : result.prelude_responses) {
        out += dump_json(response);
        out += "\n";
    }
    out += dump_json(action_result_to_json(result));
    out += "\n";
    return out;
}

void action_result_set_string(ActionResult &result, const std::string &key, const std::string &value) {
    if (!result.fields.is_object()) {
        result.fields.type = Json::Type::Object;
    }
    result.fields.object_value[key] = json_string(value);
}

void action_result_set_int(ActionResult &result, const std::string &key, int value) {
    if (!result.fields.is_object()) {
        result.fields.type = Json::Type::Object;
    }
    result.fields.object_value[key] = json_number(value);
}

void action_result_set_bool(ActionResult &result, const std::string &key, bool value) {
    if (!result.fields.is_object()) {
        result.fields.type = Json::Type::Object;
    }
    result.fields.object_value[key] = json_bool(value);
}

void action_result_set_json(ActionResult &result, const std::string &key, const Json &value) {
    if (!result.fields.is_object()) {
        result.fields.type = Json::Type::Object;
    }
    result.fields.object_value[key] = value;
}
