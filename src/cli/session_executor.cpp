#include "session_executor.hpp"

#include "action_dispatch.hpp"
#include "../gdb/gdb_session.hpp"

#include <mutex>

SessionOperationExecutor::SessionOperationExecutor(ActionContext &context)
    : context_(context) {
}

ActionOutput SessionOperationExecutor::execute(const ActionRequest &request,
                                               SessionOperationOptions) {
    std::lock_guard<std::recursive_mutex> lock(context_.session.operation_mutex());
    return dispatch_action(context_, request);
}

ActionOutput execute_session_operation(ActionContext &context,
                                       const ActionRequest &request,
                                       SessionOperationOptions options) {
    SessionOperationExecutor executor(context);
    return executor.execute(request, options);
}
