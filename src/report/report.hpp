#pragma once

#include "../evidence/evidence_store.hpp"
#include "../task/debug_task.hpp"
#include "../workflow/crash_workflow.hpp"

#include <filesystem>
#include <vector>

void write_report(const std::filesystem::path &report,
                  const std::filesystem::path &assets,
                  const DebugTask &task,
                  const SessionOutcome &outcome,
                  const std::vector<Evidence> &evidence);

