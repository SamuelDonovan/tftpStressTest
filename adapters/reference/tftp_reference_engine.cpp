#include "reference/tftp_reference_engine.hpp"

#include "net/netascii.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

namespace tftp_test_harness::reference {

using namespace tftp_test_harness::net;

// ===========================================================================
// File helpers
// ===========================================================================
std::optional<std::vector<std::uint8_t>> read_entire_file(
    const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
    return bytes;
}

bool write_entire_file(const std::filesystem::path& path,
                       const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(stream);
}

namespace {

constexpr std::uint32_t kMinimumBlockSize = 8;      // RFC 2348
constexpr std::uint32_t kMaximumBlockSize = 65464;  // RFC 2348

bool mode_is_netascii(const std::string& mode) {
    return equals_ignore_ascii_case(mode, "netascii");
}

// Map an absolute (32-bit) block index to its 16-bit on-wire block number.
// Block numbers start at 1 and wrap through 0 at 65536 (natural uint16 overflow
// — RFC 1350 does not specify wraparound; this engine's documented choice, and
// exactly what A-40 measures).
std::uint16_t wire_block_number(std::uint32_t absolute_index) {
    return static_cast<std::uint16_t>(absolute_index & 0xFFFFu);
}

// Total number of DATA blocks for a payload, including the terminating
// zero-length block when the payload is an exact multiple of the block size
// (RFC 1350 section 2). Always at least 1 (the empty-file case, A-03).
std::uint32_t total_block_count(std::size_t payload_size,
                                std::uint32_t block_size) {
    return static_cast<std::uint32_t>(payload_size / block_size) + 1u;
}

// Bytes carried by absolute block index (1-based). A block at or beyond the end
// of the payload is the terminating zero-length block.
std::vector<std::uint8_t> block_payload(const std::vector<std::uint8_t>& payload,
                                        std::uint32_t absolute_index,
                                        std::uint32_t block_size) {
    const std::size_t start =
        static_cast<std::size_t>(absolute_index - 1) * block_size;
    if (start >= payload.size()) {
        return {};
    }
    const std::size_t length =
        std::min<std::size_t>(block_size, payload.size() - start);
    const auto offset = static_cast<std::ptrdiff_t>(start);
    const auto span = static_cast<std::ptrdiff_t>(length);
    return std::vector<std::uint8_t>(payload.begin() + offset,
                                     payload.begin() + offset + span);
}

// Resolve a 16-bit ACK block number to an absolute index in the set of blocks
// currently in flight (last_acked, highest_sent]. Returns last_acked when the
// ACK does not advance (a duplicate or stale ACK). Robust across the 16-bit
// wraparound because it matches the low 16 bits of unambiguous in-flight
// absolute indices (a window is far smaller than 65536).
std::uint32_t resolve_ack_to_absolute(std::uint16_t wire_ack,
                                      std::uint32_t last_acked,
                                      std::uint32_t highest_sent) {
    for (std::uint32_t candidate = highest_sent; candidate > last_acked;
         --candidate) {
        if (wire_block_number(candidate) == wire_ack) {
            return candidate;
        }
    }
    return last_acked;
}

// Resolve a 16-bit DATA block number to an absolute index near the receiver's
// current position. Searches forward through a window then backward for
// duplicates. Returns 0 when it cannot be placed (unrelated block; dropped).
std::uint32_t resolve_data_to_absolute(std::uint16_t wire_block,
                                       std::uint32_t last_in_sequence,
                                       std::uint32_t window) {
    const std::uint32_t forward_span = std::max<std::uint32_t>(window, 1u);
    for (std::uint32_t offset = 1; offset <= forward_span; ++offset) {
        const std::uint32_t candidate = last_in_sequence + offset;
        if (wire_block_number(candidate) == wire_block) {
            return candidate;
        }
    }
    const std::uint32_t backward_span =
        std::min<std::uint32_t>(last_in_sequence, forward_span);
    for (std::uint32_t offset = 0; offset <= backward_span; ++offset) {
        const std::uint32_t candidate = last_in_sequence - offset;
        if (wire_block_number(candidate) == wire_block) {
            return candidate;
        }
    }
    return 0;
}

enum class ReceiveStatus { GotPacket, TimedOut, SocketError };

// Receive the next packet that legitimately belongs to this transfer: a
// datagram from the locked peer TID. A datagram from any other source is
// answered with ERROR code 5 (unknown transfer ID) and ignored, and reception
// continues — this is precisely the RFC 1350 section 4 TID discipline that
// A-21/A-22/G-06 require: the stray sender is rebuffed but the legitimate
// transfer is NOT aborted. Malformed datagrams from the peer are dropped and
// reception continues (G-series robustness).
ReceiveStatus receive_from_locked_peer(UdpSocket& socket,
                                       const Endpoint& locked_peer,
                                       TftpPacket& out_packet) {
    std::vector<std::uint8_t> buffer;
    Endpoint sender;
    for (;;) {
        const long received = socket.receive_from(buffer, sender);
        if (received == 0) {
            return ReceiveStatus::TimedOut;
        }
        if (received < 0) {
            return ReceiveStatus::SocketError;
        }
        if (sender != locked_peer) {
            // RFC 1350 section 4: reply ERROR 5 to the stray TID, do not abort.
            const auto error = serialize_error(
                TftpErrorCode::UnknownTransferId, "unknown transfer ID");
            socket.send_to(error, sender);
            continue;
        }
        auto parsed = parse_tftp_packet(buffer);
        if (!parsed.ok) {
            // Drop malformed packet from the peer; keep waiting.
            continue;
        }
        out_packet = std::move(parsed.packet);
        return ReceiveStatus::GotPacket;
    }
}

// ---------------------------------------------------------------------------
// Sender data plane (RFC 7440 windowed; window == 1 reduces to RFC 1350
// lock-step). Retransmits only on timeout (rollback to the last ACK), never in
// response to a duplicate ACK — unless the SAS quirk is enabled.
// ---------------------------------------------------------------------------
TransferOutcome run_data_sender(UdpSocket& socket, const Endpoint& peer,
                                const std::vector<std::uint8_t>& payload,
                                const NegotiatedParameters& params,
                                const EngineConfig& config,
                                std::uint32_t starting_data_packets_sent) {
    TransferOutcome outcome;
    outcome.negotiated = params;
    outcome.data_packets_sent = starting_data_packets_sent;

    const std::uint32_t block_size = params.block_size;
    const std::uint32_t window = std::max<std::uint16_t>(params.window_size, 1);

    std::uint32_t total_blocks = total_block_count(payload.size(), block_size);
    // Broken final-block rule (RFC 1350 section 2): omit the terminating
    // zero-length block for an exact-multiple, non-empty payload.
    if (config.quirks.omit_terminating_empty_block &&
        !payload.empty() && payload.size() % block_size == 0) {
        total_blocks -= 1; // never send the terminating empty block
    }

    std::uint32_t last_acked = 0;
    std::uint32_t highest_sent = 0;
    int consecutive_timeouts = 0;

    enum class Action { SendNew, Rollback, WaitOnly };
    Action action = Action::SendNew;

    while (last_acked < total_blocks) {
        if (action == Action::SendNew) {
            const std::uint32_t window_edge =
                std::min<std::uint32_t>(last_acked + window, total_blocks);
            for (std::uint32_t index = highest_sent + 1; index <= window_edge;
                 ++index) {
                const auto data = block_payload(payload, index, block_size);
                socket.send_to(serialize_data(wire_block_number(index), data),
                               peer);
                ++outcome.data_packets_sent;
            }
            highest_sent = std::max(highest_sent, window_edge);
        } else if (action == Action::Rollback) {
            for (std::uint32_t index = last_acked + 1; index <= highest_sent;
                 ++index) {
                const auto data = block_payload(payload, index, block_size);
                socket.send_to(serialize_data(wire_block_number(index), data),
                               peer);
                ++outcome.retransmissions;
            }
        }
        // Action::WaitOnly sends nothing and simply waits for the next packet.

        TftpPacket packet;
        const ReceiveStatus status =
            receive_from_locked_peer(socket, peer, packet);

        if (status == ReceiveStatus::SocketError) {
            outcome.error_message = "socket error while sending";
            return outcome;
        }
        if (status == ReceiveStatus::TimedOut) {
            if (++consecutive_timeouts > config.maximum_retransmissions) {
                outcome.timed_out = true;
                outcome.error_message =
                    "gave up after maximum retransmissions (sender)";
                return outcome;
            }
            action = Action::Rollback; // resend the outstanding window
            continue;
        }

        if (packet.opcode == TftpOpcode::Error) {
            outcome.error_code = packet.error_code;
            outcome.error_message = packet.error_message;
            return outcome;
        }
        if (packet.opcode != TftpOpcode::Acknowledgement) {
            action = Action::WaitOnly; // ignore unexpected opcode, keep waiting
            continue;
        }

        const std::uint32_t acked = resolve_ack_to_absolute(
            packet.block_number, last_acked, highest_sent);
        if (acked > last_acked) {
            last_acked = acked;
            consecutive_timeouts = 0;
            action = Action::SendNew;
        } else {
            // Duplicate / stale ACK. A correct sender ignores it (RFC 1123
            // section 4.2). The SAS quirk retransmits, creating the cascade.
            action = config.quirks.retransmit_on_duplicate_ack
                         ? Action::Rollback
                         : Action::WaitOnly;
        }
    }

    outcome.completed_successfully = true;
    outcome.bytes_transferred = payload.size();
    return outcome;
}

// ---------------------------------------------------------------------------
// Receiver data plane (RFC 7440 windowed). Writes received bytes into `sink`.
// A short/partial DATA block terminates the transfer (RFC 1350 section 2). On a
// gap the receiver ACKs the last in-sequence block, forcing the sender to roll
// back (RFC 7440 section 4). After the final ACK it dallies briefly, re-ACKing
// retransmitted final DATA so a lost final ACK does not hang the sender.
// ---------------------------------------------------------------------------
TransferOutcome run_data_receiver(UdpSocket& socket, const Endpoint& peer,
                                  const NegotiatedParameters& params,
                                  const EngineConfig& config,
                                  std::vector<std::uint8_t>& sink,
                                  std::uint32_t first_expected_block,
                                  const std::optional<TftpPacket>& primed) {
    TransferOutcome outcome;
    outcome.negotiated = params;

    const std::uint32_t block_size = params.block_size;
    const std::uint32_t window = std::max<std::uint16_t>(params.window_size, 1);

    std::uint32_t last_in_sequence = first_expected_block - 1;
    std::uint32_t blocks_since_ack = 0;
    int consecutive_timeouts = 0;

    std::optional<TftpPacket> pending = primed;

    for (;;) {
        TftpPacket packet;
        if (pending.has_value()) {
            packet = std::move(*pending);
            pending.reset();
        } else {
            const ReceiveStatus status =
                receive_from_locked_peer(socket, peer, packet);
            if (status == ReceiveStatus::SocketError) {
                outcome.error_message = "socket error while receiving";
                return outcome;
            }
            if (status == ReceiveStatus::TimedOut) {
                if (++consecutive_timeouts > config.maximum_retransmissions) {
                    outcome.timed_out = true;
                    outcome.error_message =
                        "gave up after maximum retransmissions (receiver)";
                    return outcome;
                }
                // Prod the sender by re-ACKing the last in-sequence block.
                socket.send_to(
                    serialize_acknowledgement(wire_block_number(last_in_sequence)),
                    peer);
                continue;
            }
        }

        if (packet.opcode == TftpOpcode::Error) {
            outcome.error_code = packet.error_code;
            outcome.error_message = packet.error_message;
            return outcome;
        }
        if (packet.opcode != TftpOpcode::Data) {
            continue; // ignore unexpected opcode
        }
        consecutive_timeouts = 0;

        const std::uint32_t absolute = resolve_data_to_absolute(
            packet.block_number, last_in_sequence, window);

        if (absolute == last_in_sequence + 1) {
            // In-sequence block: accept it.
            sink.insert(sink.end(), packet.data.begin(), packet.data.end());
            ++last_in_sequence;
            ++outcome.data_packets_received;
            ++blocks_since_ack;
            const bool final_block = packet.data.size() < block_size;
            if (final_block) {
                socket.send_to(serialize_acknowledgement(
                                   wire_block_number(last_in_sequence)),
                               peer);
                break; // transfer complete
            }
            if (blocks_since_ack >= window) {
                socket.send_to(serialize_acknowledgement(
                                   wire_block_number(last_in_sequence)),
                               peer);
                blocks_since_ack = 0;
            }
        } else if (absolute != 0 && absolute <= last_in_sequence) {
            // Duplicate of an already-accepted block: discard and re-ACK the
            // last in-sequence block (RFC 1350 duplicate handling).
            socket.send_to(
                serialize_acknowledgement(wire_block_number(last_in_sequence)),
                peer);
            blocks_since_ack = 0;
        } else {
            // Out-of-sequence (a gap). RFC 7440 section 4: ACK the last
            // in-sequence block and drop the out-of-order data, forcing the
            // sender to roll back. The quirk instead accepts it in the wrong
            // place, silently corrupting the stream.
            if (config.quirks.accept_out_of_sequence_data && absolute != 0) {
                sink.insert(sink.end(), packet.data.begin(), packet.data.end());
                last_in_sequence = absolute;
                ++outcome.data_packets_received;
                socket.send_to(serialize_acknowledgement(
                                   wire_block_number(last_in_sequence)),
                               peer);
                if (packet.data.size() < block_size) {
                    break;
                }
            } else {
                socket.send_to(serialize_acknowledgement(
                                   wire_block_number(last_in_sequence)),
                               peer);
                blocks_since_ack = 0;
            }
        }
    }

    // Dally: linger briefly to answer a retransmitted final DATA (in case our
    // final ACK was lost) so the peer does not hang. Bounded and best-effort.
    socket.set_receive_timeout(std::chrono::milliseconds(150));
    for (int dally = 0; dally < 4; ++dally) {
        TftpPacket packet;
        const ReceiveStatus status =
            receive_from_locked_peer(socket, peer, packet);
        if (status != ReceiveStatus::GotPacket) {
            break;
        }
        if (packet.opcode == TftpOpcode::Data) {
            socket.send_to(
                serialize_acknowledgement(wire_block_number(last_in_sequence)),
                peer);
        }
    }

    outcome.completed_successfully = true;
    outcome.bytes_transferred = sink.size();
    return outcome;
}

// ---------------------------------------------------------------------------
// Option negotiation shared by client and server.
// ---------------------------------------------------------------------------

// Clamp a requested block size into the RFC 2348 range [8, 65464].
std::uint32_t clamp_block_size(std::uint32_t requested) {
    return std::min(std::max(requested, kMinimumBlockSize), kMaximumBlockSize);
}

} // namespace

// ===========================================================================
// Client
// ===========================================================================
namespace {

// Build the option list a client requests, honoring which options the engine
// config claims to support.
std::vector<TftpOption> build_requested_option_list(
    const ClientRequestedOptions& options, const EngineConfig& config) {
    std::vector<TftpOption> list;
    if (!config.support_option_negotiation) {
        return list;
    }
    if (options.block_size && config.support_block_size_option) {
        list.push_back({"blksize", std::to_string(*options.block_size)});
    }
    if (options.timeout_seconds && config.support_timeout_option) {
        list.push_back({"timeout", std::to_string(*options.timeout_seconds)});
    }
    if (options.transfer_size && config.support_transfer_size_option) {
        list.push_back({"tsize", std::to_string(*options.transfer_size)});
    }
    if (options.window_size && config.support_window_size_option) {
        list.push_back({"windowsize", std::to_string(*options.window_size)});
    }
    return list;
}

// Apply an OACK from the server to the client's negotiated parameters. Returns
// false (with an ERROR-8 abort implied) if the OACK offers an option the client
// never requested (RFC 2347 section 2: B-03).
bool apply_oack_to_client(const TftpPacket& oack,
                          const ClientRequestedOptions& requested,
                          NegotiatedParameters& params,
                          std::string& rejection_reason) {
    for (const auto& option : oack.options) {
        if (equals_ignore_ascii_case(option.name, "blksize")) {
            if (!requested.block_size) {
                rejection_reason = "server OACKed unrequested blksize";
                return false;
            }
            params.block_size =
                static_cast<std::uint32_t>(std::stoul(option.value));
        } else if (equals_ignore_ascii_case(option.name, "timeout")) {
            if (!requested.timeout_seconds) {
                rejection_reason = "server OACKed unrequested timeout";
                return false;
            }
            params.timeout_seconds =
                static_cast<std::uint32_t>(std::stoul(option.value));
        } else if (equals_ignore_ascii_case(option.name, "tsize")) {
            if (!requested.transfer_size) {
                rejection_reason = "server OACKed unrequested tsize";
                return false;
            }
            params.transfer_size =
                static_cast<std::uint64_t>(std::stoull(option.value));
        } else if (equals_ignore_ascii_case(option.name, "windowsize")) {
            if (!requested.window_size) {
                rejection_reason = "server OACKed unrequested windowsize";
                return false;
            }
            params.window_size =
                static_cast<std::uint16_t>(std::stoul(option.value));
        } else {
            rejection_reason = "server OACKed unknown option " + option.name;
            return false;
        }
    }
    return true;
}

// Open and bind a fresh client socket with the configured retransmission
// timeout applied as the receive timeout.
bool open_client_socket(UdpSocket& socket, const EngineConfig& config,
                        const Endpoint& server_endpoint) {
    if (!socket.open(server_endpoint.family())) {
        return false;
    }
    const std::string bind_host = server_endpoint.is_ipv6() ? "::" : "0.0.0.0";
    if (!socket.bind(bind_host, 0)) {
        return false;
    }
    socket.set_receive_timeout(config.retransmission_timeout);
    return true;
}

} // namespace

TransferOutcome ReferenceClient::read_file(
    const Endpoint& server_endpoint, const std::string& remote_filename,
    const std::filesystem::path& destination_path, const std::string& mode,
    const ClientRequestedOptions& options) {
    TransferOutcome outcome;
    UdpSocket socket;
    if (!open_client_socket(socket, config_, server_endpoint)) {
        outcome.error_message = "client could not open socket";
        return outcome;
    }

    const auto option_list = build_requested_option_list(options, config_);
    const auto request =
        serialize_read_request(remote_filename, mode, option_list);

    // Send the RRQ, retransmitting on timeout until the first reply arrives.
    TftpPacket first_reply;
    Endpoint locked_peer;
    bool have_reply = false;
    for (int attempt = 0; attempt <= config_.maximum_retransmissions; ++attempt) {
        socket.send_to(request, server_endpoint);
        std::vector<std::uint8_t> buffer;
        Endpoint sender;
        const long received = socket.receive_from(buffer, sender);
        if (received > 0) {
            auto parsed = parse_tftp_packet(buffer);
            if (parsed.ok) {
                first_reply = std::move(parsed.packet);
                locked_peer = sender; // lock onto the server's chosen TID
                have_reply = true;
                break;
            }
        }
    }
    if (!have_reply) {
        outcome.timed_out = true;
        outcome.error_message = "no reply to RRQ (server unreachable/loss)";
        return outcome;
    }

    NegotiatedParameters params;
    params.block_size = config_.default_block_size;
    params.window_size = 1;

    std::optional<TftpPacket> primed;
    std::uint32_t first_expected_block = 1;

    if (first_reply.opcode == TftpOpcode::Error) {
        outcome.error_code = first_reply.error_code;
        outcome.error_message = first_reply.error_message;
        return outcome;
    } else if (first_reply.opcode == TftpOpcode::OptionAcknowledgement) {
        std::string reason;
        if (!apply_oack_to_client(first_reply, options, params, reason)) {
            // RFC 2347: reject with ERROR 8 and abort (B-03).
            socket.send_to(serialize_error(
                               TftpErrorCode::OptionNegotiationFailed, reason),
                           locked_peer);
            outcome.error_code = TftpErrorCode::OptionNegotiationFailed;
            outcome.error_message = reason;
            return outcome;
        }
        // Acknowledge the OACK with ACK block 0, then receive from block 1.
        socket.send_to(serialize_acknowledgement(0), locked_peer);
    } else if (first_reply.opcode == TftpOpcode::Data) {
        // Server ignored options and started sending (plain RFC 1350). Feed the
        // already-received first DATA block into the receiver loop.
        primed = first_reply;
    } else {
        outcome.error_message = "unexpected first reply to RRQ";
        return outcome;
    }

    std::vector<std::uint8_t> received_bytes;
    TransferOutcome data_outcome =
        run_data_receiver(socket, locked_peer, params, config_, received_bytes,
                          first_expected_block, primed);
    data_outcome.negotiated = params;
    if (!data_outcome.completed_successfully) {
        return data_outcome;
    }

    // netascii decode before writing to the local filesystem (RFC 1350 App).
    std::vector<std::uint8_t> local_bytes =
        mode_is_netascii(mode) ? decode_netascii_to_local(received_bytes)
                               : received_bytes;
    if (!write_entire_file(destination_path, local_bytes)) {
        data_outcome.completed_successfully = false;
        data_outcome.error_message = "could not write destination file";
    }
    return data_outcome;
}

TransferOutcome ReferenceClient::write_file(
    const Endpoint& server_endpoint, const std::filesystem::path& source_path,
    const std::string& remote_filename, const std::string& mode,
    const ClientRequestedOptions& options) {
    TransferOutcome outcome;

    auto file_bytes = read_entire_file(source_path);
    if (!file_bytes) {
        outcome.error_message = "client could not read source file";
        return outcome;
    }
    std::vector<std::uint8_t> payload =
        mode_is_netascii(mode) ? encode_local_to_netascii(*file_bytes)
                               : *file_bytes;

    UdpSocket socket;
    if (!open_client_socket(socket, config_, server_endpoint)) {
        outcome.error_message = "client could not open socket";
        return outcome;
    }

    // For tsize on a WRQ the client reports the actual payload size.
    ClientRequestedOptions effective_options = options;
    if (options.transfer_size) {
        effective_options.transfer_size = payload.size();
    }
    const auto option_list =
        build_requested_option_list(effective_options, config_);
    const auto request =
        serialize_write_request(remote_filename, mode, option_list);

    TftpPacket first_reply;
    Endpoint locked_peer;
    bool have_reply = false;
    for (int attempt = 0; attempt <= config_.maximum_retransmissions; ++attempt) {
        socket.send_to(request, server_endpoint);
        std::vector<std::uint8_t> buffer;
        Endpoint sender;
        const long received = socket.receive_from(buffer, sender);
        if (received > 0) {
            auto parsed = parse_tftp_packet(buffer);
            if (parsed.ok) {
                first_reply = std::move(parsed.packet);
                locked_peer = sender;
                have_reply = true;
                break;
            }
        }
    }
    if (!have_reply) {
        outcome.timed_out = true;
        outcome.error_message = "no reply to WRQ (server unreachable/loss)";
        return outcome;
    }

    NegotiatedParameters params;
    params.block_size = config_.default_block_size;
    params.window_size = 1;

    if (first_reply.opcode == TftpOpcode::Error) {
        outcome.error_code = first_reply.error_code;
        outcome.error_message = first_reply.error_message;
        return outcome;
    } else if (first_reply.opcode == TftpOpcode::OptionAcknowledgement) {
        std::string reason;
        if (!apply_oack_to_client(first_reply, effective_options, params,
                                  reason)) {
            socket.send_to(serialize_error(
                               TftpErrorCode::OptionNegotiationFailed, reason),
                           locked_peer);
            outcome.error_code = TftpErrorCode::OptionNegotiationFailed;
            outcome.error_message = reason;
            return outcome;
        }
        // OACK received: the client now sends DATA starting at block 1.
    } else if (first_reply.opcode == TftpOpcode::Acknowledgement &&
               first_reply.block_number == 0) {
        // Plain RFC 1350: ACK of block 0 authorizes DATA block 1.
    } else {
        outcome.error_message = "unexpected first reply to WRQ";
        return outcome;
    }

    TransferOutcome data_outcome =
        run_data_sender(socket, locked_peer, payload, params, config_, 0);
    data_outcome.negotiated = params;
    return data_outcome;
}

// ===========================================================================
// Server
// ===========================================================================
Endpoint ReferenceServer::start(const std::filesystem::path& served_directory,
                                const std::string& host) {
    served_directory_ = served_directory;
    host_ = host;
    if (!listen_socket_.bind(host, 0)) {
        return Endpoint{};
    }
    // The accept loop uses a short poll timeout so stop() is responsive.
    listen_socket_.set_receive_timeout(std::chrono::milliseconds(200));
    const Endpoint bound = listen_socket_.local_endpoint();
    running_.store(true);
    accept_thread_ = std::thread([this] { accept_loop(); });
    return bound;
}

void ReferenceServer::stop() {
    running_.store(false);
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    listen_socket_.close();
}

void ReferenceServer::accept_loop() {
    while (running_.load()) {
        std::vector<std::uint8_t> buffer;
        Endpoint client;
        const long received = listen_socket_.receive_from(buffer, client);
        if (received <= 0) {
            continue; // timeout poll or transient error
        }
        auto parsed = parse_tftp_packet(buffer);
        if (!parsed.ok) {
            continue; // ignore malformed request at the listen port
        }
        if (parsed.packet.opcode != TftpOpcode::ReadRequest &&
            parsed.packet.opcode != TftpOpcode::WriteRequest) {
            continue; // only requests are accepted at the well-known port
        }
        // Handle each request on its own thread with a fresh TID.
        ++active_handlers_;
        std::thread(
            [this, request = std::move(parsed.packet), client]() mutable {
                handle_request(std::move(request), client);
                --active_handlers_;
            })
            .detach();
    }
    // Drain outstanding handlers so ports are released before we return.
    while (active_handlers_.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

namespace {

// Resolve a requested filename against the served directory, rejecting any path
// that escapes it (path traversal → access violation, RFC 1350 error code 2).
bool resolve_served_path(const std::filesystem::path& root,
                         const std::string& filename,
                         std::filesystem::path& resolved) {
    if (filename.empty()) {
        return false;
    }
    std::filesystem::path candidate = root / filename;
    std::error_code error;
    const auto root_canonical =
        std::filesystem::weakly_canonical(root, error);
    const auto candidate_canonical =
        std::filesystem::weakly_canonical(candidate, error);
    if (error) {
        return false;
    }
    // candidate must be within root.
    const auto root_string = root_canonical.string();
    const auto candidate_string = candidate_canonical.string();
    if (candidate_string.size() < root_string.size() ||
        candidate_string.compare(0, root_string.size(), root_string) != 0) {
        return false;
    }
    resolved = candidate_canonical;
    return true;
}

// Build the server's OACK option list from a request, clamping to legal ranges
// and honoring capability toggles. `actual_file_size` supplies tsize on a RRQ.
std::vector<TftpOption> negotiate_server_options(
    const TftpPacket& request, const EngineConfig& config,
    std::optional<std::uint64_t> actual_file_size,
    NegotiatedParameters& params) {
    std::vector<TftpOption> oack_options;
    if (!config.support_option_negotiation) {
        return oack_options; // pre-RFC-2347 behavior: ignore all options (B-02)
    }
    for (const auto& option : request.options) {
        if (equals_ignore_ascii_case(option.name, "blksize") &&
            config.support_block_size_option) {
            std::uint32_t requested = 0;
            try {
                requested = static_cast<std::uint32_t>(std::stoul(option.value));
            } catch (...) {
                continue;
            }
            const std::uint32_t accepted = clamp_block_size(requested);
            params.block_size = accepted;
            oack_options.push_back({"blksize", std::to_string(accepted)});
        } else if (equals_ignore_ascii_case(option.name, "timeout") &&
                   config.support_timeout_option) {
            std::uint32_t requested = 0;
            try {
                requested = static_cast<std::uint32_t>(std::stoul(option.value));
            } catch (...) {
                continue;
            }
            if (requested < 1 || requested > 255) {
                continue; // out of range (RFC 2349): decline the option
            }
            params.timeout_seconds = requested;
            oack_options.push_back({"timeout", std::to_string(requested)});
        } else if (equals_ignore_ascii_case(option.name, "tsize") &&
                   config.support_transfer_size_option) {
            if (actual_file_size) {
                // RRQ: report the real size back (RFC 2349 section 3).
                params.transfer_size = *actual_file_size;
                oack_options.push_back(
                    {"tsize", std::to_string(*actual_file_size)});
            } else {
                // WRQ: echo the client's declared size.
                try {
                    params.transfer_size =
                        static_cast<std::uint64_t>(std::stoull(option.value));
                    oack_options.push_back({"tsize", option.value});
                } catch (...) {
                }
            }
        } else if (equals_ignore_ascii_case(option.name, "windowsize") &&
                   config.support_window_size_option) {
            std::uint32_t requested = 0;
            try {
                requested = static_cast<std::uint32_t>(std::stoul(option.value));
            } catch (...) {
                continue;
            }
            if (requested < 1) {
                continue;
            }
            const std::uint16_t accepted =
                static_cast<std::uint16_t>(std::min<std::uint32_t>(requested, 65535));
            params.window_size = accepted;
            oack_options.push_back({"windowsize", std::to_string(accepted)});
        }
        // Unknown options are silently ignored (RFC 2347 section 2): the server
        // MUST NOT send an ERROR for options it does not recognize (B-02).
    }
    return oack_options;
}

} // namespace

void ReferenceServer::handle_request(TftpPacket request,
                                     Endpoint client_endpoint) {
    // Each transfer gets its own socket => a fresh ephemeral TID (RFC 1350
    // section 4). This is the port the client will observe the server switch to
    // (A-20).
    UdpSocket transfer_socket;
    if (!transfer_socket.open(client_endpoint.family())) {
        return;
    }
    const std::string bind_host = client_endpoint.is_ipv6() ? "::" : "0.0.0.0";
    if (!transfer_socket.bind(bind_host, 0)) {
        return;
    }
    transfer_socket.set_receive_timeout(config_.retransmission_timeout);

    const bool netascii = mode_is_netascii(request.mode);

    if (request.opcode == TftpOpcode::ReadRequest) {
        std::filesystem::path path;
        if (!resolve_served_path(served_directory_, request.filename, path)) {
            transfer_socket.send_to(
                serialize_error(TftpErrorCode::AccessViolation,
                                "access violation"),
                client_endpoint);
            return;
        }
        auto file_bytes = read_entire_file(path);
        if (!file_bytes) {
            transfer_socket.send_to(
                serialize_error(TftpErrorCode::FileNotFound, "file not found"),
                client_endpoint);
            return;
        }
        std::vector<std::uint8_t> payload =
            netascii ? encode_local_to_netascii(*file_bytes) : *file_bytes;

        NegotiatedParameters params;
        params.block_size = config_.default_block_size;
        params.window_size = 1;
        auto oack_options = negotiate_server_options(
            request, config_, static_cast<std::uint64_t>(payload.size()),
            params);

        if (!oack_options.empty()) {
            // Send OACK and wait for the client's ACK of block 0 before data.
            bool acked = false;
            for (int attempt = 0;
                 attempt <= config_.maximum_retransmissions && !acked;
                 ++attempt) {
                transfer_socket.send_to(
                    serialize_option_acknowledgement(oack_options),
                    client_endpoint);
                TftpPacket reply;
                const ReceiveStatus status = receive_from_locked_peer(
                    transfer_socket, client_endpoint, reply);
                if (status == ReceiveStatus::GotPacket) {
                    if (reply.opcode == TftpOpcode::Acknowledgement &&
                        reply.block_number == 0) {
                        acked = true;
                    } else if (reply.opcode == TftpOpcode::Error) {
                        return; // client rejected the options (e.g. ERROR 8)
                    }
                }
            }
            if (!acked) {
                return; // client never acknowledged the OACK
            }
        }
        run_data_sender(transfer_socket, client_endpoint, payload, params,
                        config_, 0);
        return;
    }

    if (request.opcode == TftpOpcode::WriteRequest) {
        std::filesystem::path path;
        if (!resolve_served_path(served_directory_, request.filename, path)) {
            transfer_socket.send_to(
                serialize_error(TftpErrorCode::AccessViolation,
                                "access violation"),
                client_endpoint);
            return;
        }
        std::error_code exists_error;
        if (config_.reject_overwrite_on_write_request &&
            std::filesystem::exists(path, exists_error)) {
            transfer_socket.send_to(
                serialize_error(TftpErrorCode::FileAlreadyExists,
                                "file already exists"),
                client_endpoint);
            return;
        }

        NegotiatedParameters params;
        params.block_size = config_.default_block_size;
        params.window_size = 1;
        auto oack_options =
            negotiate_server_options(request, config_, std::nullopt, params);

        if (!oack_options.empty()) {
            transfer_socket.send_to(
                serialize_option_acknowledgement(oack_options),
                client_endpoint);
        } else {
            // Plain RFC 1350: ACK block 0 authorizes the client's DATA block 1.
            transfer_socket.send_to(serialize_acknowledgement(0),
                                    client_endpoint);
        }

        std::vector<std::uint8_t> received_bytes;
        TransferOutcome data_outcome =
            run_data_receiver(transfer_socket, client_endpoint, params, config_,
                              received_bytes, 1, std::nullopt);
        if (data_outcome.completed_successfully) {
            std::vector<std::uint8_t> local_bytes =
                netascii ? decode_netascii_to_local(received_bytes)
                         : received_bytes;
            write_entire_file(path, local_bytes);
        }
        return;
    }
}

} // namespace tftp_test_harness::reference
