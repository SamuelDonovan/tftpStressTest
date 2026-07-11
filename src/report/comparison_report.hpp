#ifndef TFTP_TEST_HARNESS_REPORT_COMPARISON_REPORT_HPP
#define TFTP_TEST_HARNESS_REPORT_COMPARISON_REPORT_HPP

// ---------------------------------------------------------------------------
// Comparison mode. Given two metrics stores produced by running two
// implementations independently through the identical suite, this diffs the six
// scoring axes and, crucially, attributes every divergence to specific test IDs
// and RFC clauses — so a reader can justify, with evidence, which
// implementation is superior on each axis.
// ---------------------------------------------------------------------------

#include "report/report_model.hpp"

#include <string>

namespace tftp_test_harness::report {

// Render a single-file HTML comparison of two scorecards (A vs B).
std::string render_comparison_report(const Scorecard& a, const Scorecard& b);

} // namespace tftp_test_harness::report

#endif // TFTP_TEST_HARNESS_REPORT_COMPARISON_REPORT_HPP
