#pragma once

#include "../replay/record_store.hpp"
#include "../workflow/probe_runtime.hpp"

class GdbSession;
struct DebugTask;
struct SessionOutcome;

struct ActionContext {
    GdbSession &session;
    const DebugTask *task = nullptr;
    SessionOutcome *outcome = nullptr;
    ProbeState &probe_state;
    RecordingState *recording = nullptr;
};
