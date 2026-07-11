#ifndef TFTP_TEST_HARNESS_NET_IMPAIRMENT_PROXY_HPP
#define TFTP_TEST_HARNESS_NET_IMPAIRMENT_PROXY_HPP

// ---------------------------------------------------------------------------
// The userspace UDP impairment proxy — the heart of the harness.
//
// It binds a UDP socket on the local host (127.0.0.1) and interposes itself
// between a client-under-test and a server-under-test. It plays two roles at
// once:
//   * fault injector — applies the F-series impairment pipeline and scripted
//     G-series / TID injection to the traffic it relays;
//   * observer — records a timestamped trace of every datagram in both
//     directions for the packet observer / oracle.
//
// TID model (RFC 1350 section 4). The client sends its request to the proxy's
// listen port, which stands in for the server's well-known port. The proxy
// relays that request to the real server from a per-session server-facing
// socket; the server replies from a freshly chosen ephemeral TID, which the
// proxy observes (this is how A-20 — "server selects a fresh TID" — is judged).
// The proxy relays server replies back to the client from its listen port, so
// from the client's point of view the proxy port is the server TID for the rest
// of the transfer. All impairment and injection happen on this loopback path
// only; nothing is directed at any external host.
// ---------------------------------------------------------------------------

#include "net/endpoint.hpp"
#include "net/impairments.hpp"
#include "net/packet_trace.hpp"
#include "net/udp_socket.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tftp_test_harness::net {

// A datagram the proxy synthesizes and injects, used to script the A-21/A-22
// and G-series interposition tests.
struct InjectedPacket {
    std::vector<std::uint8_t> bytes;
    bool toward_client = true; // deliver to the client (true) or the server
    std::chrono::milliseconds delay{0};
    // When true the datagram is sent from a fresh ephemeral port so the peer
    // sees an unexpected TID (RFC 1350 section 4) — the stray-source case that
    // A-21/A-22/G-06 require. When false it is sent from the proxy's normal
    // relay TID (used to, e.g., inject a duplicate ACK the server will accept).
    bool from_stray_tid = true;
    std::string description;
};

// Scripted injection hook. Invoked for each datagram the proxy observes; may
// return zero or more packets to inject in response. Deterministic scripts keep
// reproduction exact.
using InjectionScript =
    std::function<std::vector<InjectedPacket>(const TraceRecord&)>;

class ImpairmentProxy {
public:
    struct Config {
        std::string listen_host = "127.0.0.1";
        std::uint16_t listen_port = 0; // 0 => ephemeral
        Endpoint server_endpoint;      // the real server behind the proxy
        ImpairmentConfig impairment;
        std::uint64_t seed = 0;
        InjectionScript injection; // optional
    };

    ImpairmentProxy() = default;
    ~ImpairmentProxy();

    ImpairmentProxy(const ImpairmentProxy&) = delete;
    ImpairmentProxy& operator=(const ImpairmentProxy&) = delete;

    // Bind and start the relay thread. Returns false if binding failed.
    bool start(const Config& config);

    // The endpoint clients must send their request to (the proxy's listen port).
    Endpoint listen_endpoint() const { return listen_endpoint_; }

    // Stop the relay thread and return the recorded trace. Safe to call once.
    PacketTrace stop_and_take_trace();

    bool is_running() const { return running_.load(); }

private:
    // One client<->server transfer flowing through the proxy.
    struct Session {
        Endpoint client_endpoint;             // the client's TID
        std::unique_ptr<UdpSocket> server_socket; // proxy's server-facing TID
        Endpoint server_destination;          // where to send toward the server
        bool server_tid_locked = false;
        std::chrono::steady_clock::time_point started;
    };

    // A datagram scheduled to be sent at a deadline (delay/reorder/throttle).
    struct ScheduledSend {
        std::chrono::steady_clock::time_point deadline;
        UdpSocket* socket = nullptr;
        Endpoint destination;
        std::vector<std::uint8_t> bytes;
    };

    void relay_loop();
    Session* find_or_create_session(const Endpoint& client_endpoint);
    void handle_client_datagram(Session& session,
                                const std::vector<std::uint8_t>& datagram);
    void handle_server_datagram(Session& session,
                                const std::vector<std::uint8_t>& datagram);
    void observe_and_relay(Session& session, Direction direction,
                           const std::vector<std::uint8_t>& datagram,
                           UdpSocket& outbound, const Endpoint& destination);
    void run_injection(const TraceRecord& observed, Session& session);
    void flush_due_sends(std::chrono::steady_clock::time_point now);
    double relative_time_ms(const Session& session) const;
    UdpSocket& stray_socket_for(Session& session);

    Config config_;
    UdpSocket listen_socket_;
    Endpoint listen_endpoint_;
    std::unique_ptr<ImpairmentPipeline> pipeline_;
    std::vector<std::unique_ptr<Session>> sessions_;
    std::vector<std::unique_ptr<UdpSocket>> stray_sockets_;
    std::vector<ScheduledSend> scheduled_;
    std::chrono::steady_clock::time_point epoch_;

    std::thread relay_thread_;
    std::atomic<bool> running_{false};
    std::mutex trace_mutex_;
    PacketTrace trace_;
};

} // namespace tftp_test_harness::net

#endif // TFTP_TEST_HARNESS_NET_IMPAIRMENT_PROXY_HPP
