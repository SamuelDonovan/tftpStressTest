#ifndef TFTP_TEST_HARNESS_ADAPTERS_TFTPCLIENT_ADAPTER_HPP
#define TFTP_TEST_HARNESS_ADAPTERS_TFTPCLIENT_ADAPTER_HPP

// ---------------------------------------------------------------------------
// Adapter for SamuelDonovan/tftpClient — a C++11 CLI TFTP client implementing
// RFC 1350 plus the 2347/2348/2349/7440 option extensions.
//
//     tftp get <host> <port> <remote> <local>
//     tftp put <host> <port> <local>  <remote>
//     --blksize N --windowsize N --timeout N --retries N
//
// Two properties of the binary shape this adapter:
//
//  * It always negotiates: blksize, tsize and windowsize go out on every
//    request, and there is no switch to suppress them. So when a test requests
//    no options, we pin the transfer to --blksize 512 --windowsize 1, which is
//    lock-step 512-byte RFC 1350 behavior on the wire. Carrying the options in
//    the request is itself legal (RFC 2347: a server MAY ignore them), and the
//    harness's block-size expectations then line up with what actually flows.
//
//  * It is octet-only, so netascii is declared unsupported and those tests are
//    SKIPPED rather than FAILED.
//
// The binary's path comes from $TFTPCLIENT_BIN.
// ---------------------------------------------------------------------------

#include "subprocess_client_adapter.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace tftp_test_harness {

class TftpClientAdapter : public SubprocessClientAdapter {
public:
    explicit TftpClientAdapter(std::string binary_path)
        : binary_path_(std::move(binary_path)) {}

    static TftpClientAdapter from_environment() {
        const char* binary = std::getenv("TFTPCLIENT_BIN");
        if (binary == nullptr || *binary == '\0') {
            throw std::runtime_error(
                "tftpClient adapter: TFTPCLIENT_BIN is not set");
        }
        return TftpClientAdapter(binary);
    }

    std::string implementation_name() const override { return "tftpClient"; }
    std::string implementation_version() const override { return "1.0.0"; }

    bool supports_capability(const std::string& capability_name) const override {
        // Octet-only: no netascii mode on the command line or in the library.
        if (capability_name == capability::netascii_mode) return false;
        if (capability_name == capability::read_request ||
            capability_name == capability::write_request ||
            capability_name == capability::option_negotiation ||
            capability_name == capability::block_size ||
            capability_name == capability::timeout_option ||
            capability_name == capability::transfer_size ||
            capability_name == capability::window_size) {
            return true;
        }
        return false;
    }

    std::vector<std::string> build_read_arguments(
        const std::string& host, std::uint16_t port,
        const ReadRequestSpecification& specification) const override {
        std::vector<std::string> arguments{binary_path_, "get", host,
                                           std::to_string(port)};
        append_options(specification.requested_options, arguments);
        arguments.push_back(specification.remote_filename);
        arguments.push_back(specification.local_destination_path.string());
        return arguments;
    }

    std::vector<std::string> build_write_arguments(
        const std::string& host, std::uint16_t port,
        const WriteRequestSpecification& specification) const override {
        std::vector<std::string> arguments{binary_path_, "put", host,
                                           std::to_string(port)};
        append_options(specification.requested_options, arguments);
        arguments.push_back(specification.local_source_path.string());
        arguments.push_back(specification.remote_filename);
        return arguments;
    }

private:
    // A test that asks for nothing gets base-protocol behavior (512, lock-step);
    // a test that asks for an option gets exactly that value. The retransmission
    // timeout defaults to 1 s so that a worst-case retry chain stays inside the
    // harness's per-transfer watchdog instead of tripping it as a false "hang".
    static void append_options(const RequestedOptions& options,
                               std::vector<std::string>& arguments) {
        arguments.push_back("--blksize");
        arguments.push_back(std::to_string(options.block_size.value_or(512)));

        arguments.push_back("--windowsize");
        arguments.push_back(std::to_string(options.window_size.value_or(1)));

        arguments.push_back("--timeout");
        arguments.push_back(
            std::to_string(options.timeout_seconds.value_or(1)));

        // The retry budget is what decides how far up the loss curve the client
        // can still finish, so it stays tunable ($TFTPCLIENT_RETRIES): a run can
        // be repeated to show a resilience score is a property of the client and
        // not of the budget we handed it.
        if (const char* retries = std::getenv("TFTPCLIENT_RETRIES")) {
            arguments.push_back("--retries");
            arguments.push_back(retries);
        }
    }

    std::string binary_path_;
};

} // namespace tftp_test_harness

#endif // TFTP_TEST_HARNESS_ADAPTERS_TFTPCLIENT_ADAPTER_HPP
