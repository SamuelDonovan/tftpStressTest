#ifndef TFTP_TEST_HARNESS_ADAPTERS_EXAMPLE_SYSTEM_TFTP_CLIENT_HPP
#define TFTP_TEST_HARNESS_ADAPTERS_EXAMPLE_SYSTEM_TFTP_CLIENT_HPP

// ---------------------------------------------------------------------------
// A worked example adapter for a common system TFTP client binary (the
// tftp-hpa `tftp` program found on most Linux distributions). It demonstrates
// how an integrator plugs a real CLI implementation into the harness by
// subclassing SubprocessClientAdapter and supplying only the argument vectors.
//
// tftp-hpa command-line form used here:
//     tftp <host> <port> -m <octet|netascii> -c get <remote> <local>
//     tftp <host> <port> -m <octet|netascii> -c put <local> <remote>
//
// The binary talks to <host>/<port> — which the harness sets to the impairment
// proxy — so all of its traffic passes through the fault injector and observer.
// tftp-hpa does not implement RFC 2347 option negotiation, so those
// capabilities are declared unsupported and the option tests are SKIPPED.
// ---------------------------------------------------------------------------

#include "subprocess_client_adapter.hpp"

#include <string>

namespace tftp_test_harness {

class SystemTftpClientAdapter : public SubprocessClientAdapter {
public:
    explicit SystemTftpClientAdapter(std::string binary_path = "tftp")
        : binary_path_(std::move(binary_path)) {}

    std::string implementation_name() const override {
        return "system-tftp-hpa-client";
    }
    std::string implementation_version() const override { return "unknown"; }

    bool supports_capability(const std::string& capability_name) const override {
        // tftp-hpa supports the base protocol and both transfer modes, but not
        // RFC 2347+ option negotiation on the client command line.
        if (capability_name == capability::read_request ||
            capability_name == capability::write_request ||
            capability_name == capability::netascii_mode) {
            return true;
        }
        return false;
    }

    std::vector<std::string> build_read_arguments(
        const std::string& host, std::uint16_t port,
        const ReadRequestSpecification& specification) const override {
        return {binary_path_,
                host,
                std::to_string(port),
                "-m",
                mode_string(specification.mode),
                "-c",
                "get",
                specification.remote_filename,
                specification.local_destination_path.string()};
    }

    std::vector<std::string> build_write_arguments(
        const std::string& host, std::uint16_t port,
        const WriteRequestSpecification& specification) const override {
        return {binary_path_,
                host,
                std::to_string(port),
                "-m",
                mode_string(specification.mode),
                "-c",
                "put",
                specification.local_source_path.string(),
                specification.remote_filename};
    }

private:
    static const char* mode_string(TransferMode mode) {
        return mode == TransferMode::Netascii ? "netascii" : "octet";
    }
    std::string binary_path_;
};

} // namespace tftp_test_harness

#endif // TFTP_TEST_HARNESS_ADAPTERS_EXAMPLE_SYSTEM_TFTP_CLIENT_HPP
