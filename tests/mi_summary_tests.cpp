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

    auto escaped = parse_mi_value(R"({msg="quoted \"value\"",nested=[child={name="node",value="{x=1}"}]})");
    require(escaped.has_value(), "escaped MI strings and nested list tuples should parse");
    require(escaped->fields.size() == 2, "escaped tuple should expose two fields");
    require(escaped->fields[0].second.value == R"(quoted "value")", "escaped quotes should be decoded");

    std::string noisy = "std::vector<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char> > >";
    std::string clean = sanitize_output(noisy, "/tmp/project");
    require(contains(clean, "std::string"), "sanitizer should simplify std::string spelling");
    require(!contains(clean, "char_traits"), "sanitizer should remove string trait noise");
    require(!contains(clean, "> >"), "sanitizer should normalize adjacent template closers");
    require(contains(sanitize_output("std::vector<int, std::allocator<int> >", "/tmp/project"), "std::vector<int>"),
            "sanitizer should remove vector allocator noise");
    require(contains(sanitize_output("std::unique_ptr<Foo, std::default_delete<Foo> >", "/tmp/project"), "std::unique_ptr<Foo>"),
            "sanitizer should remove unique_ptr default_delete noise");
    require(contains(sanitize_output("std::map<int, std::string, std::less<int>, std::allocator<std::pair<int const, std::string> > >", "/tmp/project"),
                     "std::map<int, std::string>"),
            "sanitizer should remove map comparator and allocator noise");
    require(contains(sanitize_output("std::unordered_map<int, std::string, std::hash<int>, std::equal_to<int>, std::allocator<std::pair<int const, std::string> > >", "/tmp/project"),
                     "std::unordered_map<int, std::string>"),
            "sanitizer should remove unordered_map hash/equality/allocator noise");
    require(contains(sanitize_output("std::pair<const int, std::string>", "/tmp/project"), "std::pair<int, std::string>"),
            "sanitizer should remove pair const-key noise");
    require(contains(sanitize_output("std::pair<int const, std::string>", "/tmp/project"), "std::pair<int, std::string>"),
            "sanitizer should remove trailing const-key noise");
    require(contains(sanitize_output("/tmp/project/src/server.cpp:17", "/tmp/project"), "src/server.cpp:17"),
            "sanitizer should keep working-directory paths relative");

    std::vector<std::string> raw_records{
        R"(9^done,bkpt={number="1",type="breakpoint"})",
        R"(~"#0  read_session_value(session=0x0) at /tmp/project/examples/segfault.cpp:9\n")",
        R"(@"target stream line\n")",
        R"(&"log stream line\n")",
        R"(*stopped,reason="breakpoint-hit",bkptno="1",frame={func="read_session_value"})"};
    std::vector<unsigned long long> seq{10, 11, 12, 13, 14};
    auto audit = audit_mi_records(raw_records, seq);
    require(audit.size() == 5, "audit should include all raw records");
    require(audit[0].record_kind == "result", "result record should be classified");
    require(audit[0].token == "9", "result token should be captured");
    require(audit[1].record_kind == "stream" && audit[1].stream_type == "console", "console stream should be classified");
    require(audit[2].record_kind == "stream" && audit[2].stream_type == "target", "target stream should be classified");
    require(audit[3].record_kind == "stream" && audit[3].stream_type == "log", "log stream should be classified");
    require(audit[4].record_kind == "async" && audit[4].record_class == "stopped", "async class should be captured");
    std::string mi_summary = summarize_mi_records({R"(12^done,value="{name=\"x\",items=[1,2]}")"});
    require(contains(mi_summary, "result:done"), "MI summary should include result class");
    require(contains(mi_summary, "value="), "MI summary should include parsed payload fields");

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
