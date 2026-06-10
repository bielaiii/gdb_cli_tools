#pragma once

#include "../cli/action.hpp"

#include <filesystem>
#include <string>

struct ActionContext;
struct DebugTask;

void rebuild_replay_plan_from_jsonl(const std::filesystem::path &jsonl_file,
                                    const std::filesystem::path &plan_file,
                                    const std::string &name,
                                    const DebugTask *task,
                                    const std::string &source_session_id,
                                    const std::string &failure_policy);

ActionOutput replay_action_file(ActionContext &context,
                                const std::filesystem::path &path,
                                bool force = false,
                                const std::string &failure_policy_override = "");
