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
#include "tftpclient/tftpclient_adapter.hpp"
#include "tftpd64/tftpd64_server_adapter.hpp"

#include <iostream>
#include <memory>
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
        << "Usage: tftp_runner [--impl correct|buggy|base-only|tftpd64|tftpclient]\n"
        << "                   [--out PATH] [--seed N] [--filter SUBSTR] [--huge]\n"
        << "\n"
        << "  tftpd64 runs the real Win32 server under Wine and needs\n"
        << "  TFTPD64_WINE, TFTPD64_EXE and TFTPD64_WINEPREFIX in the "
           "environment.\n"
        << "  tftpclient runs an external CLI client and needs TFTPCLIENT_BIN.\n";
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

    // Exactly one side is ever the implementation under test; the other is the
    // verified-correct reference engine, so every divergence is attributable to
    // the implementation being evaluated.
    //   tftpd64    — the real Win32 server, driven through Wine.
    //   tftpclient — an external CLI client, driven as a subprocess.
    const bool external_server = (impl == "tftpd64");
    const bool external_client = (impl == "tftpclient");
    const auto personality = (external_server || external_client)
                                 ? reference::ReferencePersonality::Correct
                                 : parse_personality(impl);

    reference::ReferenceClientAdapter reference_client(personality);
    reference::ReferenceServerAdapter reference_server(personality);

    std::unique_ptr<tftpd64::Tftpd64ServerAdapter> tftpd64_server;
    std::unique_ptr<TftpClientAdapter> tftpclient;
    try {
        if (external_server) {
            tftpd64_server = std::make_unique<tftpd64::Tftpd64ServerAdapter>(
                tftpd64::Tftpd64Config::from_environment());
        } else if (external_client) {
            tftpclient = std::make_unique<TftpClientAdapter>(
                TftpClientAdapter::from_environment());
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 2;
    }

    ClientAdapter& client = external_client
                                ? static_cast<ClientAdapter&>(*tftpclient)
                                : static_cast<ClientAdapter&>(reference_client);
    ServerAdapter& server = external_server
                                ? static_cast<ServerAdapter&>(*tftpd64_server)
                                : static_cast<ServerAdapter&>(reference_server);

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
