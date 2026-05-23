#include "evidence_store.hpp"

#include "../common/string_utils.hpp"
#include "../gdb/mi_utils.hpp"

#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

static std::string summarize_backtrace(const std::string &text) {
    std::istringstream in(text);
    std::ostringstream out;
    std::string line;
    int frames = 0;
    while (std::getline(in, line)) {
        if (starts_with(trim(line), "#")) {
            out << line << '\n';
            ++frames;
            if (frames >= 12) {
                out << "... truncated after 12 frames\n";
                break;
            }
        }
    }
    std::string summary = out.str();
    return summary.empty() ? text : summary;
}

static std::string evidence_view(const Evidence &ev) {
    std::ostringstream md;
    md << "# " << ev.id << " " << ev.title << "\n\n";
    md << "- Kind: `" << ev.kind << "`\n";
    md << "- Command: `" << ev.command << "`\n";
    md << "- Human summary: `" << ev.summary_file.lexically_normal().string() << "`\n";
    md << "- Raw MI: `" << ev.raw_file.lexically_normal().string() << "`\n\n";
    md << "## Summary\n\n";
    md << "```text\n" << ev.summary << "\n```\n";
    return md.str();
}

EvidenceStore::EvidenceStore(fs::path assets, fs::path working_directory)
    : assets_(std::move(assets)), working_directory_(std::move(working_directory)) {
    fs::create_directories(assets_ / "evidence" / "raw");
    fs::create_directories(assets_ / "evidence" / "summary");
}

Evidence EvidenceStore::add(const std::string &kind,
                            const std::string &title,
                            const std::string &command,
                            const std::vector<std::string> &raw_lines,
                            bool backtrace_summary) {
    std::ostringstream id_stream;
    id_stream << 'E';
    id_stream.width(4);
    id_stream.fill('0');
    id_stream << ++counter_;
    std::string id = id_stream.str();

    std::string base = id + "." + slugify(title);
    fs::path view_file = assets_ / "evidence" / (base + ".md");
    fs::path raw_file = assets_ / "evidence" / "raw" / (base + ".mi.txt");
    fs::path summary_file = assets_ / "evidence" / "summary" / (base + ".summary.txt");

    std::string raw = joined_raw(raw_lines);
    std::string decoded = sanitize_output(decoded_streams(raw_lines), working_directory_);
    std::string summary = backtrace_summary ? summarize_backtrace(decoded) : decoded;

    Evidence ev{id, kind, title, command, raw_file, summary_file, view_file, summary};
    write_text_file(raw_file, raw);
    write_text_file(summary_file, summary);
    write_text_file(view_file, evidence_view(ev));
    evidence_.push_back(ev);
    return ev;
}

const std::vector<Evidence> &EvidenceStore::all() const {
    return evidence_;
}

