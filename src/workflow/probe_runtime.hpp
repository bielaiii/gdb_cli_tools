#pragma once

#include "../cli/action.hpp"
#include "hypothesis_store.hpp"

#include <map>
#include <string>
#include <vector>

class GdbSession;
struct ActionContext;
struct CommandResult;

struct ProbeState {
    using OnHitPolicy = OnHitPolicyRequest;

    struct OnHitActionResult {
        int index = 0;
        std::string action_name;
        std::string status;
        std::string failure_policy;
        std::string evidence_id;
        std::vector<std::string> action_evidence_ids;
        std::string error_evidence_id;
        std::string error;
        std::string skip_reason;
    };

    struct ProbeHitSnapshot {
        std::string number;
        std::string kind;
        std::string stop_reason;
        std::string signal_name;
        std::string event;
        std::string selector;
        std::string location;
        std::string expression;
        std::string condition;
        std::string comment;
        std::string purpose;
        int hit_count = 0;
        OnHitPolicy on_hit_policy;
        bool known_probe = false;
    };

    struct ProbeInfo {
        std::string number;
        std::string kind;
        std::string event;
        std::string selector;
        std::string location;
        std::string expression;
        std::string condition;
        std::string comment;
        std::string purpose;
        bool enabled = true;
        bool deleted = false;
        int hit_count = 0;
        std::string last_stop_reason;
        OnHitPolicy on_hit_policy;
    };

    std::map<std::string, ProbeInfo> probes_by_number;
    HypothesisStore hypotheses;
};

std::vector<ActionResult> handle_probe_stop(ActionContext &context, const CommandResult &result);
void write_probe_snapshot(GdbSession &session, const ProbeState &probe_state);
std::string probe_array_json(const ProbeState &probe_state, bool include_deleted = false);
ResultArray probe_array_result(const ProbeState &probe_state, bool include_deleted = false);
