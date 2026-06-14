#include "record_store.hpp"

#include "replay_plan.hpp"
#include "../common/string_utils.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

std::string json_escape(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::ostringstream hex;
                    hex << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c));
                    out += hex.str();
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

std::string json_string_array(const std::vector<std::string> &items) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << json_escape(items[i]);
    }
    out << "]";
    return out.str();
}

std::string utc_now() {
    auto now = std::chrono::system_clock::now();
    std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

void write_u32(std::ostream &out, uint32_t value) {
    std::array<char, 4> bytes{
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff),
    };
    out.write(bytes.data(), bytes.size());
}

uint32_t read_u32(std::istream &in) {
    std::array<unsigned char, 4> bytes{};
    in.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    if (!in) {
        throw std::runtime_error("truncated record artifact");
    }
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

void write_string(std::ostream &out, const std::string &value) {
    if (value.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("record artifact string is too large");
    }
    write_u32(out, static_cast<uint32_t>(value.size()));
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

std::string read_string(std::istream &in) {
    uint32_t size = read_u32(in);
    std::string value(size, '\0');
    in.read(value.data(), static_cast<std::streamsize>(size));
    if (!in) {
        throw std::runtime_error("truncated record artifact string");
    }
    return value;
}

std::string task_metadata(const DebugTask *task) {
    return task == nullptr ? std::string("null") : replay_task_metadata_json(*task);
}

} // namespace

void recording_start(RecordingState &state,
                     const std::string &name,
                     const std::string &failure_policy,
                     bool include_raw_mi) {
    state.active = true;
    state.name = name;
    state.failure_policy = normalize_replay_failure_policy(failure_policy);
    state.include_raw_mi = include_raw_mi;
    state.actions.clear();
    state.binary_path.clear();
    state.export_path.clear();
}

void recording_append_action(RecordingState &state, const ActionRequest &request) {
    if (!state.active) {
        return;
    }
    state.actions.push_back(request);
}

void recording_clear(RecordingState &state) {
    state.active = false;
    state.name.clear();
    state.failure_policy.clear();
    state.include_raw_mi = false;
    state.actions.clear();
    state.binary_path.clear();
    state.export_path.clear();
}

RecordedActionGroup make_recorded_action_group(const RecordingState &state,
                                               const DebugTask *task,
                                               const std::string &source_session_id) {
    RecordedActionGroup group;
    group.name = state.name;
    group.source_session_id = source_session_id;
    group.created_at = utc_now();
    group.failure_policy = normalize_replay_failure_policy(state.failure_policy);
    group.task_metadata_json = task_metadata(task);
    group.actions = state.actions;
    return group;
}

RecordPersistResult persist_recording(const RecordingState &state,
                                      const DebugTask *task,
                                      const std::string &source_session_id,
                                      const fs::path &assets_dir) {
    if (!state.active) {
        throw std::runtime_error("no active recording");
    }
    fs::path replay_dir = assets_dir / "replay";
    fs::create_directories(replay_dir);
    std::string base = slugify(state.name);
    fs::path binary_path = replay_dir / (base + ".gar");
    fs::path export_path = replay_dir / (base + ".json");
    RecordedActionGroup group = make_recorded_action_group(state, task, source_session_id);
    write_record_artifact(binary_path, group);
    write_text_file(export_path, record_group_to_json_export(group));

    RecordPersistResult result;
    result.binary_path = binary_path;
    result.export_path = export_path;
    result.step_count = static_cast<int>(group.actions.size());
    return result;
}

bool is_record_artifact(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string magic(std::char_traits<char>::length(kRecordArtifactMagic), '\0');
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    return in && magic == kRecordArtifactMagic;
}

void write_record_artifact(const fs::path &path, const RecordedActionGroup &group) {
    fs::create_directories(path.parent_path());
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("failed to open record artifact for write: " + temp.string());
        }
        out.write(kRecordArtifactMagic, static_cast<std::streamsize>(std::char_traits<char>::length(kRecordArtifactMagic)));
        write_u32(out, kRecordArtifactVersion);
        write_string(out, group.name);
        write_string(out, group.source_session_id);
        write_string(out, group.created_at);
        write_string(out, normalize_replay_failure_policy(group.failure_policy));
        write_string(out, group.task_metadata_json.empty() ? std::string("null") : group.task_metadata_json);
        if (group.actions.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("record action count is too large");
        }
        write_u32(out, static_cast<uint32_t>(group.actions.size()));
        for (const auto &action : group.actions) {
            write_string(out, dump_json(action_request_to_json(action)));
        }
        if (!out) {
            throw std::runtime_error("failed to write record artifact: " + temp.string());
        }
    }
    fs::rename(temp, path);
}

