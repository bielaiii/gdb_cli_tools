#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

int g_watch_value = 0;
std::atomic<int> g_thread_gate{0};

struct MatrixNode {
    int value;
    const char *label;
};

__attribute__((noinline)) int matrix_breakpoint_site(int value) {
    MatrixNode node{value + 1, "breakpoint"};
    std::cout << "breakpoint site value=" << node.value << "\n";
    return node.value;
}

__attribute__((noinline)) int matrix_stop_policy_site(int value) {
    MatrixNode node{value + 2, "stop-policy"};
    std::cout << "stop policy site value=" << node.value << "\n";
    return node.value;
}

__attribute__((noinline)) int matrix_core_stop(MatrixNode *node) {
    return node->value;
}

__attribute__((noinline)) int matrix_crash_leaf(MatrixNode *node) {
    return node->value;
}

static int probe_mode() {
    std::cerr << "probe mode entering trap\n";
    std::raise(SIGTRAP);
    g_watch_value = 7;
    int observed = matrix_breakpoint_site(g_watch_value);
    observed += matrix_stop_policy_site(g_watch_value);
    try {
        throw std::runtime_error("capability fixture throw");
    } catch (const std::exception &ex) {
        std::cerr << "caught exception: " << ex.what() << "\n";
    }
    return observed == 17 ? 0 : 2;
}

static int io_mode() {
    const char *env = std::getenv("MATRIX_ENV");
    std::string input;
    std::getline(std::cin, input);
    std::cout << "stdin=" << input << "\n";
    std::cout << "env=" << (env == nullptr ? "<missing>" : env) << "\n";
    std::cerr << "stderr=capability-fixture\n";
    return input == "matrix input" && env != nullptr ? 0 : 3;
}

static int thread_crash_mode() {
    MatrixNode *node = nullptr;
    std::thread worker([&] {
        while (g_thread_gate.load() == 0) {
        }
        matrix_crash_leaf(node);
    });
    g_thread_gate.store(1);
    worker.join();
    return 0;
}

static int core_mode() {
    MatrixNode node{42, "core"};
    std::raise(SIGTRAP);
    return matrix_core_stop(&node);
}

int main(int argc, char **argv) {
    std::string mode = argc > 1 ? argv[1] : "probe";
    if (mode == "probe") {
        return probe_mode();
    }
    if (mode == "io") {
        return io_mode();
    }
    if (mode == "thread-crash") {
        return thread_crash_mode();
    }
    if (mode == "core") {
        return core_mode();
    }
    std::cerr << "unknown mode: " << mode << "\n";
    return 64;
}
