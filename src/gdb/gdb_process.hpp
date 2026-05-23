#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <sys/types.h>

class GdbProcess {
public:
    pid_t pid = -1;
    int in_fd = -1;
    int out_fd = -1;
    std::string buffer;

    static GdbProcess spawn();

    GdbProcess() = default;
    ~GdbProcess();
    GdbProcess(const GdbProcess &) = delete;
    GdbProcess &operator=(const GdbProcess &) = delete;
    GdbProcess(GdbProcess &&other) noexcept;
    GdbProcess &operator=(GdbProcess &&other) noexcept;

    void close_all();
    void write_all(const std::string &data);
    std::optional<std::string> read_line(std::chrono::milliseconds timeout);
    void terminate();
};

