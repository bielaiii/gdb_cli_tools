#pragma once

#include "action.hpp"

#include <map>
#include <string>
#include <vector>

class GdbSession;
struct DebugTask;
struct SessionOutcome;

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

    struct HypothesisCheck {
        std::string id;
        std::string description;
        std::string expression;
        std::string assertion;
        std::string expected;
        std::string observed;
        std::string status;
        std::string evidence_id;
        std::string error_evidence_id;
    };

    struct HypothesisRecord {
        std::string id;
        std::string title;
        std::string description;
        std::string tool_status = "EvidenceCollectionStarted";
        std::string agent_conclusion;
        std::string agent_inference;
        std::vector<HypothesisCheck> checks;
    };

    std::map<std::string, ProbeInfo> probes_by_number;
    std::map<std::string, HypothesisRecord> hypotheses_by_id;
    int hypothesis_counter = 0;
};

struct ActionContext {
    GdbSession &session;
    const DebugTask *task = nullptr;
    SessionOutcome *outcome = nullptr;
    ProbeState &probe_state;
};
