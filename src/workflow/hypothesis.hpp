#pragma once

#include <string>

struct HypothesisAssertionResult {
    std::string status;
    std::string reason;
};

HypothesisAssertionResult evaluate_hypothesis_assertion(const std::string &assertion,
                                                        const std::string &observed,
                                                        const std::string &expected);
