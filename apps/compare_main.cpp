// tftp_compare — diff two metrics stores into a single self-contained HTML
// comparison report attributing every divergence to a test ID and RFC clause.
//
// Usage: tftp_compare <a.json> <b.json> <comparison.html>

#include "metrics/metrics_store.hpp"
#include "report/comparison_report.hpp"
#include "report/html_report.hpp"
#include "report/report_model.hpp"

#include <iostream>

using namespace tftp_test_harness;

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr
            << "Usage: tftp_compare <a.json> <b.json> <comparison.html>\n";
        return 2;
    }
    metrics::MetricsStore store_a, store_b;
    if (!metrics::MetricsStore::load(argv[1], store_a)) {
        std::cerr << "could not read metrics store A: " << argv[1] << "\n";
        return 1;
    }
    if (!metrics::MetricsStore::load(argv[2], store_b)) {
        std::cerr << "could not read metrics store B: " << argv[2] << "\n";
        return 1;
    }
    const report::Scorecard a = report::analyze(store_a);
    const report::Scorecard b = report::analyze(store_b);
    const std::string html = report::render_comparison_report(a, b);
    if (!report::write_text_file(argv[3], html)) {
        std::cerr << "could not write comparison: " << argv[3] << "\n";
        return 1;
    }
    std::cout << "Comparison written to " << argv[3] << "\n"
              << "  A: " << a.implementation_name << " (conformance "
              << a.conformance_score << ", integrity "
              << a.integrity_violations.size() << ")\n"
              << "  B: " << b.implementation_name << " (conformance "
              << b.conformance_score << ", integrity "
              << b.integrity_violations.size() << ")\n";
    return 0;
}
