#include "tftpd64/tftpd64_client_adapter.hpp"

#include "subprocess_client_adapter.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <unistd.h>
#endif

namespace tftp_test_harness::tftpd64 {

namespace {

using namespace std::chrono_literals;

std::string require_environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(
            std::string("tftpd64 client adapter: environment variable ") +
            name + " is not set");
    }
    return value;
}

// The GUI offers block size as a fixed dropdown; "Default" means "send no
// blksize option at all", which is what a test requesting no options wants.
std::string block_size_argument(const RequestedOptions& options) {
    if (!options.block_size.has_value()) return "Default";
    return std::to_string(*options.block_size);
}

// The driver prints "driver: tftp-error-code=N" when the client surfaced an
// RFC 1350 error code in its message box.
std::optional<std::uint16_t> parse_error_code(const std::string& output) {
    const std::string marker = "tftp-error-code=";
    const auto position = output.find(marker);
    if (position == std::string::npos) return std::nullopt;

    std::string digits;
    for (char character : output.substr(position + marker.size())) {
        if (character >= '0' && character <= '9') {
            digits += character;
        } else {
            break;
        }
    }
    if (digits.empty()) return std::nullopt;
    return static_cast<std::uint16_t>(std::stoi(digits));
}

int driver_timeout_seconds() {
    if (const char* seconds = std::getenv("TFTPD64_DRIVER_TIMEOUT")) {
        const int value = std::atoi(seconds);
        if (value > 0) return value;
    }
    return 25;
}

// Exit codes from driver.py.
constexpr int kDriverSuccess = 0;
constexpr int kDriverFailure = 1;
constexpr int kDriverUnsupported = 3;

} // namespace

Tftpd64ClientConfig Tftpd64ClientConfig::from_environment() {
    Tftpd64ClientConfig config;
    config.wine = WineEnvironment::from_environment();
    config.python = require_environment("TFTPD64_PYTHON");
    config.driver = require_environment("TFTPD64_DRIVER");
    if (const char* timeout = std::getenv("TFTPD64_TIMEOUT")) {
        config.timeout_seconds = std::atoi(timeout);
    }
    if (const char* retries = std::getenv("TFTPD64_MAXRETRANSMIT")) {
        config.max_retransmit = std::atoi(retries);
    }
    return config;
}

bool Tftpd64ClientAdapter::supports_capability(
    const std::string& capability_name) const {
    // From src/_gui/tftp_cli.c: the request carries blksize and tsize and the
    // mode is hardcoded "octet". There is no timeout option and no windowsize.
    if (capability_name == capability::read_request ||
        capability_name == capability::write_request ||
        capability_name == capability::option_negotiation ||
        capability_name == capability::block_size ||
        capability_name == capability::transfer_size) {
        return true;
    }
    return false; // netascii, timeout option, windowsize
}

void Tftpd64ClientAdapter::ensure_running() {
    if (launcher_pid_ > 0) return;

    idle_port_ = pick_free_udp_port();
    instance_directory_ =
        std::filesystem::temp_directory_path() /
        ("tftpd64_client_" + std::to_string(::getpid()));
    std::filesystem::create_directories(instance_directory_);
    ini_directory_marker_ = to_windows_path(instance_directory_);

    {
        // Services=1 is TFTP only: enabling the others makes DHCP/DNS/SNTP/
        // syslog try to bind privileged ports, which fails as an unprivileged
        // user and raises a modal "Bind error 10013" box that blocks the GUI.
        // "Keep transfer Gui=0" suppresses the progress window, leaving the
        // end-of-transfer message box as the single verdict the driver reads.
        std::ofstream ini(instance_directory_ / "Tftpd32.ini");
        ini << "[TFTPD32]\n"
            << "BaseDirectory=" << to_windows_path(instance_directory_) << "\n"
            << "TftpPort=" << idle_port_ << "\n"
            << "LocalIP=127.0.0.1\n"
            << "Services=1\n"
            << "Negociate=1\n"
            << "Hide=0\n"
            << "Keep transfer Gui=0\n"
            << "Beep=0\n"
            << "MD5=0\n"
            << "Enable IPv6=0\n"
            // Shipped defaults are a 3 s timeout and 6 retries, so the client
            // gives up ~21 s after a peer goes silent -- just past the harness's
            // 20 s watchdog, which would misreport correct give-up behavior as a
            // hang. 1 s keeps the same retry count (the thing conformance turns
            // on) while landing the give-up well inside the watchdog.
            << "Timeout=" << config_.timeout_seconds << "\n"
            << "MaxRetransmit=" << config_.max_retransmit << "\n";
    }

    launcher_pid_ = launch_tftpd64(config_.wine, ini_directory_marker_);
    if (launcher_pid_ < 0) {
        throw std::runtime_error("tftpd64 client adapter: fork() failed");
    }

    // Its own (unused) TFTP service binding is the cheapest signal from this
    // side that the process is alive; the driver then waits for the window.
    const std::uint16_t port = idle_port_;
    if (!wait_until([port] { return !port_is_free(port); }, 20s)) {
        shutdown();
        throw std::runtime_error(
            "tftpd64 client adapter: Tftpd64 did not start (is Wine working?)");
    }
}

