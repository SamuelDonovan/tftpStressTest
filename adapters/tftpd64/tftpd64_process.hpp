#ifndef TFTP_TEST_HARNESS_ADAPTERS_TFTPD64_PROCESS_HPP
#define TFTP_TEST_HARNESS_ADAPTERS_TFTPD64_PROCESS_HPP

// ---------------------------------------------------------------------------
// Shared plumbing for running Tftpd64 (a Win32 binary) under Wine. Both the
// server adapter and the GUI-client adapter need the same few things: turn a
// POSIX path into the Z: path Wine hands to the program, claim a free port,
// launch the binary against a private ini directory, and kill exactly the
// instance we launched.
// ---------------------------------------------------------------------------

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace tftp_test_harness::tftpd64 {

// Where the Wine-hosted binary lives. Comes from the environment so no path is
// baked into the tree.
struct WineEnvironment {
    std::string wine_binary;  // $TFTPD64_WINE
    std::string executable;   // $TFTPD64_EXE
    std::string wine_prefix;  // $TFTPD64_WINEPREFIX

    static WineEnvironment from_environment();
};

// Wine maps the POSIX filesystem to Z:, so an absolute path becomes a Windows
// path by prefixing Z: and flipping the separators. Tftpd64 feeds these to
// GetFileAttributes/CreateFile, so they must be Windows-shaped.
std::string to_windows_path(const std::filesystem::path& path);

std::uint16_t pick_free_udp_port();
bool port_is_free(std::uint16_t port);
bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds limit);

// Wine does not keep the exec'd Windows process on the pid we forked, so the
// only reliable handle on "our" instance is its environment: each one gets a
// unique TFTPD32_INI_DIR. Matching on that never touches an unrelated Wine app.
std::vector<int> find_processes_with_marker(const std::string& marker);

// Launch tftpd64.exe pointed at ini_directory (a Windows path holding a
// Tftpd32.ini). Returns the forked launcher pid, or -1 on failure.
int launch_tftpd64(const WineEnvironment& wine,
                   const std::string& ini_directory_windows);

// Killing a Wine launcher does not reap the Windows process it spawned, so a
// timed-out driver keeps running and wedges the GUI for the next transfer.
// Match those leftovers on their command line and kill them.
std::vector<int> find_processes_with_cmdline(const std::string& fragment);

// Kill the launcher and the Windows process it spawned.
void kill_tftpd64(int launcher_pid, const std::string& marker);

} // namespace tftp_test_harness::tftpd64

#endif // TFTP_TEST_HARNESS_ADAPTERS_TFTPD64_PROCESS_HPP
