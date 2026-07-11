#include "report/report_model.hpp"

#include "tftp_test_harness/adapter_interface.hpp"

#include <algorithm>
#include <map>

namespace tftp_test_harness::report {

using metrics::MetricRecord;
using metrics::Outcome;
using metrics::Severity;

double severity_weight(Severity severity) {
    switch (severity) {
        case Severity::Critical: return 9.0;
        case Severity::Major: return 3.0;
        case Severity::Minor: return 1.0;
        case Severity::Info: return 0.0;
    }
    return 0.0;
}

namespace {

std::string section_of(const std::string& test_id) {
    if (test_id.empty()) return "?";
    return test_id.substr(0, 1);
}

// Map matrix capabilities to the section letters whose tests exercise them, so
// the capability matrix can be inferred from SKIPPED records.
const std::map<std::string, std::string>& capability_labels() {
    static const std::map<std::string, std::string> labels = {
        {capability::read_request, "RFC 1350 read (RRQ)"},
        {capability::write_request, "RFC 1350 write (WRQ)"},
        {capability::netascii_mode, "RFC 1350 netascii"},
        {capability::option_negotiation, "RFC 2347 option negotiation"},
        {capability::block_size, "RFC 2348 blksize"},
        {capability::timeout_option, "RFC 2349 timeout"},
        {capability::transfer_size, "RFC 2349 tsize"},
        {capability::window_size, "RFC 7440 windowsize"},
    };
    return labels;
}

} // namespace

Scorecard analyze(const metrics::MetricsStore& store) {
    Scorecard card;
    card.implementation_name = store.implementation_name();
    card.records = store.records();

    // Per-section weighted accumulation for the conformance score, over the
    // conformance sections A-E and G (F is resilience; H is reported too but
    // weighted minor within its section).
    struct Accum {
        int passed = 0, failed = 0, skipped = 0;
        double weight_passed = 0.0, weight_total = 0.0;
    };
    std::map<std::string, Accum> by_section;

    std::map<std::string, ResilienceCurve> curves; // F-xx -> curve

    for (const auto& record : store.records()) {
        const std::string section = section_of(record.test_id);
        Accum& accum = by_section[section];

        switch (record.outcome) {
            case Outcome::Pass:
                ++accum.passed;
                ++card.total_passed;
                break;
            case Outcome::Fail:
                ++accum.failed;
                ++card.total_failed;
                break;
            case Outcome::Skipped:
                ++accum.skipped;
                ++card.total_skipped;
                break;
        }
        if (record.outcome != Outcome::Skipped) {
            const double weight = severity_weight(record.severity);
            accum.weight_total += weight;
            if (record.outcome == Outcome::Pass) {
                accum.weight_passed += weight;
            }
        }

        // MUST failures (CRITICAL) listed out, never averaged away.
        if (record.outcome == Outcome::Fail &&
            record.severity == Severity::Critical) {
            std::string id = record.test_id;
            if (!record.intensity_label.empty()) {
                id += " (" + record.intensity_label + ")";
            }
            card.must_failure_ids.push_back(id);
        }

        // Integrity ledger.
        if (record.integrity_violation) {
            IntegrityEntry entry;
            entry.test_id = record.test_id;
            entry.intensity_label = record.intensity_label;
            entry.narrative = record.narrative;
            entry.seed = record.seed;
            card.integrity_violations.push_back(entry);
        }

        // Graceful degradation accounting (failures only).
        if (record.outcome == Outcome::Fail) {
            if (record.ungraceful_failure) {
                ++card.ungraceful_failures;
            } else {
                ++card.clean_failures;
            }
        }

        // Efficiency counters.
        auto counter = [&](const char* name) -> double {
            auto it = record.counters.find(name);
            return it == record.counters.end() ? 0.0 : it->second;
        };
        card.total_retransmissions += counter("retransmissions");
        card.total_data_packets += counter("data_packets_sent");

        // Resilience curves: F-series records carry an intensity + success.
        if (section == "F" && record.intensity.has_value()) {
            ResilienceCurve& curve = curves[record.test_id];
            curve.impairment_id = record.test_id;
            curve.title = record.title;
            ResiliencePoint point;
            point.intensity = *record.intensity;
            auto success_it = record.counters.find("success");
            point.success = success_it == record.counters.end()
                                ? (record.outcome == Outcome::Pass ? 1.0 : 0.0)
                                : success_it->second;
            point.label = record.intensity_label;
            curve.points.push_back(point);
        }
    }

    // Assemble section scores.
    double conformance_weight_passed = 0.0;
    double conformance_weight_total = 0.0;
    for (auto& entry : by_section) {
        SectionScore score;
        score.section = entry.first;
        score.passed = entry.second.passed;
        score.failed = entry.second.failed;
        score.skipped = entry.second.skipped;
        score.weighted_score =
            entry.second.weight_total > 0.0
                ? 100.0 * entry.second.weight_passed / entry.second.weight_total
                : 0.0;
        card.sections.push_back(score);
        // Conformance score aggregates A-E and G (not F, not H).
        if (entry.first == "A" || entry.first == "B" || entry.first == "C" ||
            entry.first == "D" || entry.first == "E" || entry.first == "G") {
            conformance_weight_passed += entry.second.weight_passed;
            conformance_weight_total += entry.second.weight_total;
        }
    }
    std::sort(card.sections.begin(), card.sections.end(),
              [](const SectionScore& a, const SectionScore& b) {
                  return a.section < b.section;
              });
    card.conformance_score =
        conformance_weight_total > 0.0
            ? 100.0 * conformance_weight_passed / conformance_weight_total
            : 0.0;

    // Resilience index: mean normalized area under each F success curve. Points
    // are ordered by intensity and integrated with the trapezoidal rule over
    // the observed intensity range, then normalized to [0,1].
    double area_sum = 0.0;
    int curve_count = 0;
    for (auto& entry : curves) {
        ResilienceCurve& curve = entry.second;
        std::sort(curve.points.begin(), curve.points.end(),
                  [](const ResiliencePoint& a, const ResiliencePoint& b) {
                      return a.intensity < b.intensity;
                  });
        if (curve.points.size() == 1) {
            curve.area = curve.points.front().success;
        } else if (curve.points.size() > 1) {
            double integral = 0.0;
            const double span = curve.points.back().intensity -
                                curve.points.front().intensity;
            for (std::size_t i = 1; i < curve.points.size(); ++i) {
                const double width = curve.points[i].intensity -
                                     curve.points[i - 1].intensity;
                const double mean_height =
                    0.5 * (curve.points[i].success + curve.points[i - 1].success);
                integral += width * mean_height;
            }
            curve.area = span > 0.0 ? integral / span : 0.0;
        }
        area_sum += curve.area;
        ++curve_count;
        card.curves.push_back(curve);
    }
    std::sort(card.curves.begin(), card.curves.end(),
              [](const ResilienceCurve& a, const ResilienceCurve& b) {
                  return a.impairment_id < b.impairment_id;
              });
    card.resilience_index = curve_count > 0 ? area_sum / curve_count : 0.0;

    // Graceful-degradation rate.
    const int total_failures = card.clean_failures + card.ungraceful_failures;
    card.graceful_degradation_rate =
        total_failures > 0
            ? static_cast<double>(card.clean_failures) / total_failures
            : 1.0;

    // Average throughput over successful transfers that reported one.
    double throughput_sum = 0.0;
    int throughput_count = 0;
    for (const auto& record : store.records()) {
        auto it = record.counters.find("throughput_bytes_per_second");
        if (it != record.counters.end() && it->second > 0.0 &&
            record.outcome == Outcome::Pass) {
            throughput_sum += it->second;
            ++throughput_count;
        }
    }
    card.average_throughput_bytes_per_second =
        throughput_count > 0 ? throughput_sum / throughput_count : 0.0;

    // Capability matrix: a capability is unsupported if any test was SKIPPED for
    // it. We infer support by scanning SKIPPED narratives for the capability
    // name; a capability with no SKIP is treated as supported.
    for (const auto& label : capability_labels()) {
        card.capability_supported[label.first] = true;
    }
    for (const auto& record : store.records()) {
        if (record.outcome == Outcome::Skipped) {
            for (const auto& label : capability_labels()) {
                if (record.narrative.find(label.first) != std::string::npos) {
                    card.capability_supported[label.first] = false;
                }
            }
        }
    }

    return card;
}

} // namespace tftp_test_harness::report
