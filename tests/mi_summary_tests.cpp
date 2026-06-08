#include "../src/common/string_utils.hpp"
#include "../src/evidence/evidence_store.hpp"
#include "../src/gdb/mi_utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
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

static const MiValue *field(const MiValue &value, std::string_view name) {
    for (const auto &entry : value.fields) {
        if (entry.first == name) {
            return &entry.second;
        }
    }
    return nullptr;
}

static void test_mi_value_parser() {
    auto empty_tuple = parse_mi_value("{}");
    require(empty_tuple && empty_tuple->kind == MiValue::Kind::Tuple && empty_tuple->fields.empty(),
            "empty tuple should parse");

    auto tuple = parse_mi_value(R"({thread-id="1",frame={level="0",func="main"},under_score="ok"})");
    require(tuple && tuple->kind == MiValue::Kind::Tuple, "multi-field tuple should parse");
    require(field(*tuple, "thread-id") != nullptr, "tuple field names may contain hyphen");
    require(field(*tuple, "under_score") != nullptr, "tuple field names may contain underscore");
    require(field(*field(*tuple, "frame"), "func")->value == "main", "nested tuple field should parse");

    auto empty_list = parse_mi_value("[]");
    require(empty_list && empty_list->kind == MiValue::Kind::List && empty_list->items.empty(),
            "empty list should parse");

    auto value_list = parse_mi_value(R"(["a","b"])");
    require(value_list && value_list->items.size() == 2, "value list should parse");
    require(value_list->items[0].value == "a" && value_list->items[1].value == "b",
            "value list should expose string values");

    auto result_list = parse_mi_value(R"([frame={level="0",func="main"},frame={level="1",func="worker"}])");
    require(result_list && result_list->items.size() == 2, "result list should parse");
    require(field(result_list->items[1], "frame") != nullptr, "result list entries should retain field names");

    auto mixed = parse_mi_value(R"(["x",{name="y"},item={value="z"},["nested"]])");
    require(mixed && mixed->items.size() == 4, "mixed MI list should parse");
    require(mixed->items[1].kind == MiValue::Kind::Tuple, "tuple value inside list should parse");
    require(field(mixed->items[2], "item") != nullptr, "named result item inside list should parse");
    require(mixed->items[3].kind == MiValue::Kind::List, "nested list should parse");

    auto escaped = parse_mi_value(R"({msg="quoted \"value\"",path="C:\\tmp",nl="a\nb\tc\rd",empty="",text="{x=[1,2],y}"})");
    require(escaped && escaped->fields.size() == 5, "escaped MI strings should parse");
    require(field(*escaped, "msg")->value == R"(quoted "value")", "escaped quote should decode");
    require(field(*escaped, "path")->value == R"(C:\tmp)", "escaped backslash should decode");
    require(field(*escaped, "nl")->value == "a\nb\tc\rd", "escaped newline/tab/carriage return should decode");
    require(field(*escaped, "empty")->value.empty(), "empty string should parse");
    require(field(*escaped, "text")->value == "{x=[1,2],y}", "structural characters inside strings stay scalar");

    require(parse_mi_value("true")->value == "true", "bare true should parse");
    require(parse_mi_value("false")->value == "false", "bare false should parse");
    require(parse_mi_value("0x0")->value == "0x0", "bare address should parse");
    require(parse_mi_value("<optimized out>")->value == "<optimized out>", "optimized-out bare value should parse");
    require(parse_mi_value("<unavailable>")->value == "<unavailable>", "unavailable bare value should parse");
    require(parse_mi_value("bare text with spaces")->value == "bare text with spaces",
            "bare text with spaces should parse until delimiter");

    require(!parse_mi_value(""), "empty input should fail");
    require(!parse_mi_value(R"("unterminated)"), "unterminated string should fail");
    require(!parse_mi_value("{name}"), "tuple field missing equals should fail");
    require(!parse_mi_value(R"({name="x")"), "missing tuple close should fail");
    require(!parse_mi_value(R"(["x")"), "missing list close should fail");
    require(!parse_mi_value(R"({name="x"} garbage)"), "trailing garbage should fail");
}

