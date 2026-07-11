#include "net/impairment_proxy.hpp"

#include <algorithm>

namespace tftp_test_harness::net {

const char* trace_disposition_name(TraceDisposition disposition) {
    switch (disposition) {
        case TraceDisposition::Delivered: return "delivered";
        case TraceDisposition::Dropped: return "dropped";
        case TraceDisposition::Corrupted: return "corrupted";
        case TraceDisposition::Injected: return "injected";
    }
    return "unknown";
}

ImpairmentProxy::~ImpairmentProxy() {
    if (running_.load()) {
        stop_and_take_trace();
    }
}

bool ImpairmentProxy::start(const Config& config) {
    config_ = config;
    if (!listen_socket_.bind(config.listen_host, config.listen_port)) {
        return false;
    }
    // A short receive timeout guards against a spurious select() wakeup blocking
    // the loop; scheduling is driven by the select() timeout itself.
    listen_socket_.set_receive_timeout(std::chrono::milliseconds(100));
    listen_endpoint_ = listen_socket_.local_endpoint();
    pipeline_ = std::make_unique<ImpairmentPipeline>(config.impairment,
                                                     config.seed);
    trace_.seed = config.seed;
    epoch_ = std::chrono::steady_clock::now();
    running_.store(true);
    relay_thread_ = std::thread([this] { relay_loop(); });
    return true;
}

PacketTrace ImpairmentProxy::stop_and_take_trace() {
    running_.store(false);
    if (relay_thread_.joinable()) {
        relay_thread_.join();
    }
    listen_socket_.close();
    std::lock_guard<std::mutex> lock(trace_mutex_);
    return trace_;
}

double ImpairmentProxy::relative_time_ms(const Session& session) const {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now - session.started)
        .count();
}

ImpairmentProxy::Session* ImpairmentProxy::find_or_create_session(
    const Endpoint& client_endpoint) {
    for (auto& session : sessions_) {
        if (session->client_endpoint == client_endpoint) {
            return session.get();
        }
    }
    auto session = std::make_unique<Session>();
    session->client_endpoint = client_endpoint;
    session->server_socket = std::make_unique<UdpSocket>();
    const std::string bind_host =
        config_.server_endpoint.is_ipv6() ? "::" : "0.0.0.0";
    if (!session->server_socket->open(config_.server_endpoint.family()) ||
        !session->server_socket->bind(bind_host, 0)) {
        return nullptr;
    }
    session->server_socket->set_receive_timeout(std::chrono::milliseconds(100));
    session->server_destination = config_.server_endpoint; // server well-known
    session->started = std::chrono::steady_clock::now();
    Session* raw = session.get();
    sessions_.push_back(std::move(session));
    return raw;
}

UdpSocket& ImpairmentProxy::stray_socket_for(Session& /*session*/) {
    // A dedicated ephemeral socket whose port is, by construction, an unexpected
    // TID from the peer's point of view (RFC 1350 section 4). Shared across
    // injections; its inbound datagrams (e.g. the peer's ERROR 5 reply) are
    // drained and discarded by the relay loop.
    auto socket = std::make_unique<UdpSocket>();
    const std::string bind_host =
        config_.server_endpoint.is_ipv6() ? "::" : "0.0.0.0";
    socket->open(config_.server_endpoint.family());
    socket->bind(bind_host, 0);
    socket->set_receive_timeout(std::chrono::milliseconds(10));
    UdpSocket& reference = *socket;
    stray_sockets_.push_back(std::move(socket));
    return reference;
}

void ImpairmentProxy::flush_due_sends(
    std::chrono::steady_clock::time_point now) {
    std::vector<ScheduledSend> remaining;
    remaining.reserve(scheduled_.size());
    for (auto& send : scheduled_) {
        if (send.deadline <= now) {
            if (send.socket != nullptr) {
                send.socket->send_to(send.bytes, send.destination);
            }
        } else {
            remaining.push_back(std::move(send));
        }
    }
    scheduled_.swap(remaining);
}

