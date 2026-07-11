#ifndef TFTP_TEST_HARNESS_METRICS_METRICS_STORE_HPP
#define TFTP_TEST_HARNESS_METRICS_METRICS_STORE_HPP

// ---------------------------------------------------------------------------
// The structured metrics store. One record per (test ID, impairment intensity)
// captures everything the report needs to be objective: the outcome, its
// severity and RFC clause, the measured counters, the PRNG seed for exact
// reproduction, and a plain-language failure narrative. Records are appended as
// newline-delimited JSON (one self-contained object per line) so a long,
// checkpointed overnight run can stream results to disk and resume.
// ---------------------------------------------------------------------------

#include "metrics/json_writer.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tftp_test_harness::metrics {

// PASS / FAIL / SKIPPED — the three outcomes the matrix mandates. SKIPPED means
// a required capability is unsupported; it is never counted as a failure.
enum class Outcome { Pass, Fail, Skipped };

// Severity aligned with the matrix (RFC 2119 keyword strength).
enum class Severity { Critical, Major, Minor, Info };

const char* outcome_name(Outcome outcome);
const char* severity_name(Severity severity);
Outcome outcome_from_name(const std::string& name);
Severity severity_from_name(const std::string& name);

struct MetricRecord {
    std::string test_id;       // e.g. "F-01"
    std::string title;         // human-readable behavior under test
    std::string rfc_clause;    // e.g. "RFC 1350 section 2"
    Severity severity = Severity::Info;
    Outcome outcome = Outcome::Skipped;

    // For a swept F-series test, the intensity point (e.g. loss probability).
    // NaN / unset for non-swept tests.
    std::optional<double> intensity;
    std::string intensity_label; // e.g. "loss=25%"

    std::uint64_t seed = 0;
    std::string narrative; // what the implementation did vs what the RFC requires

    // Whether this transfer reported success but delivered non-matching bytes —
    // the CRITICAL integrity failure that dominates the scoreboard.
    bool integrity_violation = false;

    // Whether the failure was a hang/crash rather than a clean error
    // (graceful-degradation accounting).
    bool ungraceful_failure = false;

    // Free-form measured counters (retransmissions, packets on wire, bytes,
    // throughput, duration, etc.).
    std::map<std::string, double> counters;

    std::string implementation_name; // identity for the report

    JsonValue to_json() const;
    static MetricRecord from_json(const JsonValue& value);
};

// Append-only writer to a newline-delimited JSON file.
class MetricsStore {
public:
    // Open (creating/truncating) the store at `path` and write a header line.
    bool open(const std::string& path, const std::string& implementation_name);

    // Append one record (flushed immediately for crash-resilient checkpointing).
    void append(const MetricRecord& record);

    void close();
    ~MetricsStore();

    const std::vector<MetricRecord>& records() const { return records_; }
    const std::string& implementation_name() const {
        return implementation_name_;
    }

    // Load a previously written store (for the report and comparison modes).
    static bool load(const std::string& path, MetricsStore& out);

private:
    std::string path_;
    std::string implementation_name_;
    std::vector<MetricRecord> records_;
    bool open_ = false;
};

} // namespace tftp_test_harness::metrics

#endif // TFTP_TEST_HARNESS_METRICS_METRICS_STORE_HPP
