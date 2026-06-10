#pragma once

#include "action.hpp"
#include "action_context.hpp"

#include "../gdb/command_result.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <string>

ActionOutput dispatch_action(ActionContext &context, const ActionRequest &request);
ActionOutput handle_action_request(ActionContext &context, const ActionRequest &request);
ActionOutput handle_action_json(ActionContext &context, const Json &action);
ActionOutput single_output(ActionResult result);
ActionResult make_action_error(GdbSession &session,
                               ActionKind kind,
                               const std::string &action_name,
                               const std::string &message,
                               const std::string &title = "Action validation failed",
                               const std::map<std::string, std::string> &details = {});

CommandResult run_inferior(GdbSession &session,
                           const DebugTask &task,
                           std::chrono::milliseconds deadline);
void update_outcome_from_stop(SessionOutcome &outcome, const CommandResult &result);
void collect_stop_followup(GdbSession &session,
                           SessionOutcome &outcome,
                           const CommandResult &result);
std::vector<ActionResult> handle_probe_stop(ActionContext &context, const CommandResult &result);
void write_probe_snapshot(GdbSession &session, const ProbeState &probe_state);
void flush_inferior_output(GdbSession &session, SessionOutcome &outcome);
ActionOutput replay_action_file(ActionContext &context,
                                const std::filesystem::path &path,
                                bool force = false,
                                const std::string &failure_policy_override = "");
