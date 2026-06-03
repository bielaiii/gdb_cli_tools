#include "hypothesis.hpp"

#include "../common/string_utils.hpp"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <regex>
#include <vector>
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

std::optional<long long> parse_integer_token(const std::string &token) {
    if (token.empty()) {
        return std::nullopt;
    }
    char *end = nullptr;
    errno = 0;
    long long value = std::strtoll(token.c_str(), &end, 0);
    if (errno != 0 || end == token.c_str() || *end != '\0') {
        return std::nullopt;
    }
    return value;
}

std::vector<long long> extract_numeric_values(const std::string &text) {
    std::vector<long long> values;
    static const std::regex number_re(R"(([+-]?(?:0x[0-9a-fA-F]+|[0-9]+)))");
    for (std::sregex_iterator it(text.begin(), text.end(), number_re), end; it != end; ++it) {
        const auto &match = *it;
        size_t pos = static_cast<size_t>(match.position(1));
        if (pos > 0 && text[pos - 1] == '$') {
            continue;
        }
        if (pos > 0) {
            unsigned char previous = static_cast<unsigned char>(text[pos - 1]);
            if (std::isalnum(previous) || text[pos - 1] == '_') {
                continue;
            }
        }
        size_t after = pos + match.length(1);
        if (after < text.size()) {
            unsigned char next = static_cast<unsigned char>(text[after]);
            if (std::isalnum(next) || text[after] == '_') {
                continue;
            }
        }
        auto value = parse_integer_token(match.str(1));
        if (value) {
            values.push_back(*value);
        }
    }
    return values;
}

std::optional<long long> extract_single_numeric_value(const std::string &text, std::string &reason) {
    std::vector<long long> values = extract_numeric_values(text);
    if (values.empty()) {
        reason = "numeric assertion could not find an integer value";
        return std::nullopt;
    }
    long long first = values.front();
    for (long long value : values) {
        if (value != first) {
            reason = "numeric assertion found multiple different integer values";
            return std::nullopt;
        }
    }
    return first;
}

bool is_numeric_assertion(const std::string &assertion) {
    return assertion == "greater_than" ||
           assertion == "less_than" ||
           assertion == "greater_equal" ||
           assertion == "less_equal" ||
           assertion == "equals_number" ||
           assertion == "not_equals_number";
}

} // namespace

HypothesisAssertionResult evaluate_hypothesis_assertion(const std::string &assertion,
                                                        const std::string &observed,
                                                        const std::string &expected) {
    std::string normalized = assertion.empty() ? "none" : lower(assertion);
    std::string trimmed_observed = trim(observed);
    std::string trimmed_expected = trim(expected);

    if (normalized == "none") {
        return passed();
    }

    if (trimmed_observed.empty()) {
        return unknown(normalized + " requires non-empty observed");
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
        return trimmed_observed == trimmed_expected ? passed() : failed();
    }

    if (normalized == "not_equals") {
        if (trimmed_expected.empty()) {
            return unknown("not_equals requires non-empty expected");
        }
        return trimmed_observed != trimmed_expected ? passed() : failed();
    }

    if (is_numeric_assertion(normalized)) {
        if (trimmed_expected.empty()) {
            return unknown(normalized + " requires non-empty expected");
        }
        std::string observed_reason;
        std::string expected_reason;
        auto observed_value = extract_single_numeric_value(trimmed_observed, observed_reason);
        auto expected_value = extract_single_numeric_value(trimmed_expected, expected_reason);
        if (!observed_value) {
            return unknown(observed_reason);
        }
        if (!expected_value) {
            return unknown(expected_reason);
        }
        if (normalized == "greater_than") {
            return *observed_value > *expected_value ? passed() : failed();
        }
        if (normalized == "less_than") {
            return *observed_value < *expected_value ? passed() : failed();
        }
        if (normalized == "greater_equal") {
            return *observed_value >= *expected_value ? passed() : failed();
        }
        if (normalized == "less_equal") {
            return *observed_value <= *expected_value ? passed() : failed();
        }
        if (normalized == "equals_number") {
            return *observed_value == *expected_value ? passed() : failed();
        }
        if (normalized == "not_equals_number") {
            return *observed_value != *expected_value ? passed() : failed();
        }
    }

    return unknown("unknown assertion: " + assertion);
}
