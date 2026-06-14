#pragma once

#include "../cli/action.hpp"
#include "../common/json.hpp"
#include "../task/debug_task.hpp"

#include <filesystem>
#include <string>
#include <vector>

constexpr const char *kRecordArtifactMagic = "GDBA_REC1";
constexpr int kRecordArtifactVersion = 1;

struct RecordingState {
    bool active = false;
    std::string name;
    std::string failure_policy;
    bool include_raw_mi = false;
    std::vector<ActionRequest> actions;
    std::filesystem::path binary_path;
    std::filesystem::path export_path;
};

struct RecordedActionGroup {
    std::string name;
    std::string source_session_id;
    std::string created_at;
    std::string failure_policy;
    std::string task_metadata_json = "null";
    std::vector<ActionRequest> actions;
};

struct RecordPersistResult {
    std::filesystem::path binary_path;
    std::filesystem::path export_path;
    int step_count = 0;
};

void recording_start(RecordingState &state,
                     const std::string &name,
                     const std::string &failure_policy,
                     bool include_raw_mi);
void recording_append_action(RecordingState &state, const ActionRequest &request);
void recording_clear(RecordingState &state);

RecordedActionGroup make_recorded_action_group(const RecordingState &state,
                                               const DebugTask *task,
                                               const std::string &source_session_id);

RecordPersistResult persist_recording(const RecordingState &state,
                                      const DebugTask *task,
                                      const std::string &source_session_id,
                                      const std::filesystem::path &assets_dir);

bool is_record_artifact(const std::filesystem::path &path);
RecordedActionGroup read_record_artifact(const std::filesystem::path &path);
void write_record_artifact(const std::filesystem::path &path, const RecordedActionGroup &group);

std::string record_group_to_json_export(const RecordedActionGroup &group);
Json record_group_to_replay_plan_json(const RecordedActionGroup &group);
