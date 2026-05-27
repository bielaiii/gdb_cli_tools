#include "evidence_store.hpp"

#include "../common/string_utils.hpp"
#include "../gdb/mi_utils.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <ctime>
#include <utility>

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxSummaryBytes = 65536;

uint32_t rotr(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32U - count));
}

std::string sha256_hex(std::string_view input) {
    static constexpr std::array<uint32_t, 64> k{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

    std::array<uint32_t, 8> h{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

    std::string data(input);
    const uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8ULL;
    data.push_back(static_cast<char>(0x80));
    while ((data.size() % 64) != 56) {
        data.push_back('\0');
    }
    for (int i = 7; i >= 0; --i) {
        data.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xffU));
    }

    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        std::array<uint32_t, 64> w{};
        for (size_t i = 0; i < 16; ++i) {
            size_t j = chunk + i * 4;
            w[i] = (static_cast<uint32_t>(static_cast<unsigned char>(data[j])) << 24U) |
                   (static_cast<uint32_t>(static_cast<unsigned char>(data[j + 1])) << 16U) |
                   (static_cast<uint32_t>(static_cast<unsigned char>(data[j + 2])) << 8U) |
                   static_cast<uint32_t>(static_cast<unsigned char>(data[j + 3]));
        }
        for (size_t i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h[0];
        uint32_t b = h[1];
        uint32_t c = h[2];
        uint32_t d = h[3];
        uint32_t e = h[4];
        uint32_t f = h[5];
        uint32_t g = h[6];
        uint32_t hh = h[7];

        for (size_t i = 0; i < 64; ++i) {
            uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
            uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = s0 + maj;

            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint32_t word : h) {
        out << std::setw(8) << word;
    }
    return out.str();
}

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
                    out += "\\u";
                    std::ostringstream hex;
                    hex << std::hex << std::setw(4) << std::setfill('0')
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

std::string number_array_json(const std::vector<unsigned long long> &items) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << items[i];
    }
    out << "]";
    return out.str();
}

std::string captured_at_now() {
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

std::string truncate_summary(std::string summary, bool &truncated) {
    if (summary.size() <= kMaxSummaryBytes) {
        truncated = false;
        return summary;
    }
    summary.resize(kMaxSummaryBytes);
    summary += "\n... truncated by evidence summary byte limit ...\n";
    truncated = true;
    return summary;
}

std::string evidence_json(const Evidence &ev) {
    std::ostringstream out;
    out << "{";
    out << "\"id\":" << json_escape(ev.id) << ",";
    out << "\"kind\":" << json_escape(ev.kind) << ",";
    out << "\"title\":" << json_escape(ev.title) << ",";
    out << "\"command\":" << json_escape(ev.command) << ",";
    out << "\"raw_file\":" << json_escape(ev.raw_file.lexically_normal().string()) << ",";
    out << "\"summary_file\":" << json_escape(ev.summary_file.lexically_normal().string()) << ",";
    out << "\"view_file\":" << json_escape(ev.view_file.lexically_normal().string()) << ",";
    out << "\"raw_sha256\":" << json_escape(ev.raw_sha256) << ",";
    out << "\"captured_at\":" << json_escape(ev.captured_at) << ",";
    out << "\"included_records\":" << number_array_json(ev.included_records) << ",";
    out << "\"related_records\":" << number_array_json(ev.related_records) << ",";
    out << "\"concurrent_records\":" << number_array_json(ev.concurrent_records) << ",";
    out << "\"raw_bytes\":" << ev.raw_bytes << ",";
    out << "\"kept_bytes\":" << ev.kept_bytes << ",";
    out << "\"truncated\":" << (ev.truncated ? "true" : "false") << ",";
    out << "\"lossy_summary\":" << (ev.lossy_summary ? "true" : "false");
    out << "}";
    return out.str();
}

} // namespace