static void test_mi_record_audit() {
    std::vector<std::string> raw_records{
        R"(9^done,bkpt={number="1",type="breakpoint"})",
        R"(^running)",
        R"(3^error,msg="bad expression")",
        R"(*stopped,reason="breakpoint-hit",bkptno="1")",
        R"(*running,thread-id="all")",
        R"(=thread-created,id="2")",
        R"(=thread-exited,id="2")",
        R"(=breakpoint-modified,bkpt={number="1"})",
        R"(~"console stream line\n")",
        R"(@"target stream line\n")",
        R"(&"log stream line\n")",
        "(gdb)",
        "abc^done",
        "plain text"};
    std::vector<unsigned long long> seq{10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
    auto audit = audit_mi_records(raw_records, seq);
    require(audit.size() == raw_records.size(), "audit should include every raw record");
    require(audit[0].record_kind == "result" && audit[0].record_class == "done" && audit[0].token == "9",
            "result record token/class should be captured");
    require(audit[1].record_kind == "result" && audit[1].record_class == "running" && audit[1].token.empty(),
            "result without token should be captured");
    require(audit[2].record_kind == "result" && audit[2].record_class == "error" && audit[2].token == "3",
            "error result should be captured");
    require(audit[3].record_kind == "async" && audit[3].record_class == "stopped", "exec stopped should classify");
    require(audit[4].record_kind == "async" && audit[4].record_class == "running", "exec running should classify");
    require(audit[5].record_kind == "async" && audit[5].record_class == "thread-created", "notify create should classify");
    require(audit[6].record_kind == "async" && audit[6].record_class == "thread-exited", "notify exit should classify");
    require(audit[7].record_kind == "async" && audit[7].record_class == "breakpoint-modified", "notify bp should classify");
    require(audit[8].record_kind == "stream" && audit[8].stream_type == "console", "console stream should classify");
    require(audit[9].record_kind == "stream" && audit[9].stream_type == "target", "target stream should classify");
    require(audit[10].record_kind == "stream" && audit[10].stream_type == "log", "log stream should classify");
    require(audit[11].record_kind == "prompt", "prompt should classify");
    require(audit[12].record_kind == "unknown" && audit[12].token.empty(),
            "malformed non-numeric token prefix should not be treated as a token");
    require(audit[13].record_kind == "unknown", "unknown line should classify");

    std::string decoded = decoded_streams({raw_records[8], raw_records[9], raw_records[10]});
    require(contains(decoded, "console stream line"), "decoded streams should keep console payload");
    require(contains(decoded, "target stream line"), "decoded streams should keep target payload");
    require(contains(decoded, "log stream line"), "decoded streams should keep log payload");
}

static void test_mi_record_summary() {
    std::string done = summarize_mi_records({R"(12^done,value="{name=\"x\",items=[1,2]}",thread-id="1")"});
    require(contains(done, "result:done"), "MI summary should include result class");
    require(contains(done, "value="), "MI summary should include value payload");
    require(contains(done, "thread-id=1"), "MI summary should include thread id");

    std::string error = summarize_mi_records({R"(^error,msg="No symbol \"missing\" in current context.")"});
    require(contains(error, "result:error"), "MI summary should include error class");
    require(contains(error, "msg=No symbol"), "MI summary should include error message");

    std::string stopped = summarize_mi_records(
        {R"(*stopped,reason="breakpoint-hit",thread-id="1",stopped-threads="all",frame={func="main",file="/tmp/project/src/main.cpp",line="7"})"});
    require(contains(stopped, "async:stopped"), "stopped async summary should include class");
    require(contains(stopped, "reason=breakpoint-hit"), "stopped summary should include reason");
    require(contains(stopped, "thread-id=1"), "stopped summary should include thread id");
    require(contains(stopped, "frame=main at /tmp/project/src/main.cpp:7"), "stopped summary should include frame location");

    std::string watch = summarize_mi_records({R"(*stopped,reason="watchpoint-trigger",wpt={number="2",exp="g_value"})"});
    require(contains(watch, "reason=watchpoint-trigger"), "watchpoint stopped summary should include reason");
    require(contains(watch, "wpt={number=2"), "watchpoint stopped summary should include wpt payload");

    std::string signal = summarize_mi_records({R"(*stopped,reason="signal-received",thread-id="3",frame={func="crash"})"});
    require(contains(signal, "reason=signal-received"), "signal stopped summary should include reason");
    require(contains(signal, "frame=crash"), "signal stopped summary should include frame func");

    std::string exited = summarize_mi_records({R"(*stopped,reason="exited-normally")",
                                               R"(*stopped,reason="exited",exit-code="0177")",
                                               R"(=thread-created,id="4")"});
    require(contains(exited, "reason=exited-normally"), "normal exit summary should include reason");
    require(contains(exited, "reason=exited"), "exit code summary should include reason");
    require(contains(exited, "async:thread-created"), "notify async summary should include class");

    std::string streams = summarize_mi_records({R"(~"console\n")", R"(@"target\n")", R"(&"log\n")"});
    require(streams.empty(), "stream records should not be misclassified as result/async summaries");
}

static void test_cpp_sanitizer() {
    std::string noisy = "std::vector<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char> > >";
    std::string clean = sanitize_output(noisy, "/tmp/project");
    require(contains(clean, "std::vector<std::string>"), "sanitizer should simplify vector<string>");
    require(!contains(clean, "char_traits"), "sanitizer should remove string trait noise");
    require(!contains(clean, "> >"), "sanitizer should normalize adjacent template closers");

    require(contains(sanitize_output("std::list<Foo, std::allocator<Foo> >", "/tmp/project"), "std::list<Foo>"),
            "sanitizer should remove list allocator noise");
    require(contains(sanitize_output("std::deque<Foo, std::allocator<Foo> >", "/tmp/project"), "std::deque<Foo>"),
            "sanitizer should remove deque allocator noise");
    require(contains(sanitize_output("std::set<Foo, std::less<Foo>, std::allocator<Foo> >", "/tmp/project"), "std::set<Foo>"),
            "sanitizer should keep set core type");
    require(contains(sanitize_output("std::unique_ptr<Foo, std::default_delete<Foo> >", "/tmp/project"), "std::unique_ptr<Foo>"),
            "sanitizer should remove unique_ptr default_delete noise");
    require(contains(sanitize_output("std::shared_ptr< Foo >", "/tmp/project"), "std::shared_ptr<Foo>"),
            "sanitizer should normalize shared_ptr spacing");
    require(contains(sanitize_output("std::weak_ptr< Foo >", "/tmp/project"), "std::weak_ptr<Foo>"),
            "sanitizer should normalize weak_ptr spacing");
    require(contains(sanitize_output("std::optional< std::string >", "/tmp/project"), "std::optional<std::string>"),
            "sanitizer should normalize optional spacing");
    require(contains(sanitize_output("std::variant< int,std::string >", "/tmp/project"), "std::variant<int, std::string>"),
            "sanitizer should normalize variant spacing");
    require(contains(sanitize_output("std::tuple< int,std::string,Foo >", "/tmp/project"), "std::tuple<int, std::string, Foo>"),
            "sanitizer should normalize tuple spacing");
    require(contains(sanitize_output("std::pair<const int, std::string>", "/tmp/project"), "std::pair<int, std::string>"),
            "sanitizer should remove pair const-key noise");
    require(contains(sanitize_output("std::pair<int const, std::string>", "/tmp/project"), "std::pair<int, std::string>"),
            "sanitizer should remove trailing const-key noise");

    std::string map = sanitize_output(
        "std::map<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::unique_ptr<Foo, std::default_delete<Foo> >, std::less<std::__cxx11::basic_string<char> >, std::allocator<std::pair<std::__cxx11::basic_string<char> const, std::unique_ptr<Foo, std::default_delete<Foo> > > > >",
        "/tmp/project");
    require(contains(map, "std::map<std::string, std::unique_ptr<Foo>>"),
            "sanitizer should remove nested map comparator/allocator noise");

    std::string unordered = sanitize_output(
        "std::unordered_map<std::string, Bar, std::hash<std::string>, std::equal_to<std::string>, std::allocator<std::pair<std::string const, Bar> > >",
        "/tmp/project");
    require(contains(unordered, "std::unordered_map<std::string, Bar>"),
            "sanitizer should remove unordered_map hash/equality/allocator noise");

    require(contains(sanitize_output("/tmp/project/src/server.cpp:17", "/tmp/project"), "src/server.cpp:17"),
            "sanitizer should keep working-directory paths relative");
    require(contains(sanitize_output("/tmp/project/build/../src/server.cpp:17", "/tmp/project"), "src/server.cpp:17"),
            "sanitizer should normalize working-directory paths before relativizing");
    require(contains(sanitize_output("/usr/include/c++/vector:1", "/tmp/project"), "/usr/include/c++/vector:1"),
            "sanitizer should not rewrite system paths outside working directory");
    require(contains(sanitize_output("std::map< int,std::string >", "/tmp/project"), "std::map<int, std::string>"),
            "sanitizer should prefer readable comma spacing");
}

static void test_evidence_store_integration() {
    fs::path assets = fs::temp_directory_path() / "gdb-agent-mi-summary-tests";
    fs::remove_all(assets);
    EvidenceStore store(assets, "/tmp/project");

    auto bt = store.add("GdbCommand",
                        "Backtrace",
                        "thread apply all bt",
                        {R"(~"Thread 2 (Thread 0x2 (LWP 102) \"worker\"):\n")",
                         R"(~"#0  worker_loop () from /lib/x86_64-linux-gnu/libpthread.so.0\n")",
                         R"(~"#1  ?? ()\n")",
                         R"(~"Thread 1 (Thread 0x1 (LWP 101) \"main\"):\n")",
                         R"(~"#0  read_session_value(session=0x0) at /tmp/project/examples/segfault.cpp:9\n")",
                         R"(~"#1  handle_request(session=0x0) at /tmp/project/build/../src/server.cpp:13\n")"},
                        true,
                        {1, 2, 3, 4, 5, 6});
    require(contains(bt.summary, "Thread 2"), "backtrace summary should keep thread boundary");
    require(contains(bt.summary, "#0 worker_loop from /lib/x86_64-linux-gnu/libpthread.so.0"),
            "backtrace summary should include shared library source");
    require(contains(bt.summary, "#1 ??"), "backtrace summary should keep unknown frame");
    require(contains(bt.summary, "#0 read_session_value at examples/segfault.cpp:9"),
            "backtrace summary should simplify frame source path");
    require(contains(bt.summary, "#1 handle_request at src/server.cpp:13"),
            "backtrace summary should normalize build-relative source path");
    require(bt.raw_records.size() == 6, "backtrace evidence should retain raw record audit");

    std::vector<std::string> many_frames;
    std::vector<unsigned long long> many_seq;
    for (int i = 0; i < 13; ++i) {
        many_frames.push_back(R"(~"#)" + std::to_string(i) + R"(  fn() at /tmp/project/src/file.cpp:1\n")");
        many_seq.push_back(static_cast<unsigned long long>(20 + i));
    }
    auto truncated_bt = store.add("GdbCommand", "Backtrace", "bt", many_frames, true, many_seq);
    require(contains(truncated_bt.summary, "... truncated after 12 frames"),
            "backtrace summary should have stable truncation");

    auto threads = store.add("GdbCommand",
                             "Threads",
                             "info threads",
                             {R"(~"  Id   Target Id                                   Frame\n")",
                              R"(~"* 1    Thread 0x1 (LWP 101) \"main\"       read_session_value () at /tmp/project/examples/segfault.cpp:9\n")",
                              R"(~"  2    Thread 0x2 (LWP 102) \"worker\"     worker () at /tmp/project/src/worker.cpp:42\n")",
                              R"(~"  3    Thread 0x3 (LWP 103)              0x00007f in poll () from /lib/libc.so.6\n")"},
                             false,
                             {40, 41, 42, 43});
    require(!contains(threads.summary, "Target Id"), "thread summary should skip header");
    require(contains(threads.summary, "* 1    Thread 0x1"), "thread summary should mark current thread");
    require(contains(threads.summary, "\"main\""), "thread summary should keep thread name");
    require(contains(threads.summary, "LWP 102"), "thread summary should keep LWP id");
    require(contains(threads.summary, "- 2    Thread 0x2"), "thread summary should include other threads");
    require(contains(threads.summary, "worker.cpp:42"), "thread summary should keep stopped frame");

    std::vector<std::string> many_threads{R"(~"  Id   Target Id Frame\n")"};
    std::vector<unsigned long long> thread_seq{60};
    for (int i = 1; i <= 25; ++i) {
        many_threads.push_back(R"(~"  )" + std::to_string(i) + R"(    Thread 0x1 worker ()\n")");
        thread_seq.push_back(static_cast<unsigned long long>(60 + i));
    }
    auto truncated_threads = store.add("GdbCommand", "Threads", "info threads", many_threads, false, thread_seq);
    require(contains(truncated_threads.summary, "... truncated after 24 threads"),
            "thread summary should have stable truncation");

    fs::remove_all(assets);
}

int main() {
    test_mi_value_parser();
    test_mi_record_audit();
    test_mi_record_summary();
    test_cpp_sanitizer();
    test_evidence_store_integration();
    std::cout << "mi_summary_tests ok\n";
    return 0;
}
