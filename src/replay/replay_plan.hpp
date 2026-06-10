#pragma once

#include "../cli/action.hpp"
#include "../common/json.hpp"
#include "../task/debug_task.hpp"

#include <filesystem>
#include <string>
#include <vector>

constexpr const char *kReplayPlanSchema = "gdb-agent-replay-plan-v1";
constexpr int kReplayPlanSchemaVersion = 1;
constexpr const char *kReplayPolicyContinue = "continue_on_error";
constexpr const char *kReplayPolicyStop = "stop_on_error";

struct ReplayPlanValidation {
    bool ok = true;
    bool task_metadata_available = false;
    bool task_metadata_match = true;
    std::string schema;
    int schema_version = kReplayPlanSchemaVersion;
    std::string plan_fingerprint;
    std::string current_fingerprint;
    std::string warning;
    std::string error;
};

bool replay_failure_policy_valid(const std::string &policy);
std::string normalize_replay_failure_policy(const std::string &policy,
                                            const std::string &fallback = kReplayPolicyContinue);
std::string replay_task_fingerprint(const DebugTask &task);
std::string replay_task_metadata_json(const DebugTask &task);
std::string replay_action_display_name(const Json &action, int index);

void write_replay_plan(const std::filesystem::path &path,
                       const std::string &name,
                       const std::vector<ActionRequest> &actions,
                       const DebugTask *task,
                       const std::string &source_session_id,
                       const std::string &failure_policy = kReplayPolicyContinue,
                       const std::vector<std::string> &tags = {});

ReplayPlanValidation validate_replay_plan(const Json &plan,
                                          const DebugTask *task,
                                          bool force);
