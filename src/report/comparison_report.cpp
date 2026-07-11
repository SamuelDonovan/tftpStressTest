#include "report/comparison_report.hpp"

#include "report/html_report.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>

namespace tftp_test_harness::report {

using metrics::MetricRecord;
using metrics::Outcome;

namespace {

std::string format_double(double value, int decimals) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

// One key per (test_id + intensity_label) so swept points line up across runs.
std::string record_key(const MetricRecord& record) {
    return record.test_id + "|" + record.intensity_label;
}

// A per-axis verdict comparing A and B (higher is better unless noted).
struct AxisRow {
    std::string axis;
    std::string value_a;
    std::string value_b;
    std::string winner; // "A", "B", or "tie"
    std::string note;
};

std::string winner_of(double a, double b, bool higher_better = true) {
    const double epsilon = 1e-9;
    if (std::abs(a - b) < epsilon) return "tie";
    const bool a_better = higher_better ? (a > b) : (a < b);
    return a_better ? "A" : "B";
}

std::string render_axis_table(const Scorecard& a, const Scorecard& b) {
    std::vector<AxisRow> rows;
    rows.push_back({"Conformance score",
                    format_double(a.conformance_score, 1),
                    format_double(b.conformance_score, 1),
                    winner_of(a.conformance_score, b.conformance_score),
                    "weighted over A-E, G"});
    rows.push_back({"MUST-level (CRITICAL) failures",
                    std::to_string(a.must_failure_ids.size()),
                    std::to_string(b.must_failure_ids.size()),
                    winner_of(static_cast<double>(a.must_failure_ids.size()),
                              static_cast<double>(b.must_failure_ids.size()),
                              false),
                    "fewer is better"});
    rows.push_back({"Resilience index",
                    format_double(a.resilience_index, 3),
                    format_double(b.resilience_index, 3),
                    winner_of(a.resilience_index, b.resilience_index),
                    "mean area under F curves"});
    rows.push_back({"Integrity violations",
                    std::to_string(a.integrity_violations.size()),
                    std::to_string(b.integrity_violations.size()),
                    winner_of(static_cast<double>(a.integrity_violations.size()),
                              static_cast<double>(b.integrity_violations.size()),
                              false),
                    "silent corruption; fewer is better"});
    rows.push_back(
        {"Graceful degradation",
         format_double(a.graceful_degradation_rate * 100.0, 0) + "%",
         format_double(b.graceful_degradation_rate * 100.0, 0) + "%",
         winner_of(a.graceful_degradation_rate, b.graceful_degradation_rate),
         "clean failures vs hangs"});
    rows.push_back(
        {"Avg throughput (B/s)",
         format_double(a.average_throughput_bytes_per_second, 0),
         format_double(b.average_throughput_bytes_per_second, 0),
         winner_of(a.average_throughput_bytes_per_second,
                   b.average_throughput_bytes_per_second),
         "passing transfers"});

    std::ostringstream out;
    out << "<div class=\"tablewrap\"><table><thead><tr><th>Axis</th>"
           "<th>A</th><th>B</th><th>Advantage</th><th>Note</th></tr></thead>"
           "<tbody>";
    for (const auto& row : rows) {
        std::string badge = "skip";
        std::string label = "tie";
        if (row.winner == "A") {
            badge = "pass";
            label = "A";
        } else if (row.winner == "B") {
            badge = "pass";
            label = "B";
        }
        out << "<tr><td>" << html_escape(row.axis) << "</td><td>"
            << html_escape(row.value_a) << "</td><td>" << html_escape(row.value_b)
            << "</td><td><span class=\"badge " << badge << "\">"
            << html_escape(label) << "</span></td><td class=\"sub\">"
            << html_escape(row.note) << "</td></tr>";
    }
    out << "</tbody></table></div>";
    return out.str();
}

std::string render_divergences(const Scorecard& a, const Scorecard& b) {
    std::map<std::string, const MetricRecord*> a_by_key;
    std::map<std::string, const MetricRecord*> b_by_key;
    for (const auto& record : a.records) a_by_key[record_key(record)] = &record;
    for (const auto& record : b.records) b_by_key[record_key(record)] = &record;

    struct Divergence {
        std::string test_id;
        std::string intensity;
        std::string rfc;
        std::string a_outcome;
        std::string b_outcome;
        std::string detail;
    };
    std::vector<Divergence> divergences;
    for (const auto& entry : a_by_key) {
        auto it = b_by_key.find(entry.first);
        if (it == b_by_key.end()) continue;
        const MetricRecord& ra = *entry.second;
        const MetricRecord& rb = *it->second;
        if (ra.outcome != rb.outcome ||
            ra.integrity_violation != rb.integrity_violation) {
            Divergence d;
            d.test_id = ra.test_id;
            d.intensity = ra.intensity_label;
            d.rfc = ra.rfc_clause;
            d.a_outcome = metrics::outcome_name(ra.outcome);
            d.b_outcome = metrics::outcome_name(rb.outcome);
            // Attribute the divergence: prefer the failing side's narrative.
            d.detail = ra.outcome == Outcome::Fail ? ra.narrative : rb.narrative;
            divergences.push_back(std::move(d));
        }
    }
    std::sort(divergences.begin(), divergences.end(),
              [](const Divergence& x, const Divergence& y) {
                  return x.test_id < y.test_id;
              });

    std::ostringstream out;
    if (divergences.empty()) {
        out << "<div class=\"callout good\">The two implementations produced "
               "identical outcomes on every shared test. No divergences to "
               "attribute.</div>";
        return out.str();
    }
    out << "<p class=\"sub\">" << divergences.size()
        << " test(s) where the two implementations diverged, each traced to its "
           "RFC clause.</p>";
    out << "<div class=\"tablewrap\"><table><thead><tr><th>Test</th>"
           "<th>Condition</th><th>A</th><th>B</th><th>RFC</th>"
           "<th>Explanation</th></tr></thead><tbody>";
    for (const auto& d : divergences) {
        out << "<tr><td class=\"mono\">" << html_escape(d.test_id)
            << "</td><td>" << html_escape(d.intensity) << "</td><td><span "
               "class=\"badge "
            << (d.a_outcome == "PASS" ? "pass"
                                      : d.a_outcome == "FAIL" ? "fail" : "skip")
            << "\">" << d.a_outcome << "</span></td><td><span class=\"badge "
            << (d.b_outcome == "PASS" ? "pass"
                                      : d.b_outcome == "FAIL" ? "fail" : "skip")
            << "\">" << d.b_outcome << "</span></td><td class=\"sub\">"
            << html_escape(d.rfc) << "</td><td class=\"narrative\">"
            << html_escape(d.detail) << "</td></tr>";
    }
    out << "</tbody></table></div>";
    return out.str();
}

std::string overall_verdict(const Scorecard& a, const Scorecard& b) {
    // A simple lexicographic verdict: integrity first (any violation is
    // disqualifying), then MUST failures, then conformance, then resilience.
    auto describe = [](const Scorecard& s) { return s.implementation_name; };
    if (a.integrity_violations.size() != b.integrity_violations.size()) {
        const bool a_win =
            a.integrity_violations.size() < b.integrity_violations.size();
        return "On the most damning axis — data integrity — " +
               html_escape(describe(a_win ? a : b)) +
               " is superior (fewer transfers reported success while "
               "delivering corrupted bytes).";
    }
    if (a.must_failure_ids.size() != b.must_failure_ids.size()) {
        const bool a_win = a.must_failure_ids.size() < b.must_failure_ids.size();
        return html_escape(describe(a_win ? a : b)) +
               " has fewer MUST-level (CRITICAL) conformance failures.";
    }
    if (std::abs(a.conformance_score - b.conformance_score) > 0.05) {
        const bool a_win = a.conformance_score > b.conformance_score;
        return html_escape(describe(a_win ? a : b)) +
               " has the higher weighted conformance score.";
    }
    if (std::abs(a.resilience_index - b.resilience_index) > 0.001) {
        const bool a_win = a.resilience_index > b.resilience_index;
        return html_escape(describe(a_win ? a : b)) +
               " is more resilient to adversarial network conditions.";
    }
    return "The two implementations are evenly matched across the scored axes.";
}

} // namespace

std::string render_comparison_report(const Scorecard& a, const Scorecard& b) {
    std::ostringstream out;
    out << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        << "<meta name=\"viewport\" content=\"width=device-width,"
           "initial-scale=1\">"
        << "<title>TFTP comparison — " << html_escape(a.implementation_name)
        << " vs " << html_escape(b.implementation_name) << "</title><style>"
        << shared_report_css() << "</style></head><body><div class=\"wrap\">";

    out << "<h1>TFTP Implementation Comparison</h1>";
    out << "<p class=\"sub\"><strong>A:</strong> "
        << html_escape(a.implementation_name) << "<br><strong>B:</strong> "
        << html_escape(b.implementation_name) << "</p>";

    out << "<div class=\"callout good\"><strong>Verdict.</strong> "
        << overall_verdict(a, b) << "</div>";

    out << "<h2>Scoring axes</h2>";
    out << render_axis_table(a, b);

    out << "<h2>Divergences (per test ID and RFC clause)</h2>";
    out << render_divergences(a, b);

    out << "<p class=\"foot-note\">Both implementations were run independently "
           "through the identical suite. Every divergence above is attributed to "
           "a stable test ID and the RFC clause it derives from.</p>";
    out << "</div></body></html>";
    return out.str();
}

} // namespace tftp_test_harness::report
