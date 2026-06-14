#include "session_executor.hpp"

#include "action_dispatch.hpp"
#include "../gdb/gdb_session.hpp"
#include "../replay/record_store.hpp"

#include <mutex>

SessionOperationExecutor::SessionOperationExecutor(ActionContext &context)
    : context_(context) {
}

ActionOutput SessionOperationExecutor::execute(const ActionRequest &request,
                                               SessionOperationOptions options) {
    std::lock_guard<std::recursive_mutex> lock(context_.session.operation_mutex());
    ActionOutput output = dispatch_action(context_, request);
    if (context_.recording != nullptr && options.origin == SessionOperationOrigin::Direct &&
        output.final.ok) {
        const bool excluded =
            request.kind == ActionKind::RecordStart ||
            request.kind == ActionKind::RecordStatus ||
            request.kind == ActionKind::RecordStop ||
            request.kind == ActionKind::RecordDiscard ||
            request.kind == ActionKind::Replay ||
            request.kind == ActionKind::FinishSession ||
            request.kind == ActionKind::SaveAction ||
            (request.kind == ActionKind::RawMi && !context_.recording->include_raw_mi) ||
            request.kind == ActionKind::Unknown;
        if (!excluded) {
            recording_append_action(*context_.recording, request);
        }
    }
    return output;
}

ActionOutput execute_session_operation(ActionContext &context,
                                       const ActionRequest &request,
                                       SessionOperationOptions options) {
    SessionOperationExecutor executor(context);
    return executor.execute(request, options);
}
