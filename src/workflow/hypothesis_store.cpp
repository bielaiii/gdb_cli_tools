#include "hypothesis_store.hpp"

#include "../common/json.hpp"
#include "../common/string_utils.hpp"
#include "../gdb/gdb_session.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

std::string json_escape(const std::string &s) {
    Json json;
    json.type = Json::Type::String;
    json.string_value = s;
    return dump_json(json);
}

} // namespace

std::string next_hypothesis_id(HypothesisStore &store) {
    ++store.hypothesis_counter;
    std::ostringstream out;
    out << 'H';
    out.width(4);
    out.fill('0');
    out << store.hypothesis_counter;
    return out.str();
}

fs::path hypothesis_file_for(GdbSession &session, const std::string &id) {
    fs::path dir = session.assets_dir() / "hypotheses";
    fs::create_directories(dir);
    return dir / (id + ".md");
}

void write_hypothesis_index(GdbSession &session, const HypothesisStore &store) {
    fs::path dir = session.assets_dir() / "hypotheses";
    fs::create_directories(dir);
    fs::path path = dir / "index.json";
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"gdb-agent-hypotheses-v1\",\n";
    out << "  \"hypotheses\": [\n";
    bool first_hypothesis = true;
    for (const auto &[_, hypothesis] : store.hypotheses_by_id) {
        if (!first_hypothesis) {
            out << ",\n";
        }
        first_hypothesis = false;
        out << "    {\n";
        out << "      \"id\": " << json_escape(hypothesis.id) << ",\n";
        out << "      \"title\": " << json_escape(hypothesis.title) << ",\n";
        out << "      \"description\": " << json_escape(hypothesis.description) << ",\n";
        out << "      \"tool_status\": " << json_escape(hypothesis.tool_status) << ",\n";
        out << "      \"agent_conclusion\": " << json_escape(hypothesis.agent_conclusion) << ",\n";
        out << "      \"agent_inference\": " << json_escape(hypothesis.agent_inference) << ",\n";
        out << "      \"checks\": [\n";
        for (size_t i = 0; i < hypothesis.checks.size(); ++i) {
            const auto &check = hypothesis.checks[i];
            out << "        {\n";
            out << "          \"id\": " << json_escape(check.id) << ",\n";
            out << "          \"check_id\": " << json_escape(check.id) << ",\n";
            out << "          \"description\": " << json_escape(check.description) << ",\n";
            out << "          \"expression\": " << json_escape(check.expression) << ",\n";
            out << "          \"assertion\": " << json_escape(check.assertion) << ",\n";
            out << "          \"expected\": " << json_escape(check.expected) << ",\n";
            out << "          \"observed\": " << json_escape(check.observed) << ",\n";
            out << "          \"status\": " << json_escape(check.status) << ",\n";
            out << "          \"evidence\": " << json_escape(check.evidence_id) << ",\n";
            out << "          \"error_evidence\": "
                << (check.error_evidence_id.empty() ? std::string("null") : json_escape(check.error_evidence_id)) << "\n";
            out << "        }";
            if (i + 1 != hypothesis.checks.size()) {
                out << ",";
            }
            out << "\n";
        }
        out << "      ]\n";
        out << "    }";
    }
    out << "\n  ]\n";
    out << "}\n";
    write_text_file(path, out.str());
}

void append_hypothesis_text(const fs::path &path, const std::string &text) {
    std::ofstream out(path, std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to append file: " + path.string());
    }
    out << text;
}

