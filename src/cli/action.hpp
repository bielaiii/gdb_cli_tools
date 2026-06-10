#pragma once

#include "../common/json.hpp"

#include <string>
#include <vector>

enum class ActionKind {
    Unknown,
    FinishSession,
    Backtrace,
    Locals,
    Registers,
    Threads,
    ArgsInfo,
    FrameSelect,
    Evaluate,
    BreakpointSet,
    WatchpointSet,
    CatchpointSet,
    ProbeList,
    ProbeEnable,
    ProbeDisable,
    ProbeDelete,
    Run,
    Continue,
    SaveAction,
    Replay,
    HypothesisCreate,
    HypothesisCheck,
    HypothesisConclude,
    RawMi,
};

struct ActionRequest {
    ActionKind kind = ActionKind::Unknown;
    std::string action;

    std::string expression;
    std::string location;
    std::string event;
    std::string selector;
    std::string condition;
    std::string comment;
    std::string purpose;
    std::string command;
    std::string risk;
    std::string stdin_path;
    std::string hypothesis;
    std::string id;
    std::string title;
    std::string description;
    std::string assertion;
    std::string expected;
    std::string conclusion;
    std::string inference;
    std::string name;
    std::string file;
    std::string failure_policy;

    int timeout_ms = 5000;
    int deadline_ms = -1;
    int frame = 0;
    int number = -1;
    bool force = false;

    bool has_on_hit = false;
    Json on_hit;
    bool has_saved_action = false;
    Json saved_action;
};

struct ActionResult {
    bool ok = false;
    ActionKind kind = ActionKind::Unknown;
    std::string action;
    std::string error;
    std::string evidence_id;
    std::string command_evidence_id;
    bool finished = false;
    Json fields;
    std::vector<Json> prelude_responses;

    static ActionResult success(ActionKind kind, std::string action);
    static ActionResult failure(ActionKind kind, std::string action, std::string error);
};

const char *action_kind_name(ActionKind kind);
ActionKind action_kind_from_name(const std::string &name);
ActionRequest parse_action_request(const Json &json);

Json action_result_to_json(const ActionResult &result);
std::string action_result_line(const ActionResult &result);

void action_result_set_string(ActionResult &result, const std::string &key, const std::string &value);
void action_result_set_int(ActionResult &result, const std::string &key, int value);
void action_result_set_bool(ActionResult &result, const std::string &key, bool value);
void action_result_set_json(ActionResult &result, const std::string &key, const Json &value);
