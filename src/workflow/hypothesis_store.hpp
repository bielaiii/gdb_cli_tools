#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

class GdbSession;

struct HypothesisCheck {
    std::string id;
    std::string description;
    std::string expression;
    std::string assertion;
    std::string expected;
    std::string observed;
    std::string status;
    std::string evidence_id;
    std::string error_evidence_id;
};

struct HypothesisRecord {
    std::string id;
    std::string title;
    std::string description;
    std::string tool_status = "EvidenceCollectionStarted";
    std::string agent_conclusion;
    std::string agent_inference;
    std::vector<HypothesisCheck> checks;
};

struct HypothesisStore {
    std::map<std::string, HypothesisRecord> hypotheses_by_id;
    int hypothesis_counter = 0;
};

std::string next_hypothesis_id(HypothesisStore &store);
std::filesystem::path hypothesis_file_for(GdbSession &session, const std::string &id);
void write_hypothesis_index(GdbSession &session, const HypothesisStore &store);
void append_hypothesis_text(const std::filesystem::path &path, const std::string &text);
