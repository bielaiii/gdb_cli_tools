#include "gdb_process.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <stdexcept>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr size_t kReadBufferCompactThreshold = 64 * 1024;

} // namespace

GdbProcess GdbProcess::spawn() {
    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        throw std::runtime_error("pipe failed");
    }

    pid_t child = fork();
    if (child < 0) {
        throw std::runtime_error("fork failed");
    }

    if (child == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        execlp("gdb", "gdb", "--interpreter=mi2", "--quiet", nullptr);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);

    int flags = fcntl(out_pipe[0], F_GETFL, 0);
    fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);

    GdbProcess p;
    p.pid = child;
    p.in_fd = in_pipe[1];
    p.out_fd = out_pipe[0];
    return p;
}

GdbProcess::~GdbProcess() {
    close_all();
}

GdbProcess::GdbProcess(GdbProcess &&other) noexcept {
    *this = std::move(other);
}

GdbProcess &GdbProcess::operator=(GdbProcess &&other) noexcept {
    if (this != &other) {
        close_all();
        pid = std::exchange(other.pid, -1);
        in_fd = std::exchange(other.in_fd, -1);
        out_fd = std::exchange(other.out_fd, -1);
        buffer = std::move(other.buffer);
        buffer_start = std::exchange(other.buffer_start, 0);
    }
    return *this;
}

void GdbProcess::close_all() {
    if (in_fd >= 0) {
        close(in_fd);
        in_fd = -1;
    }
    if (out_fd >= 0) {
        close(out_fd);
        out_fd = -1;
    }
}

void GdbProcess::write_all(std::string_view data) {
    const char *ptr = data.data();
    size_t left = data.size();
    while (left > 0) {
        ssize_t written = write(in_fd, ptr, left);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("write to gdb failed");
        }
        ptr += written;
        left -= static_cast<size_t>(written);
    }
}

std::optional<std::string> GdbProcess::read_line(std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        auto pos = buffer.find('\n', buffer_start);
        if (pos != std::string::npos) {
            std::string line(buffer.data() + buffer_start, pos - buffer_start);
            buffer_start = pos + 1;
            if (buffer_start >= kReadBufferCompactThreshold && buffer_start * 2 >= buffer.size()) {
                buffer.erase(0, buffer_start);
                buffer_start = 0;
            }
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return std::nullopt;
        }

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        timeval tv{};
        tv.tv_sec = static_cast<long>(remaining.count() / 1000);
        tv.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(out_fd, &read_set);
        int rc = select(out_fd + 1, &read_set, nullptr, nullptr, &tv);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("select failed");
        }
        if (rc == 0) {
            return std::nullopt;
        }

        std::array<char, 4096> chunk{};
        ssize_t n = read(out_fd, chunk.data(), chunk.size());
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            throw std::runtime_error("read from gdb failed");
        }
        if (n == 0) {
            if (buffer_start < buffer.size()) {
                std::string line(buffer.data() + buffer_start, buffer.size() - buffer_start);
                buffer.clear();
                buffer_start = 0;
                return line;
            }
            buffer.clear();
            buffer_start = 0;
            return std::nullopt;
        }
        buffer.append(chunk.data(), static_cast<size_t>(n));
    }
}

void GdbProcess::terminate() {
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status = 0;
        waitpid(pid, &status, 0);
        pid = -1;
    }
}
