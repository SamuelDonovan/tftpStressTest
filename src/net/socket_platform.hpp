#ifndef TFTP_TEST_HARNESS_NET_SOCKET_PLATFORM_HPP
#define TFTP_TEST_HARNESS_NET_SOCKET_PLATFORM_HPP

// ---------------------------------------------------------------------------
// The one thin platform shim. C++ has no standard networking, so the harness
// uses the platform's native sockets: BSD sockets on POSIX, Winsock2 on
// Windows. Every other file in the tree includes this header instead of any
// OS-specific socket header, keeping the "no third-party dependency" property
// while remaining portable to constrained/air-gapped toolchains.
//
// Only the small surface the harness needs is exposed here.
// ---------------------------------------------------------------------------

#include <cstdint>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <netdb.h>
#endif

namespace tftp_test_harness::net {

#if defined(_WIN32)
using native_socket_type = SOCKET;
inline constexpr native_socket_type invalid_native_socket = INVALID_SOCKET;
#else
using native_socket_type = int;
inline constexpr native_socket_type invalid_native_socket = -1;
#endif

// Process-wide one-time initialization. On Windows this performs WSAStartup;
// on POSIX it is a no-op. Safe to call repeatedly.
void initialize_socket_subsystem();

// Close a native socket handle using the platform's close primitive.
void close_native_socket(native_socket_type handle);

// Retrieve the last socket error code in a platform-neutral way.
int last_socket_error();

// Human-readable description of a socket error code.
const char* describe_socket_error(int error_code);

// True when the last error was a non-fatal "would block" / "try again".
bool socket_error_is_would_block(int error_code);

} // namespace tftp_test_harness::net

#endif // TFTP_TEST_HARNESS_NET_SOCKET_PLATFORM_HPP
