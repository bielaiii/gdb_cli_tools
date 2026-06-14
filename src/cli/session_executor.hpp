#pragma once

#include "action.hpp"
#include "action_context.hpp"

enum class SessionOperationOrigin {
    Direct,
    ReplayStep,
    OnHitAction,
};

struct SessionOperationOptions {
    SessionOperationOrigin origin = SessionOperationOrigin::Direct;
};

class SessionOperationExecutor {
public:
    explicit SessionOperationExecutor(ActionContext &context);

    ActionOutput execute(const ActionRequest &request,
                         SessionOperationOptions options = {});

private:
    ActionContext &context_;
};

ActionOutput execute_session_operation(ActionContext &context,
                                       const ActionRequest &request,
                                       SessionOperationOptions options = {});
