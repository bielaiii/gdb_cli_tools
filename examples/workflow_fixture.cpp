#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

#ifdef __linux__
#include <unistd.h>
#endif

int g_workflow_value = 0;

struct WorkflowNode {
    int value;
    const char *label;
};

WorkflowNode *g_workflow_node = nullptr;
WorkflowNode g_breakpoint_node{0, "breakpoint"};
WorkflowNode g_stop_policy_node{0, "stop-policy"};
WorkflowNode g_auto_continue_node{0, "auto-continue"};

__attribute__((noinline)) int workflow_breakpoint_site(int value) {
    g_breakpoint_node = WorkflowNode{value + 1, "breakpoint"};
    g_workflow_node = &g_breakpoint_node;
    return g_breakpoint_node.value;
}

__attribute__((noinline)) int workflow_stop_policy_site(int value) {
    g_stop_policy_node = WorkflowNode{value + 2, "stop-policy"};
    g_workflow_node = &g_stop_policy_node;
    return g_stop_policy_node.value;
}

__attribute__((noinline)) int workflow_auto_continue_site(int value) {
    g_auto_continue_node = WorkflowNode{value + 3, "auto-continue"};
    g_workflow_node = &g_auto_continue_node;
    return g_auto_continue_node.value;
}

__attribute__((noinline)) int workflow_core_capture(WorkflowNode *node) {
    g_workflow_node = node;
    return node->value;
}

static int live_mode() {
    std::raise(SIGTRAP);

    g_workflow_value = 11;
    int total = workflow_breakpoint_site(g_workflow_value);
    total += workflow_stop_policy_site(g_workflow_value);
    total += workflow_auto_continue_site(g_workflow_value);

    try {
        throw std::runtime_error("workflow replay setup throw");
    } catch (const std::exception &) {
    }

#ifdef __linux__
    const char message[] = "workflow syscall write\n";
    (void)write(STDOUT_FILENO, message, sizeof(message) - 1);
#endif

    return total == 39 ? 0 : 2;
}

static int core_mode() {
    WorkflowNode node{42, "core"};
    g_workflow_value = 42;
    return workflow_core_capture(&node);
}

int main(int argc, char **argv) {
    std::string mode = argc > 1 ? argv[1] : "live";
    if (mode == "live") {
        return live_mode();
    }
    if (mode == "core") {
        return core_mode();
    }
    return 64;
}