static std::string summarize_backtrace(const std::string &text) {
    std::istringstream in(text);
    std::ostringstream out;
    std::string line;
    int frames = 0;
    while (std::getline(in, line)) {
        if (starts_with(trim_view(line), "#")) {
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
    md << "- Captured at: `" << ev.captured_at << "`\n";
    md << "- Raw SHA-256: `" << ev.raw_sha256 << "`\n";
    md << "- Raw bytes: `" << ev.raw_bytes << "`\n";
    md << "- Kept summary bytes: `" << ev.kept_bytes << "`\n";
    md << "- Truncated: `" << (ev.truncated ? "true" : "false") << "`\n";
    md << "- Lossy summary: `" << (ev.lossy_summary ? "true" : "false") << "`\n";
    md << "- Human summary: `" << ev.summary_file.lexically_normal().string() << "`\n";
    md << "- Raw MI: `" << ev.raw_file.lexically_normal().string() << "`\n\n";
    md << "## Record Attribution\n\n";
    md << "- Included records: `" << ev.included_records.size() << "`\n";
    md << "- Related records: `" << ev.related_records.size() << "`\n";
    md << "- Concurrent records: `" << ev.concurrent_records.size() << "`\n\n";
    md << "## Summary\n\n";
    md << "```text\n" << ev.summary << "\n```\n";
    return md.str();
}

static std::string next_evidence_id(int &counter) {
    std::ostringstream id_stream;
    id_stream << 'E';
    id_stream.width(4);
    id_stream.fill('0');
    id_stream << ++counter;
    return id_stream.str();
}

EvidenceStore::EvidenceStore(fs::path assets, fs::path working_directory)
    : assets_(std::move(assets)), working_directory_(std::move(working_directory)) {
    fs::create_directories(assets_ / "evidence" / "raw");
    fs::create_directories(assets_ / "evidence" / "summary");
    write_index();
}

Evidence EvidenceStore::add(std::string_view kind,
                            std::string_view title,
                            std::string_view command,
                            const std::vector<std::string> &raw_lines,
                            bool backtrace_summary,
                            const std::vector<unsigned long long> &included_records) {
    std::string id = next_evidence_id(counter_);

    std::string base = id + "." + slugify(title);
    fs::path view_file = assets_ / "evidence" / (base + ".md");
    fs::path raw_file = assets_ / "evidence" / "raw" / (base + ".mi.txt");
    fs::path summary_file = assets_ / "evidence" / "summary" / (base + ".summary.txt");

    std::string raw = joined_raw(raw_lines);
    std::string decoded = sanitize_output(decoded_streams(raw_lines), working_directory_);
    bool truncated = false;
    std::string summary = truncate_summary(backtrace_summary ? summarize_backtrace(decoded) : std::move(decoded), truncated);

    Evidence ev{std::move(id),
                std::string(kind),
                std::string(title),
                std::string(command),
                raw_file,
                summary_file,
                view_file,
                sha256_hex(raw),
                captured_at_now(),
                included_records,
                {},
                {},
                raw.size(),
                summary.size(),
                truncated,
                backtrace_summary || summary != raw,
                std::move(summary)};
    write_text_file(raw_file, raw);
    write_text_file(summary_file, ev.summary);
    write_text_file(view_file, evidence_view(ev));
    evidence_.push_back(std::move(ev));
    write_index();
    return evidence_.back();
}

Evidence EvidenceStore::add_text(std::string_view kind,
                                 std::string_view title,
                                 std::string_view command,
                                 std::string_view text) {
    std::string id = next_evidence_id(counter_);
    std::string base = id + "." + slugify(title);
    fs::path view_file = assets_ / "evidence" / (base + ".md");
    fs::path raw_file = assets_ / "evidence" / "raw" / (base + ".txt");
    fs::path summary_file = assets_ / "evidence" / "summary" / (base + ".summary.txt");

    std::string raw(text);
    std::string summary = sanitize_output(raw, working_directory_);
    if (summary.empty()) {
        summary = "(empty)\n";
    }
    bool truncated = false;
    summary = truncate_summary(std::move(summary), truncated);

    Evidence ev{std::move(id),
                std::string(kind),
                std::string(title),
                std::string(command),
                raw_file,
                summary_file,
                view_file,
                sha256_hex(raw),
                captured_at_now(),
                {},
                {},
                {},
                raw.size(),
                summary.size(),
                truncated,
                summary != raw,
                std::move(summary)};
    write_text_file(raw_file, raw);
    write_text_file(summary_file, ev.summary);
    write_text_file(view_file, evidence_view(ev));
    evidence_.push_back(std::move(ev));
    write_index();
    return evidence_.back();
}

const std::vector<Evidence> &EvidenceStore::all() const {
    return evidence_;
}

void EvidenceStore::write_index() const {
    fs::create_directories(assets_ / "evidence");
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"gdb-agent-evidence-index-v1\",\n";
    out << "  \"summary_byte_limit\": " << kMaxSummaryBytes << ",\n";
    out << "  \"evidence\": [\n";
    for (size_t i = 0; i < evidence_.size(); ++i) {
        out << "    " << evidence_json(evidence_[i]);
        if (i + 1 != evidence_.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    write_text_file(assets_ / "evidence" / "index.json", out.str());
}
