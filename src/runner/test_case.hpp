#ifndef TFTP_TEST_HARNESS_RUNNER_TEST_CASE_HPP
#define TFTP_TEST_HARNESS_RUNNER_TEST_CASE_HPP

// ---------------------------------------------------------------------------
// A self-describing test object. Each test carries its stable matrix ID, the
// behavior under test, the RFC citation, the severity used to weight the score,
// and the capabilities it requires. Its run() function drives one or more
// transfers through the transfer driver and returns one metric record per
// result (an F-series sweep returns several — one per intensity point).
//
// The design keeps the observer objective (it measures) and the test cases
// declarative (they judge measured facts against an RFC-derived expectation),
// so the trail from a FAIL back to a test ID and RFC clause is explicit.
// ---------------------------------------------------------------------------

#include "fixtures/fixture_generator.hpp"
#include "metrics/metrics_store.hpp"
#include "tftp_test_harness/adapter_interface.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tftp_test_harness::runner {

// Everything a test needs to run against the implementation(s) under test.
struct TestContext {
    ClientAdapter* client = nullptr;
    ServerAdapter* server = nullptr;
    std::map<std::string, fixtures::Fixture> fixtures; // keyed by logical name
    std::filesystem::path work_root;                   // per-run temp root
    std::uint64_t base_seed = 0;
    std::string implementation_name;

    // A fresh per-test work directory (isolation).
    std::filesystem::path work_dir_for(const std::string& test_id) const {
        return work_root / test_id;
    }
    const fixtures::Fixture* fixture(const std::string& key) const {
        auto it = fixtures.find(key);
        return it == fixtures.end() ? nullptr : &it->second;
    }
};

struct TestCase {
    std::string id;
    std::string title;
    std::string rfc_clause;
    metrics::Severity severity = metrics::Severity::Info;
    // Capabilities that BOTH adapters must support for the test to be meaningful;
    // if any is unsupported the test is recorded SKIPPED (never FAILED).
    std::vector<std::string> required_capabilities;
    // run receives the context and its own TestCase (for metadata: id, title,
    // RFC clause, severity).
    std::function<std::vector<metrics::MetricRecord>(TestContext&,
                                                     const TestCase&)>
        run;
};

// The full ordered suite implementing every matrix ID (A through H).
std::vector<TestCase> build_test_registry();

} // namespace tftp_test_harness::runner

#endif // TFTP_TEST_HARNESS_RUNNER_TEST_CASE_HPP
