#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

template <typename T>
class Task {
public:
    struct promise_type {
        std::optional<T> value;
        std::exception_ptr error;

        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        std::suspend_never initial_suspend() noexcept {
            return {};
        }

        std::suspend_always final_suspend() noexcept {
            return {};
        }

        void return_value(T v) {
            value = std::move(v);
        }

        void unhandled_exception() {
            error = std::current_exception();
        }
    };

    explicit Task(std::coroutine_handle<promise_type> handle) : handle_(handle) {}
    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    Task(Task &&other) noexcept : handle_(std::exchange(other.handle_, {})) {}

    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    T get() {
        if (handle_.promise().error) {
            std::rethrow_exception(handle_.promise().error);
        }
        return std::move(*handle_.promise().value);
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

