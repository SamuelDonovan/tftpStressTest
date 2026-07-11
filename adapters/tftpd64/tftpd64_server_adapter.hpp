#ifndef TFTP_TEST_HARNESS_ADAPTERS_TFTPD64_SERVER_ADAPTER_HPP
#define TFTP_TEST_HARNESS_ADAPTERS_TFTPD64_SERVER_ADAPTER_HPP

// ---------------------------------------------------------------------------
// Server adapter for Ph. Jounin's Tftpd64 (https://github.com/PJO2/tftpd64).
//
// Tftpd64 is a Win32 binary with no POSIX build, so on Linux it is driven
// through Wine. The adapter owns one Tftpd64 process per transfer: it writes a
// private Tftpd32.ini (served directory, listening port, option negotiation),
// launches the binary, waits for the UDP port to be bound, and tears the
// process down afterwards. The harness then interposes its impairment proxy in
// front of the returned endpoint, exactly as for an in-process server.
//
// Configuration comes from the environment so no path is baked into the tree:
//
//   TFTPD64_WINE       path to the wine binary
//   TFTPD64_EXE        path to tftpd64.exe
//   TFTPD64_WINEPREFIX wine prefix to run in
//
// Capabilities are declared from what the binary actually negotiates (probed
// against v4.74: blksize/timeout/tsize yes, RFC 7440 windowsize no), so the
// tests it cannot support are SKIPPED rather than FAILED.
// ---------------------------------------------------------------------------

#include "tftp_test_harness/adapter_interface.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace tftp_test_harness::tftpd64 {

// How the server is configured for a run. The retransmission settings matter to
// the adversarial (F-series) tests: Tftpd64's shipped default is a 3 s timeout
// with 6 retries, which can exceed the harness's per-transfer watchdog under
// heavy loss. A 1 s timeout keeps the server well inside the watchdog while
// staying far above the loopback round trip.
struct Tftpd64Config {
    std::string wine_binary;      // $TFTPD64_WINE
    std::string executable;       // $TFTPD64_EXE
    std::string wine_prefix;      // $TFTPD64_WINEPREFIX
    int timeout_seconds = 1;      // ini: Timeout
    int max_retransmit = 6;       // ini: MaxRetransmit
    int security_level = 1;       // ini: SecurityLevel (1 = STD, allows uploads)
    bool negotiate_options = true;// ini: Negociate (RFC 2347)

    // Populate from the environment. Throws std::runtime_error naming the
    // missing variable if the environment is not set up.
    static Tftpd64Config from_environment();
};

class Tftpd64ServerAdapter : public ServerAdapter {
public:
    explicit Tftpd64ServerAdapter(Tftpd64Config config)
        : config_(std::move(config)) {}
    ~Tftpd64ServerAdapter() override { stop(); }

    std::string implementation_name() const override;
    std::string implementation_version() const override;
    bool supports_capability(const std::string& capability_name) const override;

    EndpointConfiguration start(
        const std::filesystem::path& served_directory) override;
    void stop() override;

private:
    Tftpd64Config config_;
    std::filesystem::path instance_directory_;  // holds this instance's ini
    std::string ini_directory_marker_;          // unique; identifies our process
    int launcher_pid_ = -1;                     // the forked wine launcher
    std::uint16_t port_ = 0;
};

} // namespace tftp_test_harness::tftpd64

#endif // TFTP_TEST_HARNESS_ADAPTERS_TFTPD64_SERVER_ADAPTER_HPP
