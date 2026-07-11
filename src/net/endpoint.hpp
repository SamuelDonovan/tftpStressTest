#ifndef TFTP_TEST_HARNESS_NET_ENDPOINT_HPP
#define TFTP_TEST_HARNESS_NET_ENDPOINT_HPP

#include "net/socket_platform.hpp"

#include <cstdint>
#include <string>

namespace tftp_test_harness::net {

// An IPv4 or IPv6 loopback endpoint (address + port). The harness only ever
// binds and talks to the local host, so this is deliberately a thin value type
// over sockaddr_storage rather than a general-purpose address library.
//
// In TFTP terms the port half of an endpoint is the peer's TID (Transfer
// Identifier, RFC 1350 section 4): a transfer is identified by the pair of
// UDP ports the two ends chose. Comparing endpoints is therefore how the
// observer enforces TID discipline (A-20/A-21/A-22).
class Endpoint {
public:
    Endpoint() = default;

    // Construct from a numeric host string ("127.0.0.1" / "::1") and a port.
    // Returns whether parsing succeeded; on failure the endpoint is left empty.
    static bool from_host_and_port(const std::string& host,
                                   std::uint16_t port,
                                   Endpoint& out);

    // Wrap a raw sockaddr as delivered by recvfrom / getsockname.
    static Endpoint from_sockaddr(const sockaddr* address,
                                  socklen_t length);

    const sockaddr* sockaddr_pointer() const {
        return reinterpret_cast<const sockaddr*>(&storage_);
    }
    socklen_t sockaddr_length() const { return length_; }
    bool is_ipv6() const { return family() == AF_INET6; }
    int family() const {
        return reinterpret_cast<const sockaddr*>(&storage_)->sa_family;
    }

    std::uint16_t port() const;
    std::string host() const;                 // numeric host string
    std::string to_string() const;            // "host:port"

    bool valid() const { return length_ != 0; }

    // Two endpoints are equal when address family, address, and port match.
    // This is exactly the TID equality the observer relies on.
    bool operator==(const Endpoint& other) const;
    bool operator!=(const Endpoint& other) const { return !(*this == other); }

private:
    sockaddr_storage storage_{};
    socklen_t length_{0};
};

} // namespace tftp_test_harness::net

#endif // TFTP_TEST_HARNESS_NET_ENDPOINT_HPP
