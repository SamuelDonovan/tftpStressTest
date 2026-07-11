// tftp_report — turn a metrics store into a single self-contained HTML report.
//
// Usage: tftp_report <metrics.json> <report.html>

#include "metrics/metrics_store.hpp"
#include "report/html_report.hpp"
#include "report/report_model.hpp"

#include <iostream>
#include <string>

using namespace tftp_test_harness;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: tftp_report <metrics.json> <report.html>\n";
        return 2;
    }
    metrics::MetricsStore store;
    if (!metrics::MetricsStore::load(argv[1], store)) {
        std::cerr << "could not read metrics store: " << argv[1] << "\n";
        return 1;
    }
    const report::Scorecard card = report::analyze(store);
    const std::string html = report::render_single_report(card);
    if (!report::write_text_file(argv[2], html)) {
        std::cerr << "could not write report: " << argv[2] << "\n";
        return 1;
    }
    std::cout << "Report written to " << argv[2] << " ("
              << store.records().size() << " records; conformance "
              << card.conformance_score << ", resilience "
              << card.resilience_index << ", integrity violations "
              << card.integrity_violations.size() << ")\n";
    return 0;
}