void ImpairmentProxy::observe_and_relay(
    Session& session, Direction direction,
    const std::vector<std::uint8_t>& datagram, UdpSocket& outbound,
    const Endpoint& destination) {
    const auto now = std::chrono::steady_clock::now();

    TraceRecord record;
    record.relative_time_ms = relative_time_ms(session);
    record.direction = direction;
    if (direction == Direction::ClientToServer) {
        record.source_tid = session.client_endpoint.port();
        record.destination_tid = session.server_destination.port();
    } else {
        record.source_tid = session.server_destination.port();
        record.destination_tid = session.client_endpoint.port();
    }
    record.datagram_length = datagram.size();
    auto parsed = parse_tftp_packet(datagram);
    record.parseable = parsed.ok;
    if (parsed.ok) {
        record.packet = parsed.packet;
    } else {
        record.parse_failure = parsed.failure_reason;
    }

    const bool is_data = record.is_data();
    PipelineResult result = pipeline_->process(direction, datagram, is_data, now);

    if (result.dropped) {
        record.disposition = TraceDisposition::Dropped;
        record.note = result.drop_reason;
    } else {
        record.was_duplicated = result.deliveries.size() > 1;
        bool any_corrupted = false;
        for (auto& delivery : result.deliveries) {
            ScheduledSend send;
            send.deadline = now + delivery.delay;
            send.socket = &outbound;
            send.destination = destination;
            send.bytes = delivery.bytes;
            scheduled_.push_back(std::move(send));
            any_corrupted = any_corrupted || delivery.corrupted;
        }
        record.disposition = any_corrupted ? TraceDisposition::Corrupted
                                           : TraceDisposition::Delivered;
        if (any_corrupted) {
            record.note = "corrupted payload";
        }
    }

    {
        std::lock_guard<std::mutex> lock(trace_mutex_);
        trace_.records.push_back(record);
    }

    run_injection(record, session);
}

void ImpairmentProxy::run_injection(const TraceRecord& observed,
                                    Session& session) {
    if (!config_.injection) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::vector<InjectedPacket> injections = config_.injection(observed);
    for (auto& injected : injections) {
        UdpSocket* outbound = nullptr;
        Endpoint destination;
        if (injected.toward_client) {
            destination = session.client_endpoint;
            outbound = injected.from_stray_tid ? &stray_socket_for(session)
                                               : &listen_socket_;
        } else {
            destination = session.server_destination;
            outbound = injected.from_stray_tid
                           ? &stray_socket_for(session)
                           : session.server_socket.get();
        }

        ScheduledSend send;
        send.deadline = now + injected.delay;
        send.socket = outbound;
        send.destination = destination;
        send.bytes = injected.bytes;

        TraceRecord record;
        record.relative_time_ms = relative_time_ms(session);
        record.direction = injected.toward_client ? Direction::ServerToClient
                                                   : Direction::ClientToServer;
        record.source_tid = outbound->local_endpoint().port();
        record.destination_tid = destination.port();
        record.datagram_length = injected.bytes.size();
        auto parsed = parse_tftp_packet(injected.bytes);
        record.parseable = parsed.ok;
        if (parsed.ok) {
            record.packet = parsed.packet;
        }
        record.disposition = TraceDisposition::Injected;
        record.note = injected.description.empty() ? "injected packet"
                                                    : injected.description;

        scheduled_.push_back(std::move(send));
        {
            std::lock_guard<std::mutex> lock(trace_mutex_);
            trace_.records.push_back(record);
        }
    }
}

void ImpairmentProxy::handle_client_datagram(
    Session& session, const std::vector<std::uint8_t>& datagram) {
    observe_and_relay(session, Direction::ClientToServer, datagram,
                      *session.server_socket, session.server_destination);
}

void ImpairmentProxy::handle_server_datagram(
    Session& session, const std::vector<std::uint8_t>& datagram) {
    observe_and_relay(session, Direction::ServerToClient, datagram,
                      listen_socket_, session.client_endpoint);
}

