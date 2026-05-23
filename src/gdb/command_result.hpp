#pragma once

#include <string>
#include <vector>

struct CommandResult {
    std::string command;
    std::vector<std::string> raw_lines;
    std::string result_class;
    std::string stop_reason;
    std::string signal_name;
    std::string breakpoint_number;
    bool timed_out = false;
    bool exited = false;
};
