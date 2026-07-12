#include "tftpd64/tftpd64_process.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>

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

} // namespace

WineEnvironment WineEnvironment::from_environment() {
    WineEnvironment wine;
    wine.wine_binary = require_environment("TFTPD64_WINE");
    wine.executable = require_environment("TFTPD64_EXE");
    wine.wine_prefix = require_environment("TFTPD64_WINEPREFIX");
    return wine;
}

std::string to_windows_path(const std::filesystem::path& path) {
    std::string windows = "Z:" + std::filesystem::absolute(path).string();
    for (char& character : windows) {
        if (character == '/') character = '\\';
    }
    return windows;
}

std::uint16_t pick_free_udp_port() {
    const int probe = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (probe < 0) throw std::runtime_error("tftpd64 adapter: socket() failed");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof address) !=
        0) {
        ::close(probe);
        throw std::runtime_error("tftpd64 adapter: bind() failed");
    }
    socklen_t length = sizeof address;
    if (::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) !=
        0) {
        ::close(probe);
        throw std::runtime_error("tftpd64 adapter: getsockname() failed");
    }
    ::close(probe);
    return ::ntohs(address.sin_port);
}

bool port_is_free(std::uint16_t port) {
    const int probe = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (probe < 0) return false;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    address.sin_port = ::htons(port);
    const bool free_now =
        ::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof address) ==
        0;
    ::close(probe);
    return free_now;
}

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return predicate();
}

std::vector<int> find_processes_with_marker(const std::string& marker) {
    std::vector<int> pids;
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) return pids;

    while (const dirent* entry = ::readdir(proc)) {
        const int pid = std::atoi(entry->d_name);
        if (pid <= 0) continue;

        std::ifstream environment("/proc/" + std::string(entry->d_name) +
                                  "/environ");
        if (!environment) continue;
        const std::string contents{std::istreambuf_iterator<char>(environment),
                                   std::istreambuf_iterator<char>()};
        if (contents.find(marker) != std::string::npos) pids.push_back(pid);
    }
    ::closedir(proc);
    return pids;
}

std::vector<int> find_processes_with_cmdline(const std::string& fragment) {
    std::vector<int> pids;
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) return pids;

    while (const dirent* entry = ::readdir(proc)) {
        const int pid = std::atoi(entry->d_name);
        if (pid <= 0) continue;

        std::ifstream cmdline("/proc/" + std::string(entry->d_name) + "/cmdline");
        if (!cmdline) continue;
        const std::string contents{std::istreambuf_iterator<char>(cmdline),
                                   std::istreambuf_iterator<char>()};
        if (contents.find(fragment) != std::string::npos) pids.push_back(pid);
    }
    ::closedir(proc);
    return pids;
}

int launch_tftpd64(const WineEnvironment& wine,
                   const std::string& ini_directory_windows) {
    const pid_t child = ::fork();
    if (child < 0) return -1;

    if (child == 0) {
        // Detach into its own session so a stray Wine process can never outlive
        // the run attached to our terminal, and silence the GUI's output.
        ::setsid();
        const int null_fd = ::open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            ::dup2(null_fd, 1);
            ::dup2(null_fd, 2);
            if (null_fd > 2) ::close(null_fd);
        }
        ::setenv("WINEPREFIX", wine.wine_prefix.c_str(), 1);
        ::setenv("TFTPD32_INI_DIR", ini_directory_windows.c_str(), 1);
        ::setenv("WINEDEBUG", "-all", 1);
        ::setenv("WINEDLLOVERRIDES", "mscoree,mshtml=", 1);

        ::execl(wine.wine_binary.c_str(), wine.wine_binary.c_str(),
                wine.executable.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return child;
}

void kill_tftpd64(int launcher_pid, const std::string& marker) {
    if (!marker.empty()) {
        for (const int pid : find_processes_with_marker(marker)) {
            ::kill(pid, SIGKILL);
        }
    }
    if (launcher_pid > 0) {
        ::kill(launcher_pid, SIGKILL);
        int status = 0;
        ::waitpid(launcher_pid, &status, 0);
    }
}

} // namespace tftp_test_harness::tftpd64
