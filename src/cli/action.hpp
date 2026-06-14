#pragma once

#include "../common/json.hpp"

#include <memory>
#include <string>
#include <variant>
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
    RecordStart,
    RecordStatus,
    RecordStop,
    RecordDiscard,
};

struct ActionRequest;
using ActionRequestPtr = std::shared_ptr<ActionRequest>;

struct OnHitPolicyRequest {
    bool configured = false;
    std::vector<ActionRequestPtr> actions;
    int timeout_ms = 5000;
    int max_output_bytes = 8192;
    int max_summary_lines = 80;
    std::string failure_policy = "continue_on_error";
    bool continue_after_hit = false;
    std::string error;
};

struct NoPayload {};
struct TimeoutPayload { int timeout_ms = 5000; bool timeout_ms_set = false; };
struct FinishPayload { std::string agent_inference; std::string final_conclusion; };
struct FrameSelectPayload { int frame = 0; int timeout_ms = 5000; bool timeout_ms_set = false; };
struct EvaluatePayload { std::string expression; int timeout_ms = 5000; bool timeout_ms_set = false; };
struct BreakpointSetPayload {
    std::string location;
    std::string condition;
    std::string comment;
    std::string purpose;
    OnHitPolicyRequest on_hit;
};
struct WatchpointSetPayload {
    std::string expression;
    std::string condition;
    std::string comment;
    std::string purpose;
    OnHitPolicyRequest on_hit;
};
struct CatchpointSetPayload {
    std::string event;
    std::string selector;
    std::string comment;
    std::string purpose;
    OnHitPolicyRequest on_hit;
};
struct ProbeMutationPayload { int number = -1; };
struct RunPayload { int deadline_ms = -1; bool deadline_ms_set = false; std::string stdin_path; };
struct ContinuePayload { int deadline_ms = 30000; bool deadline_ms_set = false; };
struct RawMiPayload { std::string command; std::string risk; int timeout_ms = 5000; bool timeout_ms_set = false; };
struct HypothesisCreatePayload { std::string id; std::string title; std::string description; };
struct HypothesisCheckPayload {
    std::string hypothesis;
    std::string expression;
    std::string description;
    std::string assertion = "none";
    std::string expected;
};
struct HypothesisConcludePayload {
    std::string hypothesis;
    std::string conclusion = "Inconclusive";
    std::string inference;
};
struct SaveActionPayload {
    std::string name;
    std::string failure_policy;
    ActionRequestPtr saved_action;
};
struct ReplayPayload {
    std::string file;
    std::string name;
    bool force = false;
    std::string failure_policy;
};
struct RecordStartPayload {
    std::string name;
    std::string failure_policy;
    bool include_raw_mi = false;
};

using ActionPayload = std::variant<
    NoPayload,
    TimeoutPayload,
    FinishPayload,
    FrameSelectPayload,
    EvaluatePayload,
    BreakpointSetPayload,
    WatchpointSetPayload,
    CatchpointSetPayload,
    ProbeMutationPayload,
    RunPayload,
    ContinuePayload,
    RawMiPayload,
    HypothesisCreatePayload,
    HypothesisCheckPayload,
    HypothesisConcludePayload,
    SaveActionPayload,
    ReplayPayload,
    RecordStartPayload>;

struct ActionRequest {
    ActionKind kind = ActionKind::Unknown;
    std::string action;
    ActionPayload payload;
};

struct ResultObject;
struct ResultArray;
using ResultObjectPtr = std::shared_ptr<ResultObject>;
using ResultArrayPtr = std::shared_ptr<ResultArray>;

struct ResultField {
    using Value = std::variant<std::nullptr_t, std::string, int, bool, ResultObjectPtr, ResultArrayPtr>;
    std::string key;
    Value value = nullptr;
};

struct ResultObject {
    std::vector<ResultField> fields;
};

struct ResultArray {
    std::vector<ResultObject> items;
};

struct ActionResult {
    bool ok = false;
    ActionKind kind = ActionKind::Unknown;
    std::string action;
    std::string error;
    std::string evidence_id;
    std::string command_evidence_id;
    bool finished = false;
    std::vector<ResultField> fields;

    static ActionResult success(ActionKind kind, std::string action);
    static ActionResult failure(ActionKind kind, std::string action, std::string error);
};

struct ActionOutput {
    std::vector<ActionResult> prelude;
    ActionResult final;
};

const char *action_kind_name(ActionKind kind);
ActionKind action_kind_from_name(const std::string &name);
ActionRequest parse_action_request(const Json &json);

Json action_request_to_json(const ActionRequest &request);
Json action_result_to_json(const ActionResult &result);
std::string action_result_line(const ActionResult &result);
std::string action_output_text(const ActionOutput &output);

void action_result_set_string(ActionResult &result, const std::string &key, const std::string &value);
void action_result_set_int(ActionResult &result, const std::string &key, int value);
void action_result_set_bool(ActionResult &result, const std::string &key, bool value);
void action_result_set_null(ActionResult &result, const std::string &key);
void action_result_set_object(ActionResult &result, const std::string &key, ResultObject value);
void action_result_set_array(ActionResult &result, const std::string &key, ResultArray value);

void result_object_set_string(ResultObject &object, const std::string &key, const std::string &value);
void result_object_set_int(ResultObject &object, const std::string &key, int value);
void result_object_set_bool(ResultObject &object, const std::string &key, bool value);
void result_object_set_null(ResultObject &object, const std::string &key);
void result_object_set_object(ResultObject &object, const std::string &key, ResultObject value);
void result_object_set_array(ResultObject &object, const std::string &key, ResultArray value);

ActionRequest with_timeout_defaults(ActionRequest request, int timeout_ms);