void ImpairmentProxy::relay_loop() {
    std::vector<std::uint8_t> buffer;
    Endpoint sender;

    while (running_.load()) {
        const auto now = std::chrono::steady_clock::now();
        flush_due_sends(now);

        // Compute the select() timeout: the soonest of a default poll interval
        // and the next scheduled send deadline.
        std::chrono::steady_clock::duration wait =
            std::chrono::milliseconds(50);
        for (const auto& send : scheduled_) {
            const auto until = send.deadline - now;
            if (until < wait) {
                wait = std::max<std::chrono::steady_clock::duration>(
                    until, std::chrono::steady_clock::duration::zero());
            }
        }

        fd_set read_set;
        FD_ZERO(&read_set);
        native_socket_type max_handle = listen_socket_.native_handle();
        FD_SET(listen_socket_.native_handle(), &read_set);
        for (auto& session : sessions_) {
            const auto handle = session->server_socket->native_handle();
            FD_SET(handle, &read_set);
            max_handle = std::max(max_handle, handle);
        }
        for (auto& stray : stray_sockets_) {
            const auto handle = stray->native_handle();
            FD_SET(handle, &read_set);
            max_handle = std::max(max_handle, handle);
        }

        timeval timeout;
        const auto wait_us =
            std::chrono::duration_cast<std::chrono::microseconds>(wait).count();
        timeout.tv_sec = static_cast<long>(wait_us / 1000000);
        timeout.tv_usec = static_cast<long>(wait_us % 1000000);

#if defined(_WIN32)
        const int ready = ::select(0, &read_set, nullptr, nullptr, &timeout);
#else
        const int ready = ::select(static_cast<int>(max_handle) + 1, &read_set,
                                   nullptr, nullptr, &timeout);
#endif
        if (ready <= 0) {
            continue; // timeout or interrupted; loop to flush sends
        }

        if (FD_ISSET(listen_socket_.native_handle(), &read_set)) {
            const long received = listen_socket_.receive_from(buffer, sender);
            if (received > 0) {
                Session* session = find_or_create_session(sender);
                if (session != nullptr) {
                    handle_client_datagram(*session, buffer);
                }
            }
        }

        // Iterate by index because a session may have been added this pass.
        for (std::size_t i = 0; i < sessions_.size(); ++i) {
            Session& session = *sessions_[i];
            if (!FD_ISSET(session.server_socket->native_handle(), &read_set)) {
                continue;
            }
            const long received =
                session.server_socket->receive_from(buffer, sender);
            if (received <= 0) {
                continue;
            }
            // Lock onto the server's chosen TID on the first reply (A-20).
            if (!session.server_tid_locked) {
                session.server_destination = sender;
                session.server_tid_locked = true;
            }
            handle_server_datagram(session, buffer);
        }

        for (auto& stray : stray_sockets_) {
            if (FD_ISSET(stray->native_handle(), &read_set)) {
                // An inbound datagram on a stray socket is the victim's reply to
                // an injected stray-TID packet — typically ERROR code 5 (unknown
                // transfer ID). It is not relayed, but it IS recorded so the
                // observer can confirm the peer rebuffed the stray TID rather
                // than aborting the legitimate transfer (A-21/A-22/G-06).
                const long received = stray->receive_from(buffer, sender);
                if (received > 0) {
                    TraceRecord record;
                    record.direction = Direction::ClientToServer;
                    record.source_tid = sender.port();
                    record.destination_tid = stray->local_endpoint().port();
                    record.datagram_length = buffer.size();
                    auto parsed = parse_tftp_packet(buffer);
                    record.parseable = parsed.ok;
                    if (parsed.ok) {
                        record.packet = parsed.packet;
                    }
                    record.disposition = TraceDisposition::Injected;
                    record.note = "reply to stray TID (dropped by proxy)";
                    std::lock_guard<std::mutex> lock(trace_mutex_);
                    trace_.records.push_back(record);
                }
            }
        }
    }

    // Final flush so any last scheduled relays go out before shutdown.
    flush_due_sends(std::chrono::steady_clock::now() +
                    std::chrono::hours(1));
}

} // namespace tftp_test_harness::net
