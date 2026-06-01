#include "hypothesis.hpp"

#include "../common/string_utils.hpp"

#include <regex>
#include <utility>

namespace {

bool contains_null_marker(const std::string &observed) {
    return observed.find("0x0") != std::string::npos ||
           std::regex_search(observed, std::regex(R"((^|[=\s])0($|\s))")) ||
           observed.find("= nullptr") != std::string::npos ||
           observed.find("(nil)") != std::string::npos;
}

HypothesisAssertionResult passed() {
    return {"passed", ""};
}

HypothesisAssertionResult failed() {
    return {"failed", ""};
}

HypothesisAssertionResult unknown(std::string reason) {
    return {"unknown", std::move(reason)};
}

} // namespace

HypothesisAssertionResult evaluate_hypothesis_assertion(const std::string &assertion,
                                                        const std::string &observed,
                                                        const std::string &expected) {
    std::string normalized = assertion.empty() ? "none" : lower(assertion);
    std::string trimmed_expected = trim(expected);

    if (normalized == "none") {
        return passed();
    }

    if (normalized == "contains") {
        if (trimmed_expected.empty()) {
            return unknown("contains requires non-empty expected");
        }
        return observed.find(expected) != std::string::npos ? passed() : failed();
    }

    if (normalized == "not_contains") {
        if (trimmed_expected.empty()) {
            return unknown("not_contains requires non-empty expected");
        }
        return observed.find(expected) == std::string::npos ? passed() : failed();
    }

    if (normalized == "is_null") {
        return contains_null_marker(observed) ? passed() : failed();
    }

    if (normalized == "non_null") {
        return contains_null_marker(observed) ? failed() : passed();
    }

    if (normalized == "equals") {
        if (trimmed_expected.empty()) {
            return unknown("equals requires non-empty expected");
        }
        return trim(observed) == expected ? passed() : failed();
    }

    if (normalized == "not_equals") {
        if (trimmed_expected.empty()) {
            return unknown("not_equals requires non-empty expected");
        }
        return trim(observed) != expected ? passed() : failed();
    }

    return unknown("unknown assertion: " + assertion);
}
