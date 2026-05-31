#include "../src/common/json.hpp"
#include "../src/replay/replay_plan.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

static void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

static std::string read_file(const fs::path &path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to read " + path.string());
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static DebugTask make_task(const std::string &problem, const std::string &args) {
    DebugTask task;
    task.problem = problem;
    task.executable = "/tmp/replay-test/app";
    task.working_directory = "/tmp/replay-test";
    task.args_raw = args;
    task.args = args.empty() ? std::vector<std::string>{} : std::vector<std::string>{"--mode", args};
    task.stdin_path = "/dev/null";
    return task;
}

int main() {
    try {
        DebugTask task = make_task("segfault in callback", "crash");
        fs::path plan_path = fs::temp_directory_path() /
                             ("gdb-agent-replay-plan-test-" + std::to_string(getpid()) + ".json");
        write_replay_plan(plan_path,
                          "bt checks",
                          {R"({"action":"backtrace"})", R"({"action":"args_info"})"},
                          &task,
                          "S1",
                          kReplayPolicyStop,
                          {"smoke"});

        Json plan = parse_json(read_file(plan_path));
        require(plan.string_or("schema") == kReplayPlanSchema, "schema missing");
        require(plan.int_or("schema_version") == kReplayPlanSchemaVersion, "schema version missing");
        require(plan.string_or("failure_policy") == kReplayPolicyStop, "failure policy missing");
        require(plan.string_or("source_session_id") == "S1", "source session missing");

        const Json *task_json = plan.find("task");
        require(task_json != nullptr && task_json->is_object(), "task metadata missing");
        require(!task_json->string_or("fingerprint").empty(), "fingerprint missing");

        ReplayPlanValidation same = validate_replay_plan(plan, &task, false);
        require(same.ok, "matching plan should validate");
        require(same.task_metadata_match, "matching plan should report task match");

        DebugTask changed = make_task("segfault in callback", "other");
        ReplayPlanValidation mismatch = validate_replay_plan(plan, &changed, false);
        require(!mismatch.ok, "mismatched plan should be rejected by default");
        require(!mismatch.error.empty(), "mismatch should explain error");

        ReplayPlanValidation forced = validate_replay_plan(plan, &changed, true);
        require(forced.ok, "force should allow mismatch");
        require(!forced.task_metadata_match, "force mismatch should remain visible");
        require(forced.warning.find("force replay accepted") != std::string::npos,
                "force should record mismatch warning");

        Json legacy = parse_json(R"({"actions":[{"id":"a1","action":{"action":"backtrace"}}]})");
        ReplayPlanValidation legacy_result = validate_replay_plan(legacy, &task, false);
        require(legacy_result.ok, "legacy structured plan should remain readable");
        require(!legacy_result.warning.empty(), "legacy structured plan should warn");

        Json bad_schema = parse_json(R"({"schema":"other","actions":[]})");
        ReplayPlanValidation bad_schema_result = validate_replay_plan(bad_schema, &task, false);
        require(!bad_schema_result.ok, "unsupported schema should be rejected");

        require(normalize_replay_failure_policy("invalid") == kReplayPolicyContinue,
                "invalid policy should default to continue_on_error");
        require(normalize_replay_failure_policy("invalid", kReplayPolicyStop) == kReplayPolicyStop,
                "invalid step policy should inherit fallback");

        fs::remove(plan_path);
        std::cout << "replay_plan_tests ok\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "replay_plan_tests failed: " << ex.what() << "\n";
        return 1;
    }
}
