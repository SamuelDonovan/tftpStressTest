#ifndef TFTP_TEST_HARNESS_REPORT_REPORT_MODEL_HPP
#define TFTP_TEST_HARNESS_REPORT_REPORT_MODEL_HPP

// ---------------------------------------------------------------------------
// The analysis model: it reduces a metrics store into the six objective scoring
// axes the report must make legible:
//   1. conformance score  (weighted pass rate over A-E, G; MUST failures shown
//                           separately, never averaged away)
//   2. resilience index   (normalized area under each F success-rate curve)
//   3. integrity ledger   (transfers that reported success but delivered wrong
//                           bytes — the most damning metric)
//   4. graceful-degradation rate (clean failures vs hang/crash/corruption)
//   5. efficiency         (retransmissions, packets on wire, throughput)
//   6. capability matrix  (supported/unsupported; SKIPPED, never counted a fail)
//
// The HTML report renders this model; the comparison report diffs two of them.
// ---------------------------------------------------------------------------

#include "metrics/metrics_store.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tftp_test_harness::report {

struct SectionScore {
    std::string section;      // "A".."H"
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    double weighted_score = 0.0; // 0..100, weighting CRITICAL >> MAJOR >> MINOR
};

struct ResiliencePoint {
    double intensity = 0.0;
    double success = 0.0; // 1.0 == byte-exact completion
    std::string label;
};

struct ResilienceCurve {
    std::string impairment_id;   // "F-01"
    std::string title;
    std::vector<ResiliencePoint> points;
    double area = 0.0; // normalized area under the success curve, 0..1
};

struct IntegrityEntry {
    std::string test_id;
    std::string intensity_label;
    std::string narrative;
    std::uint64_t seed = 0;
};

struct Scorecard {
    std::string implementation_name;

    // 1. Conformance.
    double conformance_score = 0.0; // weighted, 0..100
    std::vector<SectionScore> sections;
    std::vector<std::string> must_failure_ids; // CRITICAL failures, listed out
    int total_passed = 0;
    int total_failed = 0;
    int total_skipped = 0;

    // 2. Resilience.
    double resilience_index = 0.0; // mean normalized area, 0..1
    std::vector<ResilienceCurve> curves;

    // 3. Integrity ledger.
    std::vector<IntegrityEntry> integrity_violations;

    // 4. Graceful degradation.
    int clean_failures = 0;
    int ungraceful_failures = 0;
    double graceful_degradation_rate = 1.0;

    // 5. Efficiency.
    double total_retransmissions = 0.0;
    double total_data_packets = 0.0;
    double average_throughput_bytes_per_second = 0.0;

    // 6. Capability matrix.
    std::map<std::string, bool> capability_supported; // capability -> supported

    // Full record set (for the appendix / per-test tables).
    std::vector<metrics::MetricRecord> records;
};

// Build a scorecard from a loaded metrics store.
Scorecard analyze(const metrics::MetricsStore& store);

// Numeric weight used for a severity in the conformance score.
double severity_weight(metrics::Severity severity);

} // namespace tftp_test_harness::report

#endif // TFTP_TEST_HARNESS_REPORT_REPORT_MODEL_HPP
