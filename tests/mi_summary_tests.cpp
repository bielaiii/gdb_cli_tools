#include "../src/common/string_utils.hpp"
#include "../src/evidence/evidence_store.hpp"
#include "../src/gdb/mi_utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void require(bool ok, const std::string &message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

static bool contains(const std::string &text, const std::string &needle) {
    return text.find(needle) != std::string::npos;
}

int main() {
    auto parsed = parse_mi_value(R"({name="value",frame={level="0",func="main"},args=[arg={name="s",value="hello\nworld"}]})");
    require(parsed.has_value(), "tuple MI value should parse");
    require(parsed->kind == MiValue::Kind::Tuple, "top-level MI value should be tuple");
    require(parsed->fields.size() == 3, "tuple should expose fields");

    auto list = parse_mi_value(R"(["a","b",{name="c"}])");
    require(list.has_value(), "list MI value should parse");
    require(list->kind == MiValue::Kind::List, "top-level MI value should be list");
    require(list->items.size() == 3, "list should expose items");

    std::string noisy = "std::vector<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char> > >";
    std::string clean = sanitize_output(noisy, "/tmp/project");
    require(contains(clean, "std::string"), "sanitizer should simplify std::string spelling");
    require(!contains(clean, "char_traits"), "sanitizer should remove string trait noise");
    require(!contains(clean, "> >"), "sanitizer should normalize adjacent template closers");

    std::vector<std::string> raw_records{
        R"(9^done,bkpt={number="1",type="breakpoint"})",
        R"(~"#0  read_session_value(session=0x0) at /tmp/project/examples/segfault.cpp:9\n")",
        R"(*stopped,reason="breakpoint-hit",bkptno="1",frame={func="read_session_value"})"};
    std::vector<unsigned long long> seq{10, 11, 12};
    auto audit = audit_mi_records(raw_records, seq);
    require(audit.size() == 3, "audit should include all raw records");
    require(audit[0].record_kind == "result", "result record should be classified");
    require(audit[0].token == "9", "result token should be captured");
    require(audit[1].record_kind == "stream" && audit[1].stream_type == "console", "console stream should be classified");
    require(audit[2].record_kind == "async" && audit[2].record_class == "stopped", "async class should be captured");

    fs::path assets = fs::temp_directory_path() / "gdb-agent-mi-summary-tests";
    fs::remove_all(assets);
    EvidenceStore store(assets, "/tmp/project");

    auto bt = store.add("GdbCommand",
                        "Backtrace",
                        "bt",
                        {R"(~"#0  read_session_value(session=0x0) at /tmp/project/examples/segfault.cpp:9\n")",
                         R"(~"#1  handle_request(session=0x0) at /tmp/project/examples/segfault.cpp:13\n")"},
                        true,
                        {1, 2});
    require(contains(bt.summary, "#0 read_session_value at examples/segfault.cpp:9"), "backtrace summary should simplify frame 0");
    require(contains(bt.summary, "#1 handle_request at examples/segfault.cpp:13"), "backtrace summary should simplify frame 1");
    require(bt.raw_records.size() == 2, "backtrace evidence should retain raw record audit");

    auto threads = store.add("GdbCommand",
                             "Threads",
                             "info threads",
                             {R"(~"  Id   Target Id         Frame\n")",
                              R"(~"* 1    Thread 0x1       read_session_value () at /tmp/project/examples/segfault.cpp:9\n")",
                              R"(~"  2    Thread 0x2       worker () at /tmp/project/src/worker.cpp:42\n")"},
                             false,
                             {3, 4, 5});
    require(contains(threads.summary, "* 1    Thread 0x1"), "thread summary should mark current thread");
    require(contains(threads.summary, "- 2    Thread 0x2"), "thread summary should include other threads");

    fs::remove_all(assets);
    std::cout << "mi_summary_tests ok\n";
    return 0;
}
