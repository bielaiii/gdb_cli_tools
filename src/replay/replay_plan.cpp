#include "replay_plan.hpp"

#include "../common/string_utils.hpp"

#include <chrono>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
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

std::string summarize_problem(std::string problem) {
    problem = trim(problem);
    std::string compact;
    bool previous_space = false;
    for (char c : problem) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!previous_space) {
                compact.push_back(' ');
            }
            previous_space = true;
            continue;
        }
        compact.push_back(c);
        previous_space = false;
    }
    if (compact.size() > 160) {
        compact.resize(160);
    }
    return compact;
}

std::string core_dump_string(const DebugTask &task) {
    return task.core_dump ? task.core_dump->string() : "";
}

std::string fingerprint_hex(std::string_view text) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::string task_fingerprint_input(const DebugTask &task) {
    std::ostringstream out;
    out << "problem_summary=" << summarize_problem(task.problem) << "\n";
    out << "executable=" << task.executable.string() << "\n";
    out << "working_directory=" << task.working_directory.string() << "\n";
    out << "args=" << task.args_raw << "\n";
    out << "argv=";
    for (const auto &arg : task.args) {
        out << arg << '\0';
    }
    out << "\n";
    out << "core_dump=" << core_dump_string(task) << "\n";
    return out.str();
}

} // namespace

bool replay_failure_policy_valid(const std::string &policy) {
    return policy == kReplayPolicyContinue || policy == kReplayPolicyStop;
}

std::string normalize_replay_failure_policy(const std::string &policy,
                                            const std::string &fallback) {
    if (replay_failure_policy_valid(policy)) {
        return policy;
    }
    if (replay_failure_policy_valid(fallback)) {
        return fallback;
    }
    return kReplayPolicyContinue;
}

std::string replay_task_fingerprint(const DebugTask &task) {
    return "fnv1a64:" + fingerprint_hex(task_fingerprint_input(task));
}

std::string replay_task_metadata_json(const DebugTask &task) {
    std::ostringstream out;
    out << "{";
    out << "\"problem_summary\":" << json_escape(summarize_problem(task.problem)) << ",";
    out << "\"executable\":" << json_escape(task.executable.string()) << ",";
    out << "\"working_directory\":" << json_escape(task.working_directory.string()) << ",";
    out << "\"args\":" << json_escape(task.args_raw) << ",";
    out << "\"argv\":" << json_string_array(task.args) << ",";
    out << "\"core_dump\":"
        << (task.core_dump ? json_escape(task.core_dump->string()) : std::string("null")) << ",";
    out << "\"fingerprint\":" << json_escape(replay_task_fingerprint(task));
    out << "}";
    return out.str();
}

std::string replay_action_display_name(const Json &action, int index) {
    std::string name = action.string_or("name");
    if (!name.empty()) {
        return name;
    }
    name = action.string_or("action");
    if (!name.empty()) {
        return name;
    }
    std::ostringstream fallback;
    fallback << "step-" << index;
    return fallback.str();
}

void write_replay_plan(const fs::path &path,
                       const std::string &name,
                       const std::vector<std::string> &actions,
                       const DebugTask *task,
                       const std::string &source_session_id,
                       const std::string &failure_policy,
                       const std::vector<std::string> &tags) {
    std::string plan_policy = normalize_replay_failure_policy(failure_policy);
    std::ostringstream plan;
    plan << "{\n";
    plan << "  \"schema\": " << json_escape(kReplayPlanSchema) << ",\n";
    plan << "  \"schema_version\": " << kReplayPlanSchemaVersion << ",\n";
    plan << "  \"id\": " << json_escape("replay-" + slugify(name)) << ",\n";
    plan << "  \"name\": " << json_escape(name) << ",\n";
    plan << "  \"tags\": " << json_string_array(tags) << ",\n";
    plan << "  \"source_session_id\": " << json_escape(source_session_id) << ",\n";
    plan << "  \"created_at\": " << json_escape(utc_now()) << ",\n";
    plan << "  \"failure_policy\": " << json_escape(plan_policy) << ",\n";
    plan << "  \"task\": " << (task == nullptr ? std::string("null") : replay_task_metadata_json(*task)) << ",\n";
    plan << "  \"actions\": [\n";
    for (size_t i = 0; i < actions.size(); ++i) {
        Json action = parse_json(actions[i]);
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
        if (i + 1 != actions.size()) {
            plan << ",";
        }
        plan << "\n";
    }
    plan << "  ]\n";
    plan << "}\n";
    write_text_file(path, plan.str());
}

ReplayPlanValidation validate_replay_plan(const Json &plan,
                                          const DebugTask *task,
                                          bool force) {
    ReplayPlanValidation result;
    result.schema = plan.string_or("schema");
    if (!result.schema.empty() && result.schema != kReplayPlanSchema) {
        result.ok = false;
        result.error = "unsupported replay plan schema: " + result.schema;
        return result;
    }
    if (result.schema.empty()) {
        result.warning = "legacy replay plan has no schema; defaulting to gdb-agent-replay-plan-v1 compatibility";
    }

    const Json *version = plan.find("schema_version");
    if (version != nullptr && version->is_number()) {
        result.schema_version = static_cast<int>(version->number_value);
    }
    if (result.schema_version != kReplayPlanSchemaVersion) {
        result.ok = false;
        result.error = "unsupported replay plan schema_version: " + std::to_string(result.schema_version);
        return result;
    }

    const Json *task_json = plan.find("task");
    if (task_json == nullptr || task_json->is_null()) {
        if (!result.warning.empty()) {
            result.warning += "; ";
        }
        result.warning += "replay plan has no task metadata; task match could not be verified";
        return result;
    }
    if (!task_json->is_object()) {
        result.ok = false;
        result.error = "replay plan task metadata must be an object or null";
        return result;
    }

    result.task_metadata_available = true;
    result.plan_fingerprint = task_json->string_or("fingerprint");
    if (result.plan_fingerprint.empty()) {
        if (!result.warning.empty()) {
            result.warning += "; ";
        }
        result.warning += "replay plan task metadata has no fingerprint; task match could not be verified";
        return result;
    }
    if (task == nullptr) {
        if (!result.warning.empty()) {
            result.warning += "; ";
        }
        result.warning += "current task is unavailable; task match could not be verified";
        return result;
    }

    result.current_fingerprint = replay_task_fingerprint(*task);
    result.task_metadata_match = result.plan_fingerprint == result.current_fingerprint;
    if (!result.task_metadata_match) {
        std::ostringstream message;
        message << "replay plan task fingerprint mismatch: plan=" << result.plan_fingerprint
                << " current=" << result.current_fingerprint;
        if (!force) {
            result.ok = false;
            result.error = message.str();
        } else {
            if (!result.warning.empty()) {
                result.warning += "; ";
            }
            result.warning += "force replay accepted despite " + message.str();
        }
    }
    return result;
}