void Tftpd64ClientAdapter::shutdown() {
    if (launcher_pid_ < 0 && ini_directory_marker_.empty()) return;

    kill_tftpd64(launcher_pid_, ini_directory_marker_);
    launcher_pid_ = -1;

    if (!instance_directory_.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(instance_directory_, ignored);
        instance_directory_.clear();
    }
    ini_directory_marker_.clear();
    idle_port_ = 0;
}

TransferResult Tftpd64ClientAdapter::drive(
    const std::string& operation, const EndpointConfiguration& endpoint,
    const std::filesystem::path& local_path, const std::string& remote_name,
    const RequestedOptions& options) {
    TransferResult result;

    try {
        ensure_running();
    } catch (const std::exception& error) {
        result.reported_error_message = error.what();
        return result;
    }

    // python.exe is a Windows program: the script path it opens must be a
    // Windows path, not the POSIX one the environment gave us.
    const std::vector<std::string> arguments{
        config_.wine.wine_binary,
        config_.python,
        to_windows_path(config_.driver),
        "--op",      operation,
        "--host",    endpoint.proxy_host,
        "--port",    std::to_string(endpoint.proxy_port),
        "--local",   to_windows_path(local_path),
        "--remote",  remote_name,
        "--blksize", block_size_argument(options),
        // The client ignores the ini timeout: tftp_cli.c uses a fixed 3 s timer
        // and 6 retries, so it gives up at ~21 s and recovers from loss slowly.
        // The driver must outlast that to see a verdict, and the run needs a
        // watchdog above the driver ($TFTPD64_DRIVER_TIMEOUT, default 25 s;
        // ordering: driver < subprocess < --watchdog).
        "--timeout", std::to_string(driver_timeout_seconds()),
    };

    const auto run =
        run_subprocess(arguments, /*working_directory=*/"",
                       std::chrono::seconds(driver_timeout_seconds() + 3));

    // SIGKILLing the Wine launcher does not reap the python.exe Wine spawned:
    // it would keep driving the GUI and corrupt the next transfer's result.
    if (run.timed_out) {
        for (const int pid : find_processes_with_cmdline("driver.py")) {
            ::kill(pid, SIGKILL);
        }
    }

    result.process_exit_code = run.exit_code;
    result.wall_clock_duration = run.duration;
    result.reported_error_message = run.captured_output;
    result.reported_error_code = parse_error_code(run.captured_output);

    if (run.launch_failed) {
        result.reported_error_message = "failed to launch the Tftpd64 driver";
        return result;
    }
    if (run.timed_out) {
        result.reported_error_message = "the Tftpd64 driver timed out";
        return result;
    }
    if (run.exit_code == kDriverUnsupported) {
        // The GUI cannot express this request (e.g. a block size absent from
        // its dropdown). Unsupported, not failed.
        result.unsupported_configuration = true;
        return result;
    }
    result.completed_successfully = (run.exit_code == kDriverSuccess);
    if (result.completed_successfully) {
        result.reported_error_message.clear();
    } else if (run.exit_code != kDriverFailure) {
        result.reported_error_message =
            "Tftpd64 driver error: " + run.captured_output;
    }
    return result;
}

TransferResult Tftpd64ClientAdapter::perform_read_request(
    const EndpointConfiguration& endpoint,
    const ReadRequestSpecification& specification) {
    return drive("get", endpoint, specification.local_destination_path,
                 specification.remote_filename,
                 specification.requested_options);
}

TransferResult Tftpd64ClientAdapter::perform_write_request(
    const EndpointConfiguration& endpoint,
    const WriteRequestSpecification& specification) {
    return drive("put", endpoint, specification.local_source_path,
                 specification.remote_filename,
                 specification.requested_options);
}

} // namespace tftp_test_harness::tftpd64
