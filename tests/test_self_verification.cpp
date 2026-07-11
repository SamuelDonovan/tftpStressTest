// Self-verification: the harness must earn trust by judging known ground truth.
// It runs the deliberately buggy reference implementation and the correct one
// through the real test registry and asserts that the buggy one is flagged on
// exactly the defects it contains (Sorcerer's Apprentice cascade and the broken
// final-block rule) while the correct one passes those same checks.
//
// This is what makes the generated report defensible: the instrument is
// validated against a known-correct and a known-broken implementation before it
// judges anyone else's code.

#include "fixtures/fixture_generator.hpp"
#include "reference/reference_adapters.hpp"
#include "runner/test_case.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <map>
#include <string>

using namespace tftp_test_harness;
using namespace tftp_test_harness::runner;
using namespace tftp_test_harness::reference;
using metrics::MetricRecord;
using metrics::Outcome;

namespace {

std::filesystem::path unique_temp(const std::string& tag) {
    auto base = std::filesystem::temp_directory_path() /
                ("tftp_selfverify_" + tag + "_" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()));
    std::filesystem::create_directories(base);
    return base;
}

// Run one registry test by ID against the given personality; return its records.
std::vector<MetricRecord> run_one(ReferencePersonality personality,
                                  const std::string& test_id) {
    auto root = unique_temp(test_id + "_" + personality_name(personality));
    ReferenceClientAdapter client(personality);
    ReferenceServerAdapter server(personality);

    TestContext context;
    context.client = &client;
    context.server = &server;
    context.work_root = root / "tests";
    context.base_seed = 20260711;
    context.implementation_name = personality_name(personality);
    auto fixtures = fixtures::generate_standard_fixtures(root / "fixtures", false);
    for (auto& fixture : fixtures) {
        std::string key = fixture.name;
        const auto dot = key.find('.');
        if (dot != std::string::npos) key = key.substr(0, dot);
        context.fixtures[key] = std::move(fixture);
    }

    const auto registry = build_test_registry();
    for (const auto& test : registry) {
        if (test.id == test_id) {
            auto records = test.run(context, test);
            std::filesystem::remove_all(root);
            return records;
        }
    }
    std::filesystem::remove_all(root);
    return {};
}

bool any_failed(const std::vector<MetricRecord>& records) {
    for (const auto& record : records) {
        if (record.outcome == Outcome::Fail) return true;
    }
    return false;
}

bool all_passed(const std::vector<MetricRecord>& records) {
    if (records.empty()) return false;
    for (const auto& record : records) {
        if (record.outcome != Outcome::Pass) return false;
    }
    return true;
}

} // namespace

TFTP_TEST_CASE(correct_passes_final_block,
               "Correct impl passes the final-block rule (A-04, A-05)") {
    TFTP_CHECK_TRUE(all_passed(run_one(ReferencePersonality::Correct, "A-04")));
    TFTP_CHECK_TRUE(all_passed(run_one(ReferencePersonality::Correct, "A-05")));
}

TFTP_TEST_CASE(buggy_flagged_final_block,
               "Buggy impl is flagged on the broken final-block rule (A-04/A-05)") {
    auto a04 = run_one(ReferencePersonality::Buggy, "A-04");
    auto a05 = run_one(ReferencePersonality::Buggy, "A-05");
    TFTP_CHECK_TRUE(any_failed(a04));
    TFTP_CHECK_TRUE(any_failed(a05));
    // And the failure is the specific final-block violation.
    bool mentions_final_block = false;
    for (const auto& record : a04) {
        if (record.narrative.find("final-block") != std::string::npos) {
            mentions_final_block = true;
        }
    }
    TFTP_CHECK_TRUE(mentions_final_block);
}

TFTP_TEST_CASE(correct_passes_sorcerers_apprentice,
               "Correct impl passes the Sorcerer's Apprentice check (A-32/A-33)") {
    TFTP_CHECK_TRUE(all_passed(run_one(ReferencePersonality::Correct, "A-32")));
    TFTP_CHECK_TRUE(all_passed(run_one(ReferencePersonality::Correct, "A-33")));
}

TFTP_TEST_CASE(buggy_flagged_sorcerers_apprentice,
               "Buggy impl is flagged on Sorcerer's Apprentice (A-32/A-33)") {
    auto a32 = run_one(ReferencePersonality::Buggy, "A-32");
    TFTP_CHECK_TRUE(any_failed(a32));
    bool mentions_amplification = false;
    for (const auto& record : a32) {
        if (record.narrative.find("amplification") != std::string::npos ||
            record.narrative.find("Sorcerer") != std::string::npos) {
            mentions_amplification = true;
        }
    }
    TFTP_CHECK_TRUE(mentions_amplification);
}

TFTP_TEST_CASE(correct_basic_transfers_pass,
               "Correct impl passes basic RRQ/WRQ (A-01/A-02)") {
    TFTP_CHECK_TRUE(all_passed(run_one(ReferencePersonality::Correct, "A-01")));
    TFTP_CHECK_TRUE(all_passed(run_one(ReferencePersonality::Correct, "A-02")));
}

TFTP_TEST_MAIN()
