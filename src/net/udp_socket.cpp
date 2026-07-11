#include "net/udp_socket.hpp"

#include <cstring>
#include <utility>

namespace tftp_test_harness::net {

UdpSocket::~UdpSocket() { close(); }

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : handle_(other.handle_),
      address_family_(other.address_family_),
      last_error_(std::move(other.last_error_)) {
    other.handle_ = invalid_native_socket;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        address_family_ = other.address_family_;
        last_error_ = std::move(other.last_error_);
        other.handle_ = invalid_native_socket;
    }
    return *this;
}

void UdpSocket::record_error(const char* where) {
    const int code = last_socket_error();
    last_error_ = std::string(where) + ": " + describe_socket_error(code);
}

bool UdpSocket::open(int address_family) {
    initialize_socket_subsystem();
    close();
    address_family_ = address_family;
    handle_ = ::socket(address_family, SOCK_DGRAM, IPPROTO_UDP);
    if (handle_ == invalid_native_socket) {
        record_error("socket()");
        return false;
    }
    return true;
}

bool UdpSocket::bind(const std::string& host, std::uint16_t port) {
    Endpoint local;
    if (!Endpoint::from_host_and_port(host, port, local)) {
        last_error_ = "bind(): could not parse host '" + host + "'";
        return false;
    }
    if (!is_open() && !open(local.family())) {
        return false;
    }
    if (::bind(handle_, local.sockaddr_pointer(), local.sockaddr_length()) != 0) {
        record_error("bind()");
        return false;
    }
    return true;
}

Endpoint UdpSocket::local_endpoint() const {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(handle_, reinterpret_cast<sockaddr*>(&storage),
                      &length) != 0) {
        return Endpoint{};
    }
    return Endpoint::from_sockaddr(reinterpret_cast<sockaddr*>(&storage), length);
}

long UdpSocket::send_to(const void* data, std::size_t length,
                        const Endpoint& peer) {
    if (!is_open()) {
        last_error_ = "send_to(): socket not open";
        return -1;
    }
#if defined(_WIN32)
    const int payload_length = static_cast<int>(length);
#else
    const std::size_t payload_length = length;
#endif
    const long sent = static_cast<long>(::sendto(
        handle_, reinterpret_cast<const char*>(data), payload_length, 0,
        peer.sockaddr_pointer(), peer.sockaddr_length()));
    if (sent < 0) {
        record_error("sendto()");
    }
    return sent;
}

long UdpSocket::receive_from(std::vector<std::uint8_t>& buffer,
                             Endpoint& sender, std::size_t maximum_length) {
    if (!is_open()) {
        last_error_ = "receive_from(): socket not open";
        return -1;
    }
    buffer.assign(maximum_length, 0);
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
#if defined(_WIN32)
    const int receive_capacity = static_cast<int>(maximum_length);
#else
    const std::size_t receive_capacity = maximum_length;
#endif
    const long received = static_cast<long>(::recvfrom(
        handle_, reinterpret_cast<char*>(buffer.data()), receive_capacity, 0,
        reinterpret_cast<sockaddr*>(&storage), &length));
    if (received < 0) {
        const int code = last_socket_error();
        buffer.clear();
        if (socket_error_is_would_block(code)) {
            return 0; // treated as a timeout, not an error
        }
        record_error("recvfrom()");
        return -1;
    }
    buffer.resize(static_cast<std::size_t>(received));
    sender = Endpoint::from_sockaddr(reinterpret_cast<sockaddr*>(&storage),
                                     length);
    return received;
}

bool UdpSocket::set_receive_timeout(std::chrono::milliseconds timeout) {
    if (!is_open()) {
        last_error_ = "set_receive_timeout(): socket not open";
        return false;
    }
#if defined(_WIN32)
    DWORD milliseconds = static_cast<DWORD>(timeout.count());
    if (::setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&milliseconds),
                     sizeof(milliseconds)) != 0) {
        record_error("setsockopt(SO_RCVTIMEO)");
        return false;
    }
#else
    timeval value{};
    value.tv_sec = static_cast<long>(timeout.count() / 1000);
    value.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    if (::setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO, &value,
                     sizeof(value)) != 0) {
        record_error("setsockopt(SO_RCVTIMEO)");
        return false;
    }
#endif
    return true;
}

void UdpSocket::close() {
    if (handle_ != invalid_native_socket) {
        close_native_socket(handle_);
        handle_ = invalid_native_socket;
    }
}

} // namespace tftp_test_harness::net
