#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct Evidence {
    std::string id;
    std::string kind;
    std::string title;
    std::string command;
    std::filesystem::path raw_file;
    std::filesystem::path summary_file;
    std::filesystem::path view_file;
    std::string raw_sha256;
    std::string captured_at;
    std::vector<unsigned long long> included_records;
    std::vector<unsigned long long> related_records;
    std::vector<unsigned long long> concurrent_records;
    size_t raw_bytes = 0;
    size_t kept_bytes = 0;
    bool truncated = false;
    bool lossy_summary = false;
    std::string summary;
};

class EvidenceStore {
public:
    EvidenceStore(std::filesystem::path assets, std::filesystem::path working_directory);

    Evidence add(std::string_view kind,
                 std::string_view title,
                 std::string_view command,
                 const std::vector<std::string> &raw_lines,
                 bool backtrace_summary = false,
                 const std::vector<unsigned long long> &included_records = {});

    Evidence add_text(std::string_view kind,
                      std::string_view title,
                      std::string_view command,
                      std::string_view text);

    const std::vector<Evidence> &all() const;
    void write_index() const;

private:
    std::filesystem::path assets_;
    std::filesystem::path working_directory_;
    int counter_ = 0;
    std::vector<Evidence> evidence_;
};
