#include "../src/workflow/hypothesis.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

static void require(bool ok, const std::string &message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

static void require_status(const std::string &assertion,
                           const std::string &observed,
                           const std::string &expected,
                           const std::string &status) {
    auto result = evaluate_hypothesis_assertion(assertion, observed, expected);
    require(result.status == status,
            assertion + " expected " + status + " but got " + result.status + " reason=" + result.reason);
}

int main() {
    require_status("none", "$1 = 42", "", "passed");
    require_status("", "$1 = 42", "", "passed");

    require_status("contains", "$1 = read_session_value", "read_session", "passed");
    require_status("contains", "$1 = read_session_value", "write_session", "failed");
    require_status("not_contains", "$1 = read_session_value", "write_session", "passed");
    require_status("not_contains", "$1 = read_session_value", "read_session", "failed");

    require_status("is_null", "$1 = 0x0", "", "passed");
    require_status("is_null", "$1 = 0x1234", "", "failed");
    require_status("non_null", "$1 = 0x1234", "", "passed");
    require_status("non_null", "$1 = 0", "", "failed");

    require_status("equals", "  $1 = 7  ", "$1 = 7", "passed");
    require_status("equals", "  $1 = 7  ", "  $1 = 7  ", "passed");
    require_status("equals", "$1 = 7", "$1 = 8", "failed");
    require_status("not_equals", "$1 = 7", "$1 = 8", "passed");
    require_status("not_equals", "$1 = 7", "  $1 = 8  ", "passed");
    require_status("not_equals", "$1 = 7", "$1 = 7", "failed");

    require_status("greater_than", "$1 = 42", "41", "passed");
    require_status("greater_than", "$1 = 42", "42", "failed");
    require_status("less_than", "$2 = -7", "-6", "passed");
    require_status("less_than", "$2 = -7", "-8", "failed");
    require_status("greater_equal", "$3 = 0x10", "16", "passed");
    require_status("greater_equal", "$3 = 0x10", "0x11", "failed");
    require_status("less_equal", "value = 17", "0x11", "passed");
    require_status("less_equal", "value = 17", "16", "failed");
    require_status("equals_number", "$4 = 0x10", "16", "passed");
    require_status("equals_number", "$4 = 0x10", "17", "failed");
    require_status("not_equals_number", "$4 = 0x10", "17", "passed");
    require_status("not_equals_number", "$4 = 0x10", "16", "failed");

    require_status("contains", "", "$1", "unknown");
    require_status("is_null", "", "", "unknown");
    require_status("non_null", "", "", "unknown");
    require_status("contains", "$1 = 7", "", "unknown");
    require_status("equals", "$1 = 7", "", "unknown");
    require_status("greater_than", "$1 = 7", "", "unknown");
    require_status("greater_than", "no numeric value", "0", "unknown");
    require_status("greater_than", "first = 7 second = 8", "0", "unknown");
    require_status("greater_than", "$1 = 7", "no expected number", "unknown");
    require_status("greater_than", "$1 = 7", "min = 0 max = 1", "unknown");
    require_status("definitely_not_supported", "$1 = 7", "", "unknown");

    std::cout << "hypothesis_assertion_tests ok\n";
    return 0;
}
