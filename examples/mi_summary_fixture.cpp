#include <atomic>
#include <csignal>
#include <chrono>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mi_summary_fixture {

std::atomic<bool> g_worker_ready{false};
std::atomic<bool> g_worker_stop{false};
volatile int g_worker_sink = 0;

struct SummaryPayload {
    std::vector<std::string> names;
    std::map<std::string, int> counters;
    std::string_view label;
};

__attribute__((noinline)) int worker_spin_site(int seed) {
    int value = seed;
    while (!g_worker_stop.load(std::memory_order_acquire)) {
        value = (value * 33 + 17) & 0x7fff;
        g_worker_sink = value;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return value;
}

void worker_thread() {
    g_worker_ready.store(true, std::memory_order_release);
    worker_spin_site(7);
}

__attribute__((noinline)) int mi_summary_observe_here(SummaryPayload &payload, int guard) {
    int local_guard = guard + static_cast<int>(payload.names.size());
    std::cout << "observe label=" << payload.label << " guard=" << local_guard << "\n";
    return local_guard + payload.counters["alpha"];
}

__attribute__((noinline)) int mi_summary_leaf(SummaryPayload &payload, int guard) {
    return mi_summary_observe_here(payload, guard + 1);
}

__attribute__((noinline)) int mi_summary_middle(SummaryPayload &payload, int guard) {
    return mi_summary_leaf(payload, guard + 1);
}

__attribute__((noinline)) int mi_summary_entry(SummaryPayload &payload) {
    return mi_summary_middle(payload, 40);
}

} // namespace mi_summary_fixture

int main() {
    using namespace mi_summary_fixture;

    std::thread worker(worker_thread);
    while (!g_worker_ready.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::raise(SIGTRAP);

    SummaryPayload payload{{"alpha", "beta", "gamma"}, {{"alpha", 3}, {"beta", 5}}, "fixture"};
    int result = mi_summary_entry(payload);

    g_worker_stop.store(true, std::memory_order_release);
    worker.join();
    std::cout << "result=" << result << "\n";
    return result > 0 ? 0 : 1;
}
