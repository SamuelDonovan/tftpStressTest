#ifndef TFTP_TEST_HARNESS_ADAPTERS_SUBPROCESS_CLIENT_ADAPTER_HPP
#define TFTP_TEST_HARNESS_ADAPTERS_SUBPROCESS_CLIENT_ADAPTER_HPP

// ---------------------------------------------------------------------------
// A reusable base adapter for evaluating a TFTP *client* that is an external
// command-line binary (for example the system `tftp` program). The base handles
// launching the process pointed at the impairment proxy's host/port, enforcing
// a timeout, and capturing the exit code. A concrete adapter only supplies the
// argument vector for a read or a write request and declares which capabilities
// the binary supports.
//
// This keeps the "one plug-in point" property: an integrator wanting to assess a
// CLI TFTP client subclasses this and implements two small argument builders.
// ---------------------------------------------------------------------------

#include "tftp_test_harness/adapter_interface.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace tftp_test_harness {

// The result of running a subprocess: its exit code (or a sentinel), whether it
// was killed by the timeout, and its captured stderr for the report narrative.
struct SubprocessResult {
    int exit_code = -1;
    bool timed_out = false;
    bool launch_failed = false;
    std::string captured_output;
    std::chrono::milliseconds duration{0};
};

// Run argv[0] with the given arguments in working_directory, bounded by timeout.
// POSIX implementation (fork/exec/waitpid); on Windows this returns
// launch_failed=true until a CreateProcess variant is supplied (documented
// limitation — the socket shim is the portable core; subprocess launching is a
// convenience helper).
SubprocessResult run_subprocess(const std::vector<std::string>& argv,
                                const std::string& working_directory,
                                std::chrono::milliseconds timeout);

// Base class for a CLI TFTP client under test.
class SubprocessClientAdapter : public ClientAdapter {
public:
    // Subclasses build the exact argument vector the binary needs. host/port is
    // the impairment proxy endpoint the binary MUST talk to.
    virtual std::vector<std::string> build_read_arguments(
        const std::string& host, std::uint16_t port,
        const ReadRequestSpecification& specification) const = 0;
    virtual std::vector<std::string> build_write_arguments(
        const std::string& host, std::uint16_t port,
        const WriteRequestSpecification& specification) const = 0;

    // The per-transfer timeout (bounds a hung binary).
    virtual std::chrono::milliseconds transfer_timeout() const {
        return std::chrono::seconds(20);
    }

    TransferResult perform_read_request(
        const EndpointConfiguration& endpoint,
        const ReadRequestSpecification& specification) override;
    TransferResult perform_write_request(
        const EndpointConfiguration& endpoint,
        const WriteRequestSpecification& specification) override;
};

} // namespace tftp_test_harness

#endif // TFTP_TEST_HARNESS_ADAPTERS_SUBPROCESS_CLIENT_ADAPTER_HPP
