#include "net/socket_platform.hpp"

#include <cstring>
#include <mutex>

namespace tftp_test_harness::net {

void initialize_socket_subsystem() {
#if defined(_WIN32)
    static std::once_flag started;
    std::call_once(started, [] {
        WSADATA winsock_data;
        // Winsock 2.2. Any failure here is fatal for the whole harness; the
        // process cannot open a socket without it.
        WSAStartup(MAKEWORD(2, 2), &winsock_data);
    });
#else
    // POSIX needs no per-process socket initialization.
#endif
}

void close_native_socket(native_socket_type handle) {
    if (handle == invalid_native_socket) {
        return;
    }
#if defined(_WIN32)
    ::closesocket(handle);
#else
    ::close(handle);
#endif
}

int last_socket_error() {
#if defined(_WIN32)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

const char* describe_socket_error(int error_code) {
#if defined(_WIN32)
    // std::strerror is not meaningful for Winsock codes; keep it simple and
    // stable across the report rather than pulling in FormatMessage.
    static thread_local char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "winsock error %d", error_code);
    return buffer;
#else
    return std::strerror(error_code);
#endif
}

bool socket_error_is_would_block(int error_code) {
#if defined(_WIN32)
    return error_code == WSAEWOULDBLOCK;
#else
    return error_code == EWOULDBLOCK || error_code == EAGAIN;
#endif
}

} // namespace tftp_test_harness::net
