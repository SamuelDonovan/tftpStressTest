#ifndef TFTP_TEST_HARNESS_TESTS_TEST_SUPPORT_HPP
#define TFTP_TEST_HARNESS_TESTS_TEST_SUPPORT_HPP

// ---------------------------------------------------------------------------
// A minimal, header-only test framework. "Standard library only" applies to the
// harness itself, so its own self-tests likewise pull in no third-party test
// framework (no GoogleTest / Catch). Tests register themselves at static-init
// time; main() runs them all and reports a pass/fail count and a nonzero exit
// on any failure.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace tftp_test_harness::testing {

struct TestCaseEntry {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCaseEntry>& registry() {
    static std::vector<TestCaseEntry> cases;
    return cases;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> body) {
        registry().push_back({std::move(name), std::move(body)});
    }
};

// Thrown by a failed assertion; carries the human-readable failure detail.
struct AssertionFailure {
    std::string message;
};

inline void fail(const std::string& expression, const std::string& file,
                 int line, const std::string& detail) {
    std::ostringstream out;
    out << file << ":" << line << ": assertion failed: " << expression;
    if (!detail.empty()) {
        out << "\n    " << detail;
    }
    throw AssertionFailure{out.str()};
}

inline int run_all_registered_tests() {
    int passed = 0;
    int failed = 0;
    for (const auto& test_case : registry()) {
        try {
            test_case.body();
            std::cout << "[ PASS ] " << test_case.name << "\n";
            ++passed;
        } catch (const AssertionFailure& failure) {
            std::cout << "[ FAIL ] " << test_case.name << "\n"
                      << "         " << failure.message << "\n";
            ++failed;
        } catch (const std::exception& error) {
            std::cout << "[ FAIL ] " << test_case.name
                      << " (unexpected exception: " << error.what() << ")\n";
            ++failed;
        } catch (...) {
            std::cout << "[ FAIL ] " << test_case.name
                      << " (unknown exception)\n";
            ++failed;
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed, "
              << (passed + failed) << " total\n";
    return failed == 0 ? 0 : 1;
}

} // namespace tftp_test_harness::testing

// Define a test case. Bodies are ordinary functions using the CHECK_* macros.
#define TFTP_TEST_CASE(unique_symbol, human_name)                            \
    static void unique_symbol();                                             \
    static ::tftp_test_harness::testing::Registrar                          \
        unique_symbol##_registrar(human_name, unique_symbol);               \
    static void unique_symbol()

#define TFTP_CHECK(condition)                                                 \
    do {                                                                     \
        if (!(condition)) {                                                  \
            ::tftp_test_harness::testing::fail(#condition, __FILE__,         \
                                               __LINE__, "");               \
        }                                                                    \
    } while (0)

#define TFTP_CHECK_EQUAL(actual, expected)                                   \
    do {                                                                     \
        auto&& tftp_actual_value = (actual);                                \
        auto&& tftp_expected_value = (expected);                            \
        if (!((tftp_actual_value) == (tftp_expected_value))) {              \
            std::ostringstream tftp_detail_stream;                          \
            tftp_detail_stream << "expected " << #actual << " == "          \
                               << #expected;                                \
            ::tftp_test_harness::testing::fail(#actual " == " #expected,    \
                                               __FILE__, __LINE__,          \
                                               tftp_detail_stream.str());   \
        }                                                                   \
    } while (0)

#define TFTP_CHECK_TRUE(condition) TFTP_CHECK(condition)
#define TFTP_CHECK_FALSE(condition) TFTP_CHECK(!(condition))

#define TFTP_TEST_MAIN()                                                     \
    int main() {                                                            \
        return ::tftp_test_harness::testing::run_all_registered_tests();    \
    }

#endif // TFTP_TEST_HARNESS_TESTS_TEST_SUPPORT_HPP
