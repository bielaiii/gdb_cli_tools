#include "../src/replay/record_store.hpp"
#include "../src/replay/replay_plan.hpp"
#include "../src/common/string_utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

static void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

static Json parse_action(const std::string &text) {
    Json json = parse_json(text);
    require(json.is_object(), "test action must parse as object");
    return json;
}

static DebugTask sample_task() {
    DebugTask task;
    task.problem = "record store test";
    task.executable = "/tmp/record-fixture";
    task.working_directory = "/tmp";
    task.args_raw = "--mode test";
    task.args = {"--mode", "test"};
    return task;
}

int main() {
    try {
        RecordingState state;
        recording_start(state, "record checks", "stop_on_error", false);
        require(state.active, "recording should become active");
        require(state.name == "record checks", "recording name should be stored");
        require(state.failure_policy == kReplayPolicyStop, "failure policy should be normalized");
        require(!state.include_raw_mi, "raw MI should not be included by default");

        ActionRequest backtrace = parse_action_request(parse_action(R"({"action":"backtrace"})"));
        ActionRequest locals = parse_action_request(parse_action(R"({"action":"locals","timeout_ms":7000})"));
        recording_append_action(state, backtrace);
        recording_append_action(state, locals);
        require(state.actions.size() == 2, "recording should append typed actions");

        DebugTask task = sample_task();
        fs::path root = fs::temp_directory_path() / ("gdb-agent-record-store-test-" + std::to_string(::getpid()));
        fs::remove_all(root);
        fs::create_directories(root);

        RecordPersistResult persisted = persist_recording(state, &task, "S-record", root);
        require(persisted.step_count == 2, "persisted step count should match");
        require(fs::exists(persisted.binary_path), "binary artifact should exist");
        require(fs::exists(persisted.export_path), "JSON export should exist");
        require(is_record_artifact(persisted.binary_path), "binary artifact magic should be recognized");

        RecordedActionGroup roundtrip = read_record_artifact(persisted.binary_path);
        require(roundtrip.name == "record checks", "roundtrip name should match");
        require(roundtrip.source_session_id == "S-record", "roundtrip source session should match");
        require(roundtrip.failure_policy == kReplayPolicyStop, "roundtrip policy should match");
        require(roundtrip.actions.size() == 2, "roundtrip actions should match");
        require(roundtrip.task_metadata_json.find(replay_task_fingerprint(task)) != std::string::npos,
                "task fingerprint should be persisted");

        std::string export_json = record_group_to_json_export(roundtrip);
        require(export_json.find("\"record_artifact_authority\": \"binary\"") != std::string::npos,
                "export should mark binary authority");
        require(export_json.find("\"source_session_id\": \"S-record\"") != std::string::npos,
                "export should include source session");
        require(export_json.find("\"failure_policy\": \"stop_on_error\"") != std::string::npos,
                "export should include failure policy");
        require(export_json.find("\"action\":\"backtrace\"") != std::string::npos,
                "export should include recorded action");

        Json plan = record_group_to_replay_plan_json(roundtrip);
        require(plan.string_or("schema") == kReplayPlanSchema, "record group should convert to replay schema");
        require(plan.find("record_artifact_authority") == nullptr,
                "runtime replay plan should not include export-only authority marker");
        const Json *actions = plan.find("actions");
        require(actions != nullptr && actions->is_array() && actions->array_value.size() == 2,
                "runtime replay plan should include actions");

        recording_clear(state);
        require(!state.active, "recording clear should deactivate");
        require(state.actions.empty(), "recording clear should drop buffered actions");

        fs::remove_all(root);
    } catch (const std::exception &ex) {
        std::cerr << "record_store_tests failed: " << ex.what() << "\n";
        return 1;
    }
    std::cout << "record_store_tests ok\n";
    return 0;
}
