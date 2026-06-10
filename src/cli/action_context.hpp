#pragma once

#include "../workflow/probe_runtime.hpp"

class GdbSession;
struct DebugTask;
struct SessionOutcome;

struct ActionContext {
    GdbSession &session;
    const DebugTask *task = nullptr;
    SessionOutcome *outcome = nullptr;
    ProbeState &probe_state;
};
