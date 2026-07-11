#ifndef TFTP_TEST_HARNESS_REPORT_HTML_REPORT_HPP
#define TFTP_TEST_HARNESS_REPORT_HTML_REPORT_HPP

// ---------------------------------------------------------------------------
// The single-file HTML report generator. It renders a Scorecard into one
// self-contained HTML document: inline CSS, hand-generated inline SVG charts,
// no external assets, no JavaScript frameworks, no network fetch. The document
// is theme-aware (light/dark) and safe to open on an air-gapped machine.
//
// Sections: executive scorecard, capability matrix, per-section conformance
// tables, resilience curves (SVG), the integrity ledger, efficiency counters,
// and an appendix of per-test failure narratives, each linking test ID to RFC
// clause.
// ---------------------------------------------------------------------------

#include "report/report_model.hpp"

#include <string>

namespace tftp_test_harness::report {

// Render a single-implementation report to an HTML string.
std::string render_single_report(const Scorecard& scorecard);

// Shared CSS + small helpers reused by the comparison report.
std::string shared_report_css();
std::string html_escape(const std::string& raw);

// Render a small single-series line chart (success vs intensity) as inline SVG.
std::string render_resilience_svg(const ResilienceCurve& curve);

// Write text to a file. Returns success.
bool write_text_file(const std::string& path, const std::string& contents);

} // namespace tftp_test_harness::report

#endif // TFTP_TEST_HARNESS_REPORT_HTML_REPORT_HPP
