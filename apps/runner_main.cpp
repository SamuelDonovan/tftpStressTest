// tftp_runner — drive a TFTP implementation through the full conformance and
// robustness suite and write a metrics store.
//
// Usage:
//   tftp_runner --impl <correct|buggy|base-only> --out <path> [--seed N]
//               [--filter A-] [--huge]
//
// The shipped reference personalities let the harness self-verify; a real
// integrator swaps in their own ClientAdapter/ServerAdapter here.

#include "reference/reference_adapters.hpp"
#include "runner/runner.hpp"

#include <iostream>
#include <string>

using namespace tftp_test_harness;

namespace {

reference::ReferencePersonality parse_personality(const std::string& name) {
    if (name == "buggy") return reference::ReferencePersonality::Buggy;
    if (name == "base-only")
        return reference::ReferencePersonality::OptionsUnsupported;
    return reference::ReferencePersonality::Correct;
}

void print_usage() {
    std::cout
        << "Usage: tftp_runner [--impl correct|buggy|base-only] [--out PATH]\n"
        << "                   [--seed N] [--filter SUBSTR] [--huge]\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string impl = "correct";
    runner::RunnerOptions options;
    options.metrics_output_path = "results.tftp-metrics.json";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--impl") {
            impl = next("--impl");
        } else if (arg == "--out") {
            options.metrics_output_path = next("--out");
        } else if (arg == "--seed") {
            options.base_seed =
                static_cast<std::uint64_t>(std::stoull(next("--seed")));
        } else if (arg == "--filter") {
            options.id_filter = next("--filter");
        } else if (arg == "--huge") {
            options.include_huge_fixtures = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            print_usage();
            return 2;
        }
    }

    const auto personality = parse_personality(impl);
    reference::ReferenceClientAdapter client(personality);
    reference::ReferenceServerAdapter server(personality);

    std::cout << "Running suite against: " << client.implementation_name()
              << " / " << server.implementation_name() << "\n";
    std::cout << "Metrics output: " << options.metrics_output_path << "\n\n";

    options.progress = [](const std::string& id, std::size_t index,
                          std::size_t total, const std::string& worst) {
        std::cout << "[" << index << "/" << total << "] " << id << " -> "
                  << worst << "\n";
    };

    const auto summary = runner::run_suite(client, server, options);

    std::cout << "\n=== Summary ===\n"
              << "records:  " << summary.total_records << "\n"
              << "passed:   " << summary.passed << "\n"
              << "failed:   " << summary.failed << "\n"
              << "skipped:  " << summary.skipped << "\n"
              << "integrity violations: " << summary.integrity_violations
              << "\n"
              << "ungraceful failures:  " << summary.ungraceful_failures << "\n";

    return 0;
}