RecordedActionGroup read_record_artifact(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open record artifact: " + path.string());
    }
    std::string magic(std::char_traits<char>::length(kRecordArtifactMagic), '\0');
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!in || magic != kRecordArtifactMagic) {
        throw std::runtime_error("invalid record artifact magic: " + path.string());
    }
    uint32_t version = read_u32(in);
    if (version != kRecordArtifactVersion) {
        throw std::runtime_error("unsupported record artifact version: " + std::to_string(version));
    }

    RecordedActionGroup group;
    group.name = read_string(in);
    group.source_session_id = read_string(in);
    group.created_at = read_string(in);
    group.failure_policy = normalize_replay_failure_policy(read_string(in));
    group.task_metadata_json = read_string(in);
    uint32_t count = read_u32(in);
    group.actions.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Json action = parse_json(read_string(in));
        if (!action.is_object()) {
            throw std::runtime_error("record artifact action payload must be an object: " + path.string());
        }
        group.actions.push_back(parse_action_request(action));
    }
    return group;
}

std::string record_group_to_json_export(const RecordedActionGroup &group) {
    std::ostringstream plan;
    std::string policy = normalize_replay_failure_policy(group.failure_policy);
    plan << "{\n";
    plan << "  \"schema\": " << json_escape(kReplayPlanSchema) << ",\n";
    plan << "  \"schema_version\": " << kReplayPlanSchemaVersion << ",\n";
    plan << "  \"record_artifact_schema\": \"gdb-agent-record-artifact-v1\",\n";
    plan << "  \"record_artifact_authority\": \"binary\",\n";
    plan << "  \"id\": " << json_escape("record-" + slugify(group.name)) << ",\n";
    plan << "  \"name\": " << json_escape(group.name) << ",\n";
    plan << "  \"tags\": " << json_string_array(std::vector<std::string>{"recorded"}) << ",\n";
    plan << "  \"source_session_id\": " << json_escape(group.source_session_id) << ",\n";
    plan << "  \"created_at\": " << json_escape(group.created_at) << ",\n";
    plan << "  \"failure_policy\": " << json_escape(policy) << ",\n";
    plan << "  \"task\": " << (group.task_metadata_json.empty() ? std::string("null") : group.task_metadata_json) << ",\n";
    plan << "  \"actions\": [\n";
    for (size_t i = 0; i < group.actions.size(); ++i) {
        Json action = action_request_to_json(group.actions[i]);
        int index = static_cast<int>(i + 1);
        std::ostringstream id;
        id << 'a' << index;
        plan << "    {\n";
        plan << "      \"id\": " << json_escape(id.str()) << ",\n";
        plan << "      \"name\": " << json_escape(replay_action_display_name(action, index)) << ",\n";
        plan << "      \"enabled\": true,\n";
        plan << "      \"tags\": [],\n";
        plan << "      \"failure_policy\": null,\n";
        plan << "      \"action\": " << dump_json(action) << "\n";
        plan << "    }";
        if (i + 1 != group.actions.size()) {
            plan << ",";
        }
        plan << "\n";
    }
    plan << "  ]\n";
    plan << "}\n";
    return plan.str();
}

Json record_group_to_replay_plan_json(const RecordedActionGroup &group) {
    Json plan = parse_json(record_group_to_json_export(group));
    plan.object_value.erase("record_artifact_authority");
    plan.object_value.erase("record_artifact_schema");
    return plan;
}
