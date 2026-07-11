#include "net/endpoint.hpp"

#include <cstring>

namespace tftp_test_harness::net {

bool Endpoint::from_host_and_port(const std::string& host,
                                  std::uint16_t port,
                                  Endpoint& out) {
    out = Endpoint{};

    // Try IPv4 first, then IPv6. inet_pton parses numeric addresses only, which
    // is exactly what we want: the harness never performs DNS resolution.
    sockaddr_in ipv4{};
    if (::inet_pton(AF_INET, host.c_str(), &ipv4.sin_addr) == 1) {
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons(port);
        std::memcpy(&out.storage_, &ipv4, sizeof(ipv4));
        out.length_ = sizeof(ipv4);
        return true;
    }

    sockaddr_in6 ipv6{};
    if (::inet_pton(AF_INET6, host.c_str(), &ipv6.sin6_addr) == 1) {
        ipv6.sin6_family = AF_INET6;
        ipv6.sin6_port = htons(port);
        std::memcpy(&out.storage_, &ipv6, sizeof(ipv6));
        out.length_ = sizeof(ipv6);
        return true;
    }
    return false;
}

Endpoint Endpoint::from_sockaddr(const sockaddr* address, socklen_t length) {
    Endpoint result;
    if (address != nullptr && length > 0 &&
        static_cast<std::size_t>(length) <= sizeof(result.storage_)) {
        std::memcpy(&result.storage_, address, length);
        result.length_ = length;
    }
    return result;
}

std::uint16_t Endpoint::port() const {
    if (family() == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&storage_);
        return ntohs(ipv4->sin_port);
    }
    if (family() == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&storage_);
        return ntohs(ipv6->sin6_port);
    }
    return 0;
}

std::string Endpoint::host() const {
    char text[INET6_ADDRSTRLEN] = {0};
    if (family() == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&storage_);
        ::inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text));
    } else if (family() == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&storage_);
        ::inet_ntop(AF_INET6, &ipv6->sin6_addr, text, sizeof(text));
    }
    return std::string(text);
}

std::string Endpoint::to_string() const {
    if (!valid()) {
        return "<unbound>";
    }
    if (is_ipv6()) {
        return "[" + host() + "]:" + std::to_string(port());
    }
    return host() + ":" + std::to_string(port());
}

bool Endpoint::operator==(const Endpoint& other) const {
    if (length_ != other.length_) {
        return false;
    }
    if (family() != other.family()) {
        return false;
    }
    if (family() == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(&storage_);
        const auto* b = reinterpret_cast<const sockaddr_in*>(&other.storage_);
        return a->sin_port == b->sin_port &&
               std::memcmp(&a->sin_addr, &b->sin_addr, sizeof(a->sin_addr)) == 0;
    }
    if (family() == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(&storage_);
        const auto* b = reinterpret_cast<const sockaddr_in6*>(&other.storage_);
        return a->sin6_port == b->sin6_port &&
               std::memcmp(&a->sin6_addr, &b->sin6_addr,
                           sizeof(a->sin6_addr)) == 0;
    }
    return false;
}

} // namespace tftp_test_harness::net
