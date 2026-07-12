#ifndef TFTP_TEST_HARNESS_ADAPTERS_TFTPD64_CLIENT_ADAPTER_HPP
#define TFTP_TEST_HARNESS_ADAPTERS_TFTPD64_CLIENT_ADAPTER_HPP

// ---------------------------------------------------------------------------
// Adapter for Tftpd64's TFTP *client*.
//
// Tftpd64 ships no command-line client: the client's transfer state machine
// lives in the GUI dialog (src/_gui/tftp_cli.c), driven by dialog controls and
// a window timer. It cannot be run headlessly, and porting it would mean
// testing our port rather than testing Tftpd64.
//
// So the real dialog is driven in place. A small Windows-side driver script
// (tools/tftpd64/driver.py, run under Wine) fills in the host/port/file fields,
// triggers the Get/Put handler exactly the way tftp_cli.c triggers it
// internally (WM_COMMAND to the client dialog), and reads the verdict back off
// the message box the client raises at the end of every transfer -- a success
// reads "N blocks transferred ... MD5: ...", a failure carries "Error #N".
//
// This adapter owns one long-lived Tftpd64 GUI process and runs the driver once
// per transfer. Environment:
//
//   TFTPD64_WINE / TFTPD64_EXE / TFTPD64_WINEPREFIX   (see tftpd64_process.hpp)
//   TFTPD64_PYTHON   path to a Windows python.exe (the embeddable build)
//   TFTPD64_DRIVER   path to driver.py
//
// Capabilities are declared from what the GUI client actually does: it always
// sends blksize + tsize and hardcodes octet mode, so netascii, the RFC 2349
// timeout option and RFC 7440 windowsize are unsupported. Its block size is a
// fixed dropdown (Default/128/512/1024/1468/2048/...), so a test asking for a
// value it does not offer is reported as an unsupported configuration
// (SKIPPED), not a failure -- see TransferResult::unsupported_configuration.
// ---------------------------------------------------------------------------

#include "tftp_test_harness/adapter_interface.hpp"
#include "tftpd64/tftpd64_process.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace tftp_test_harness::tftpd64 {

struct Tftpd64ClientConfig {
    WineEnvironment wine;
    std::string python;  // $TFTPD64_PYTHON  (Windows python.exe)
    std::string driver;  // $TFTPD64_DRIVER  (driver.py)

    // Retransmission budget. The retry count is what conformance turns on; the
    // interval only decides whether a correct give-up lands inside the harness's
    // watchdog. Tunable ($TFTPD64_TIMEOUT / $TFTPD64_MAXRETRANSMIT) so a result
    // can be shown to be a property of the client, not of how we configured it.
    int timeout_seconds = 1;
    int max_retransmit = 6;

    static Tftpd64ClientConfig from_environment();
};

class Tftpd64ClientAdapter : public ClientAdapter {
public:
    explicit Tftpd64ClientAdapter(Tftpd64ClientConfig config)
        : config_(std::move(config)) {}
    ~Tftpd64ClientAdapter() override { shutdown(); }

    std::string implementation_name() const override {
        return "Tftpd64 GUI client (Wine)";
    }
    std::string implementation_version() const override { return "4.74"; }
    bool supports_capability(const std::string& capability_name) const override;

    TransferResult perform_read_request(
        const EndpointConfiguration& endpoint,
        const ReadRequestSpecification& specification) override;

    TransferResult perform_write_request(
        const EndpointConfiguration& endpoint,
        const WriteRequestSpecification& specification) override;

private:
    // The GUI is started once and reused: each transfer only costs a driver
    // run, not a fresh Wine process and window.
    void ensure_running();
    void shutdown();

    TransferResult drive(const std::string& operation,
                         const EndpointConfiguration& endpoint,
                         const std::filesystem::path& local_path,
                         const std::string& remote_name,
                         const RequestedOptions& options);

    Tftpd64ClientConfig config_;
    std::filesystem::path instance_directory_;
    std::string ini_directory_marker_;
    int launcher_pid_ = -1;
    std::uint16_t idle_port_ = 0;  // its own server; unused, kept off port 69
};

} // namespace tftp_test_harness::tftpd64

#endif // TFTP_TEST_HARNESS_ADAPTERS_TFTPD64_CLIENT_ADAPTER_HPP
