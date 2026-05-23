#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct Evidence {
    std::string id;
    std::string kind;
    std::string title;
    std::string command;
    std::filesystem::path raw_file;
    std::filesystem::path summary_file;
    std::filesystem::path view_file;
    std::string summary;
};

class EvidenceStore {
public:
    EvidenceStore(std::filesystem::path assets, std::filesystem::path working_directory);

    Evidence add(const std::string &kind,
                 const std::string &title,
                 const std::string &command,
                 const std::vector<std::string> &raw_lines,
                 bool backtrace_summary = false);

    const std::vector<Evidence> &all() const;

private:
    std::filesystem::path assets_;
    std::filesystem::path working_directory_;
    int counter_ = 0;
    std::vector<Evidence> evidence_;
};

