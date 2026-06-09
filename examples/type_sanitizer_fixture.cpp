#include <array>
#include <chrono>
#include <csignal>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace type_sanitizer {

struct Foo {
    int value = 0;
};

struct FdCloser {
    void operator()(Foo *ptr) const {
        delete ptr;
    }
};

template <typename T>
struct ArenaAllocator {
    using value_type = T;

    ArenaAllocator() = default;

    template <typename U>
    ArenaAllocator(const ArenaAllocator<U> &) {
    }

    T *allocate(std::size_t n) {
        return std::allocator<T>{}.allocate(n);
    }

    void deallocate(T *ptr, std::size_t n) {
        std::allocator<T>{}.deallocate(ptr, n);
    }
};

template <typename T, typename U>
bool operator==(const ArenaAllocator<T> &, const ArenaAllocator<U> &) {
    return true;
}

template <typename T, typename U>
bool operator!=(const ArenaAllocator<T> &, const ArenaAllocator<U> &) {
    return false;
}

struct TransparentLess {
    bool operator()(const Foo &lhs, const Foo &rhs) const {
        return lhs.value < rhs.value;
    }
};

struct ViewLess {
    bool operator()(std::string_view lhs, std::string_view rhs) const {
        return lhs < rhs;
    }
};

struct ViewHash {
    std::size_t operator()(std::string_view value) const {
        return std::hash<std::string_view>{}(value);
    }
};

struct ViewEqual {
    bool operator()(std::string_view lhs, std::string_view rhs) const {
        return lhs == rhs;
    }
};

struct DefaultTypes {
    std::vector<Foo> default_vector;
    std::set<Foo> default_set;
    std::map<std::string_view, std::chrono::steady_clock::time_point> default_map;
    std::unordered_map<std::string_view, std::array<int, 2>> default_unordered;
    std::unique_ptr<Foo> default_unique;
    std::array<std::string_view, 2> labels;
    std::function<int(std::string_view)> callback;
    std::chrono::duration<long, std::ratio<1, 1000>> timeout;
    std::chrono::steady_clock::time_point deadline;
};

struct CustomTypes {
    std::vector<Foo, ArenaAllocator<Foo>> custom_vector;
    std::set<Foo, TransparentLess, ArenaAllocator<Foo>> custom_set;
    std::map<std::string_view,
             std::chrono::steady_clock::time_point,
             ViewLess,
             ArenaAllocator<std::pair<const std::string_view, std::chrono::steady_clock::time_point>>>
        custom_map;
    std::unordered_map<std::string_view,
                       std::array<int, 2>,
                       ViewHash,
                       ViewEqual,
                       ArenaAllocator<std::pair<const std::string_view, std::array<int, 2>>>>
        custom_unordered;
    std::unique_ptr<Foo, FdCloser> custom_unique;
};

DefaultTypes g_default_types;
CustomTypes g_custom_types;

__attribute__((noinline)) int type_sanitizer_stop() {
    g_default_types.labels = {"alpha", "beta"};
    g_default_types.callback = [](std::string_view value) {
        return static_cast<int>(value.size());
    };
    g_default_types.timeout = std::chrono::duration<long, std::ratio<1, 1000>>(7);
    g_default_types.deadline = std::chrono::steady_clock::now();
    std::raise(SIGTRAP);
    return g_default_types.callback(g_default_types.labels[0]);
}

} // namespace type_sanitizer

int main() {
    return type_sanitizer::type_sanitizer_stop();
}
