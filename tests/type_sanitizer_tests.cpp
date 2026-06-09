#include "../src/common/string_utils.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

static void require(bool ok, const std::string &message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

static void require_equal(const std::string &actual, const std::string &expected, const std::string &message) {
    if (actual != expected) {
        throw std::runtime_error(message + ": expected [" + expected + "], got [" + actual + "]");
    }
}

static bool contains(const std::string &text, const std::string &needle) {
    return text.find(needle) != std::string::npos;
}

static bool not_contains(const std::string &text, const std::string &needle) {
    return text.find(needle) == std::string::npos;
}

static std::string sanitize(const std::string &text) {
    return sanitize_output(text, "/tmp/project");
}

static void test_default_policy_compression() {
    require_equal(sanitize("std::vector<Foo, std::allocator<Foo> >"),
                  "std::vector<Foo>",
                  "default vector allocator should produce an exact compressed summary");
    require_equal(sanitize("std::map<Key, Value, std::less<Key>, std::allocator<std::pair<Key const, Value> > >"),
                  "std::map<Key, Value>",
                  "default map policies should produce an exact compressed summary");
    require_equal(sanitize("std::unordered_map<Key, Value, std::hash<Key>, std::equal_to<Key>, std::allocator<std::pair<const Key, Value> > >"),
                  "std::unordered_map<Key, Value>",
                  "default unordered_map policies should produce an exact compressed summary");
    require_equal(sanitize("std::unique_ptr<Foo, std::default_delete<Foo> >"),
                  "std::unique_ptr<Foo>",
                  "default unique_ptr deleter should produce an exact compressed summary");

    require(contains(sanitize("std::vector<Foo, std::allocator<Foo> >"),
                     "std::vector<Foo>"),
            "default vector allocator should be compressed");
    require(contains(sanitize("std::list<Foo, std::allocator<Foo> >"),
                     "std::list<Foo>"),
            "default list allocator should be compressed");
    require(contains(sanitize("std::deque<Foo, std::allocator<Foo> >"),
                     "std::deque<Foo>"),
            "default deque allocator should be compressed");
    require(contains(sanitize("std::set<Foo, std::less<Foo>, std::allocator<Foo> >"),
                     "std::set<Foo>"),
            "default set comparator/allocator should be compressed");
    require(contains(sanitize("std::map<Key, Value, std::less<Key>, std::allocator<std::pair<Key const, Value> > >"),
                     "std::map<Key, Value>"),
            "default map comparator/allocator should be compressed");
    require(contains(sanitize("std::unordered_map<Key, Value, std::hash<Key>, std::equal_to<Key>, std::allocator<std::pair<const Key, Value> > >"),
                     "std::unordered_map<Key, Value>"),
            "default unordered_map hash/equality/allocator should be compressed");
    require(contains(sanitize("std::unique_ptr<Foo, std::default_delete<Foo> >"),
                     "std::unique_ptr<Foo>"),
            "default unique_ptr deleter should be compressed");
}

static void test_custom_policy_preservation() {
    std::string vector = sanitize("std::vector<Foo, project::ArenaAllocator<Foo> >");
    require(contains(vector, "std::vector<Foo, project::ArenaAllocator<Foo>>"),
            "custom vector allocator should be preserved");

    std::string set = sanitize("std::set<Foo, my::TransparentLess, project::ArenaAllocator<Foo> >");
    require(contains(set, "std::set<Foo, my::TransparentLess, project::ArenaAllocator<Foo>>"),
            "custom set comparator and allocator should be preserved");

    std::string map_custom_compare = sanitize(
        "std::map<Key, Value, CustomCompare, std::allocator<std::pair<const Key, Value> > >");
    require(contains(map_custom_compare, "std::map<Key, Value, CustomCompare>"),
            "custom map comparator should be preserved");

    std::string map_custom_allocator = sanitize(
        "std::map<Key, Value, std::less<Key>, project::ArenaAllocator<std::pair<const Key, Value> > >");
    require(contains(map_custom_allocator,
                     "std::map<Key, Value, std::less<Key>, project::ArenaAllocator<std::pair<Key, Value>>>"),
            "custom map allocator should be preserved with comparator position");

    std::string unordered = sanitize(
        "std::unordered_map<Key, Value, MyHash, MyEqual, project::ArenaAllocator<std::pair<Key const, Value> > >");
    require(contains(unordered,
                     "std::unordered_map<Key, Value, MyHash, MyEqual, project::ArenaAllocator<std::pair<Key, Value>>>"),
            "custom unordered_map hash/equality/allocator should be preserved");

    std::string unique = sanitize("std::unique_ptr<Foo, FdCloser>");
    require(contains(unique, "std::unique_ptr<Foo, FdCloser>"),
            "custom unique_ptr deleter should be preserved");
    require(not_contains(unique, "std::default_delete"),
            "custom unique_ptr deleter should not be replaced with default_delete");
}

static void test_new_type_support() {
    require(contains(sanitize("std::basic_string_view<char, std::char_traits<char> >"),
                     "std::string_view"),
            "basic_string_view<char> should simplify to string_view");
    require(contains(sanitize("std::string_view"),
                     "std::string_view"),
            "string_view alias should remain readable");
    require(contains(sanitize("std::array< std::string_view, 2 >"),
                     "std::array<std::string_view, 2>"),
            "array spacing should normalize and preserve size");
    require(contains(sanitize("std::function< int(std::string_view, int) >"),
                     "std::function<int(std::string_view, int)>"),
            "function signature should remain intact");
    require(contains(sanitize("std::ratio<1l, 1000l>"),
                     "std::ratio<1, 1000>"),
            "ratio suffix and spacing noise should normalize");
    require(contains(sanitize("std::chrono::duration<long int, std::ratio<1l, 1000l> >"),
                     "std::chrono::duration<long int, std::ratio<1, 1000>>"),
            "chrono duration should keep rep and simplified period");
    require(contains(sanitize("std::chrono::time_point<std::chrono::steady_clock, std::chrono::duration<long int, std::ratio<1l, 1000000000l> > >"),
                     "std::chrono::time_point<std::chrono::steady_clock, std::chrono::duration<long int, std::ratio<1, 1000000000>>>"),
            "chrono time_point should keep clock and simplified duration");
}

static void test_nested_realistic_strings() {
    std::string nested = sanitize(
        "std::map<std::basic_string_view<char, std::char_traits<char> >, "
        "std::chrono::time_point<std::chrono::steady_clock, std::chrono::duration<long int, std::ratio<1l, 1000000000l> > >, "
        "std::less<std::basic_string_view<char, std::char_traits<char> > >, "
        "std::allocator<std::pair<std::basic_string_view<char, std::char_traits<char> > const, "
        "std::chrono::time_point<std::chrono::steady_clock, std::chrono::duration<long int, std::ratio<1l, 1000000000l> > > > > >");
    require(contains(nested,
                     "std::map<std::string_view, std::chrono::time_point<std::chrono::steady_clock, std::chrono::duration<long int, std::ratio<1, 1000000000>>>>"),
            "nested map should compress default policy while preserving chrono value");

    std::string custom_nested = sanitize(
        "std::unordered_map<std::string_view, std::array<int, 2>, project::ViewHash, project::ViewEqual, "
        "project::ArenaAllocator<std::pair<std::string_view const, std::array<int, 2> > > >");
    require(contains(custom_nested,
                     "std::unordered_map<std::string_view, std::array<int, 2>, project::ViewHash, project::ViewEqual, project::ArenaAllocator<std::pair<std::string_view, std::array<int, 2>>>>"),
            "nested unordered_map should preserve custom policies");
}

int main() {
    test_default_policy_compression();
    test_custom_policy_preservation();
    test_new_type_support();
    test_nested_realistic_strings();
    std::cout << "type_sanitizer_tests ok\n";
    return 0;
}
