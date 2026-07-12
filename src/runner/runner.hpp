#ifndef TFTP_TEST_HARNESS_RUNNER_RUNNER_HPP
#define TFTP_TEST_HARNESS_RUNNER_RUNNER_HPP

// ---------------------------------------------------------------------------
// The suite runner. It generates the fixture catalogue, walks the test
// registry, resolves each test's required capabilities against the plugged-in
// adapters (unsupported => SKIPPED, never FAILED), drives the supported tests,
// and streams every result into the metrics store. Long F-series sweeps write
// results incrementally so an interrupted overnight run is resumable from the
// store.
// ---------------------------------------------------------------------------

#include "metrics/metrics_store.hpp"
#include "runner/test_case.hpp"
#include "tftp_test_harness/adapter_interface.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace tftp_test_harness::runner {

struct RunnerOptions {
    std::string metrics_output_path = "results.tftp-metrics.json";
    std::uint64_t base_seed = 0xC0FFEE1234ULL;
    std::filesystem::path work_root; // defaults to a temp dir if empty
    bool include_huge_fixtures = false;
    // Per-transfer hang bound; raise it for an implementation whose legitimate
    // give-up is slower than the default (see TestContext::watchdog).
    std::chrono::milliseconds watchdog{20000};
    // Optional substring filter: only run tests whose ID contains this string.
    std::string id_filter;
    // Progress callback (id, index, total, outcome-summary). Optional.
    std::function<void(const std::string&, std::size_t, std::size_t,
                       const std::string&)>
        progress;
};

struct RunSummary {
    std::size_t total_records = 0;
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    std::size_t integrity_violations = 0;
    std::size_t ungraceful_failures = 0;
};

// Run the whole suite against the given adapters and write the metrics store.
RunSummary run_suite(ClientAdapter& client, ServerAdapter& server,
                     const RunnerOptions& options);

} // namespace tftp_test_harness::runner

#endif // TFTP_TEST_HARNESS_RUNNER_RUNNER_HPP
