#include "runner/runner.hpp"

#include "fixtures/fixture_generator.hpp"

#include <chrono>

namespace tftp_test_harness::runner {

using metrics::MetricRecord;
using metrics::Outcome;
using metrics::Severity;

namespace {

bool both_support(ClientAdapter& client, ServerAdapter& server,
                  const std::string& capability) {
    return client.supports_capability(capability) &&
           server.supports_capability(capability);
}

// Map fixture descriptions to the logical keys the registry references.
std::map<std::string, fixtures::Fixture> build_fixture_map(
    const std::filesystem::path& directory, bool include_huge) {
    auto list = fixtures::generate_standard_fixtures(directory, include_huge);
    std::map<std::string, fixtures::Fixture> map;
    for (auto& fixture : list) {
        std::string key = fixture.name;
        const auto dot = key.find('.');
        if (dot != std::string::npos) key = key.substr(0, dot);
        map[key] = std::move(fixture);
    }
    // Alias keys used by the registry.
    if (map.count("netascii_mixed")) map["netascii_mixed"] = map["netascii_mixed"];
    return map;
}

} // namespace

RunSummary run_suite(ClientAdapter& client, ServerAdapter& server,
                     const RunnerOptions& options) {
    RunSummary summary;

    std::filesystem::path work_root = options.work_root;
    if (work_root.empty()) {
        const auto unique = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        work_root = std::filesystem::temp_directory_path() /
                    ("tftp_harness_run_" + std::to_string(unique));
    }
    std::error_code ec;
    std::filesystem::create_directories(work_root, ec);

    const std::string implementation =
        client.implementation_name() + " / " + server.implementation_name();

    metrics::MetricsStore store;
    store.open(options.metrics_output_path, implementation);

    TestContext context;
    context.client = &client;
    context.server = &server;
    context.work_root = work_root / "tests";
    context.base_seed = options.base_seed;
    context.watchdog = options.watchdog;
    context.implementation_name = implementation;
    context.fixtures = build_fixture_map(work_root / "fixtures",
                                         options.include_huge_fixtures);

    const auto tests = build_test_registry();

    std::size_t index = 0;
    for (const auto& test : tests) {
        ++index;
        if (!options.id_filter.empty() &&
            test.id.find(options.id_filter) == std::string::npos) {
            continue;
        }

        // Capability resolution: unsupported => a single SKIPPED record.
        bool supported = true;
        std::string missing;
        for (const auto& capability : test.required_capabilities) {
            if (!both_support(client, server, capability)) {
                supported = false;
                missing = capability;
                break;
            }
        }
        if (!supported) {
            MetricRecord record;
            record.test_id = test.id;
            record.title = test.title;
            record.rfc_clause = test.rfc_clause;
            record.severity = Severity::Info;
            record.outcome = Outcome::Skipped;
            record.implementation_name = implementation;
            record.narrative =
                "capability '" + missing + "' not supported by the "
                "implementation under test";
            store.append(record);
            ++summary.total_records;
            ++summary.skipped;
            if (options.progress) {
                options.progress(test.id, index, tests.size(), "SKIPPED");
            }
            continue;
        }

        std::vector<MetricRecord> records;
        try {
            records = test.run(context, test);
        } catch (const std::exception& error) {
            MetricRecord record;
            record.test_id = test.id;
            record.title = test.title;
            record.rfc_clause = test.rfc_clause;
            record.severity = test.severity;
            record.outcome = Outcome::Fail;
            record.ungraceful_failure = true;
            record.implementation_name = implementation;
            record.narrative =
                std::string("test threw an exception: ") + error.what();
            records.push_back(record);
        }

        std::string worst = "PASS";
        for (auto& record : records) {
            if (record.implementation_name.empty()) {
                record.implementation_name = implementation;
            }
            store.append(record);
            ++summary.total_records;
            switch (record.outcome) {
                case Outcome::Pass: ++summary.passed; break;
                case Outcome::Fail:
                    ++summary.failed;
                    worst = "FAIL";
                    break;
                case Outcome::Skipped:
                    ++summary.skipped;
                    if (worst == "PASS") worst = "SKIPPED";
                    break;
            }
            if (record.integrity_violation) ++summary.integrity_violations;
            if (record.ungraceful_failure) ++summary.ungraceful_failures;
        }
        if (options.progress) {
            options.progress(test.id, index, tests.size(), worst);
        }
    }

    store.close();
    return summary;
}

} // namespace tftp_test_harness::runner
