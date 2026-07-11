#ifndef TFTP_TEST_HARNESS_NET_UDP_SOCKET_HPP
#define TFTP_TEST_HARNESS_NET_UDP_SOCKET_HPP

#include "net/endpoint.hpp"
#include "net/socket_platform.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace tftp_test_harness::net {

// A move-only RAII wrapper over a single unconnected UDP socket. Every UDP
// endpoint in the harness — the impairment proxy's listen and relay sockets,
// the reference client and server, the injection socket — is one of these.
//
// All operations are on the local host. Datagrams are the only transport unit;
// TFTP is a datagram protocol (RFC 1350) so there is deliberately no stream
// abstraction here.
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Open an unbound UDP socket in the given family (AF_INET / AF_INET6).
    bool open(int address_family = AF_INET);

    // Bind to host:port. Port 0 asks the kernel for an ephemeral port; use
    // local_endpoint() afterward to learn which one was assigned (the TID).
    bool bind(const std::string& host, std::uint16_t port);

    // The address this socket is actually bound to, including a kernel-assigned
    // ephemeral port. This is how the harness discovers a server's chosen TID.
    Endpoint local_endpoint() const;

    // Send a whole datagram to a specific peer. Returns bytes sent or -1.
    long send_to(const void* data, std::size_t length, const Endpoint& peer);
    long send_to(const std::vector<std::uint8_t>& datagram,
                 const Endpoint& peer) {
        return send_to(datagram.data(), datagram.size(), peer);
    }

    // Receive one datagram, recording the sender endpoint. Blocks up to the
    // configured receive timeout (see set_receive_timeout). Returns:
    //   > 0  : datagram length (payload copied into buffer, resized to fit)
    //   0    : timed out with no datagram (buffer left empty)
    //   -1   : error
    long receive_from(std::vector<std::uint8_t>& buffer,
                      Endpoint& sender,
                      std::size_t maximum_length = 70000);

    // Apply a blocking receive timeout. A zero duration means block forever.
    bool set_receive_timeout(std::chrono::milliseconds timeout);

    bool is_open() const { return handle_ != invalid_native_socket; }
    native_socket_type native_handle() const { return handle_; }
    void close();

    const std::string& last_error() const { return last_error_; }

private:
    void record_error(const char* where);

    native_socket_type handle_{invalid_native_socket};
    int address_family_{AF_INET};
    std::string last_error_;
};

} // namespace tftp_test_harness::net

#endif // TFTP_TEST_HARNESS_NET_UDP_SOCKET_HPP
