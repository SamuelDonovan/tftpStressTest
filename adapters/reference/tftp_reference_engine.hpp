#ifndef TFTP_TEST_HARNESS_REFERENCE_TFTP_REFERENCE_ENGINE_HPP
#define TFTP_TEST_HARNESS_REFERENCE_TFTP_REFERENCE_ENGINE_HPP

// ---------------------------------------------------------------------------
// A self-contained, in-process TFTP engine that can act as either a client or a
// server, implementing RFC 1350 (base), RFC 2347 (option extension), RFC 2348
// (blksize), RFC 2349 (timeout / tsize) and RFC 7440 (windowsize).
//
// Two roles the harness needs it for:
//   * As the *reference implementation under test* — the correct engine must
//     pass the whole matrix and the deliberately buggy variant must be flagged
//     on exactly its injected defects (self-verification).
//   * As the *peer* for whichever side is under test — e.g. to evaluate a
//     client the harness needs a correct server behind the proxy, and vice
//     versa.
//
// Correctness-affecting deviations are isolated in EngineQuirks so the buggy
// reference is the same code path with named defects switched on, keeping the
// injected bugs explicit and auditable rather than forked into a separate,
// possibly-differently-wrong implementation.
// ---------------------------------------------------------------------------

#include "net/deterministic_prng.hpp"
#include "net/endpoint.hpp"
#include "net/tftp_packet.hpp"
#include "net/udp_socket.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tftp_test_harness::reference {

// Named, auditable defects. Every field defaults to the correct behavior; the
// buggy reference sets exactly the ones it means to exhibit.
struct EngineQuirks {
    // Sorcerer's Apprentice Syndrome (RFC 1123 section 4.2): a correct sender
    // retransmits DATA only on timeout, never in response to a duplicate ACK.
    // When true the sender retransmits on every duplicate ACK, producing the
    // packet-doubling cascade that A-32/A-33 and F-03 detect.
    bool retransmit_on_duplicate_ack = false;

    // Final-block rule (RFC 1350 section 2): a transfer ends with a DATA packet
    // whose payload is shorter than the block size, so a file that is an exact
    // multiple of the block size requires a terminating zero-length DATA. When
    // true the sender omits that terminating block, so the receiver of an
    // exact-multiple file never sees end-of-transfer. Detected by A-04/A-05.
    bool omit_terminating_empty_block = false;

    // When true a receiver that gets an out-of-sequence DATA block accepts it as
    // if it were in sequence (writing it at the wrong offset), instead of
    // discarding and re-ACKing the last in-sequence block. Silent corruption;
    // detected by A-41 / E-04 and, ultimately, the content oracle.
    bool accept_out_of_sequence_data = false;
};

// Engine configuration shared by client and server construction.
struct EngineConfig {
    std::uint32_t default_block_size = 512; // RFC 2348 default
    std::chrono::milliseconds retransmission_timeout{800};
    int maximum_retransmissions = 6; // give-up bound (A-34)

    // Server-side policy.
    bool reject_overwrite_on_write_request = true; // WRQ to existing => ERROR 6

    // Capability toggles. Turning one off models an implementation that does not
    // support that feature (e.g. support_option_negotiation=false models a
    // pre-RFC-2347 server, exercising B-02). The matching adapter reports the
    // capability as unsupported so those tests are SKIPPED rather than FAILED.
    bool support_option_negotiation = true;
    bool support_block_size_option = true;
    bool support_timeout_option = true;
    bool support_transfer_size_option = true;
    bool support_window_size_option = true;
    bool support_netascii = true;

    EngineQuirks quirks;
};

// The negotiated parameters actually in force for one transfer, after any OACK.
struct NegotiatedParameters {
    std::uint32_t block_size = 512;
    std::uint16_t window_size = 1;               // RFC 7440; 1 == lock-step
    std::optional<std::uint32_t> timeout_seconds; // RFC 2349
    std::optional<std::uint64_t> transfer_size;   // RFC 2349 tsize
};

// Outcome of a client-driven transfer, from the engine's own point of view.
struct TransferOutcome {
    bool completed_successfully = false;
    std::optional<net::TftpErrorCode> error_code;
    std::string error_message;
    NegotiatedParameters negotiated;
    std::uint64_t bytes_transferred = 0;
    std::uint32_t data_packets_sent = 0;
    std::uint32_t data_packets_received = 0;
    std::uint32_t retransmissions = 0;
    bool timed_out = false; // gave up after maximum_retransmissions
};

// Options a client may request per transfer (std::nullopt == not requested).
struct ClientRequestedOptions {
    std::optional<std::uint32_t> block_size;
    std::optional<std::uint32_t> timeout_seconds;
    std::optional<std::uint64_t> transfer_size;
    std::optional<std::uint16_t> window_size;
};

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
class ReferenceClient {
public:
    explicit ReferenceClient(EngineConfig config) : config_(config) {}

    // Transfer mode is the on-wire mode string ("octet" / "netascii"), keeping
    // the engine independent of the adapter's TransferMode enum.

    // Download remote_filename from server into destination_path (RRQ).
    TransferOutcome read_file(const net::Endpoint& server_endpoint,
                              const std::string& remote_filename,
                              const std::filesystem::path& destination_path,
                              const std::string& mode,
                              const ClientRequestedOptions& options);

    // Upload source_path to the server as remote_filename (WRQ).
    TransferOutcome write_file(const net::Endpoint& server_endpoint,
                               const std::filesystem::path& source_path,
                               const std::string& remote_filename,
                               const std::string& mode,
                               const ClientRequestedOptions& options);

private:
    EngineConfig config_;
};

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
class ReferenceServer {
public:
    explicit ReferenceServer(EngineConfig config) : config_(config) {}
    ~ReferenceServer() { stop(); }

    ReferenceServer(const ReferenceServer&) = delete;
    ReferenceServer& operator=(const ReferenceServer&) = delete;

    // Begin serving files out of served_directory on host:0 (ephemeral). Spawns
    // an accept thread and returns the bound endpoint (the server's well-known
    // port from the client's point of view). Each accepted request is handled
    // on its own socket with a fresh ephemeral TID (RFC 1350 section 4), on its
    // own thread, so concurrent transfers (H-01) are supported.
    net::Endpoint start(const std::filesystem::path& served_directory,
                        const std::string& host = "127.0.0.1");
    void stop();

    bool is_running() const { return running_.load(); }

private:
    void accept_loop();
    void handle_request(net::TftpPacket request, net::Endpoint client_endpoint);

    EngineConfig config_;
    std::filesystem::path served_directory_;
    std::string host_;
    net::UdpSocket listen_socket_;
    std::thread accept_thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> active_handlers_{0};
};

// Read an entire file into memory. Returns nullopt if it cannot be opened.
std::optional<std::vector<std::uint8_t>> read_entire_file(
    const std::filesystem::path& path);

// Write bytes to a file, creating/truncating. Returns success.
bool write_entire_file(const std::filesystem::path& path,
                       const std::vector<std::uint8_t>& bytes);

} // namespace tftp_test_harness::reference

#endif // TFTP_TEST_HARNESS_REFERENCE_TFTP_REFERENCE_ENGINE_HPP
