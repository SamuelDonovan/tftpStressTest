#include "tftpd64/tftpd64_server_adapter.hpp"

#include "tftpd64/tftpd64_process.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace tftp_test_harness::tftpd64 {

namespace {

using namespace std::chrono_literals;

std::string require_environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(
            std::string("tftpd64 adapter: environment variable ") + name +
            " is not set");
    }
    return value;
}

std::atomic<unsigned> g_instance_counter{0};

} // namespace

Tftpd64Config Tftpd64Config::from_environment() {
    Tftpd64Config config;
    config.wine_binary = require_environment("TFTPD64_WINE");
    config.executable = require_environment("TFTPD64_EXE");
    config.wine_prefix = require_environment("TFTPD64_WINEPREFIX");

    // The retransmission settings are the ones that decide how the server
    // behaves under the F-series impairments, so they stay tunable: a run can be
    // repeated with Tftpd64's shipped defaults (3 s / 6 retries) to show the
    // result is a property of the server and not of how we configured it.
    if (const char* timeout = std::getenv("TFTPD64_TIMEOUT")) {
        config.timeout_seconds = std::atoi(timeout);
    }
    if (const char* retries = std::getenv("TFTPD64_MAXRETRANSMIT")) {
        config.max_retransmit = std::atoi(retries);
    }
    return config;
}

std::string Tftpd64ServerAdapter::implementation_name() const {
    return "Tftpd64 (Wine)";
}

std::string Tftpd64ServerAdapter::implementation_version() const {
    return "4.74";
}

bool Tftpd64ServerAdapter::supports_capability(
    const std::string& capability_name) const {
    // Probed against v4.74: it OACKs blksize/timeout/tsize and correctly ignores
    // RFC 7440 windowsize, which it does not implement.
    if (capability_name == capability::window_size) return false;
    if (capability_name == capability::option_negotiation)
        return config_.negotiate_options;
    if (capability_name == capability::block_size ||
        capability_name == capability::timeout_option ||
        capability_name == capability::transfer_size) {
        return config_.negotiate_options;
    }
    if (capability_name == capability::read_request ||
        capability_name == capability::write_request ||
        capability_name == capability::netascii_mode) {
        return true;
    }
    return false;
}

EndpointConfiguration Tftpd64ServerAdapter::start(
    const std::filesystem::path& served_directory) {
    stop(); // never leak a previous instance

    port_ = pick_free_udp_port();

    const unsigned instance = ++g_instance_counter;
    instance_directory_ =
        std::filesystem::temp_directory_path() /
        ("tftpd64_cfg_" + std::to_string(::getpid()) + "_" +
         std::to_string(instance));
    std::filesystem::create_directories(instance_directory_);
    ini_directory_marker_ = to_windows_path(instance_directory_);

    // Tftpd64 reads Tftpd32.ini out of $TFTPD32_INI_DIR before it looks beside
    // the executable, which is what lets each instance have its own config.
    {
        std::ofstream ini(instance_directory_ / "Tftpd32.ini");
        ini << "[TFTPD32]\n"
            << "BaseDirectory=" << to_windows_path(served_directory) << "\n"
            << "TftpPort=" << port_ << "\n"
            << "LocalIP=127.0.0.1\n"
            << "Services=15\n"
            << "Negociate=" << (config_.negotiate_options ? 1 : 0) << "\n"
            << "Timeout=" << config_.timeout_seconds << "\n"
            << "MaxRetransmit=" << config_.max_retransmit << "\n"
            << "SecurityLevel=" << config_.security_level << "\n"
            << "UnixStrings=1\n"
            << "VirtualRoot=0\n"
            << "PXECompatibility=0\n"
            << "Hide=1\n"
            << "Beep=0\n"
            << "MD5=0\n"
            << "WinSize=0\n"
            << "Enable IPv6=0\n"
            << "Ignore ack for last TFTP packet=0\n"
            << "Max Simultaneous Transfers=100\n";
    }

    WineEnvironment wine;
    wine.wine_binary = config_.wine_binary;
    wine.executable = config_.executable;
    wine.wine_prefix = config_.wine_prefix;
    const int child = launch_tftpd64(wine, ini_directory_marker_);
    if (child < 0) throw std::runtime_error("tftpd64 adapter: fork() failed");

    launcher_pid_ = child;

    const std::uint16_t port = port_;
    if (!wait_until([port] { return !port_is_free(port); }, 20s)) {
        stop();
        throw std::runtime_error(
            "tftpd64 adapter: server did not bind UDP port " +
            std::to_string(port) + " (is Wine working?)");
    }

    EndpointConfiguration endpoint;
    endpoint.proxy_host = "127.0.0.1";
    endpoint.proxy_port = port_;
    return endpoint;
}

void Tftpd64ServerAdapter::stop() {
    if (launcher_pid_ < 0 && ini_directory_marker_.empty()) return;

    kill_tftpd64(launcher_pid_, ini_directory_marker_);
    launcher_pid_ = -1;

    if (port_ != 0) {
        const std::uint16_t port = port_;
        wait_until([port] { return port_is_free(port); }, 5s);
        port_ = 0;
    }

    if (!instance_directory_.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(instance_directory_, ignored);
        instance_directory_.clear();
    }
    ini_directory_marker_.clear();
}

} // namespace tftp_test_harness::tftpd64
