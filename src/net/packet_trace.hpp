#ifndef TFTP_TEST_HARNESS_NET_PACKET_TRACE_HPP
#define TFTP_TEST_HARNESS_NET_PACKET_TRACE_HPP

// ---------------------------------------------------------------------------
// The packet trace produced by the impairment proxy and consumed by the packet
// observer / oracle. Because the proxy sees every datagram in both directions,
// this trace is the ground truth the observer uses to make packet-level
// conformance judgments the adapters cannot make on their own (Sorcerer's
// Apprentice amplification, TID discipline, final-block correctness, block
// monotonicity, windowsize rollback).
//
// One record is appended per datagram the proxy observes arriving from a real
// peer, plus one per datagram the proxy itself injects. The record captures
// what was on the wire (direction, source/destination TID, parsed opcode and
// block number, length) and what the proxy did with it (delivered, dropped,
// corrupted, injected).
// ---------------------------------------------------------------------------

#include "net/impairments.hpp"
#include "net/tftp_packet.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tftp_test_harness::net {

// What the proxy did with an observed datagram.
enum class TraceDisposition {
    Delivered,   // forwarded intact to the far side
    Dropped,     // lost by an impairment stage (never reached the far side)
    Corrupted,   // forwarded, but with mutated bytes (F-06)
    Injected,    // synthesized by the proxy (A-21/A-22/G-series), not a peer send
};

const char* trace_disposition_name(TraceDisposition disposition);

// A single observed (or injected) datagram.
struct TraceRecord {
    double relative_time_ms = 0.0; // since this session's first datagram
    Direction direction = Direction::ClientToServer;

    // The source and destination UDP ports — the TIDs (RFC 1350 section 4).
    std::uint16_t source_tid = 0;
    std::uint16_t destination_tid = 0;

    std::size_t datagram_length = 0;
    bool parseable = false;
    TftpPacket packet;             // valid when parseable
    std::string parse_failure;     // when !parseable

    TraceDisposition disposition = TraceDisposition::Delivered;
    bool was_duplicated = false;   // proxy emitted an extra copy (F-03)
    std::string note;              // drop reason / injection description

    // Convenience accessors used throughout the observer.
    bool is_data() const {
        return parseable && packet.opcode == TftpOpcode::Data;
    }
    bool is_ack() const {
        return parseable && packet.opcode == TftpOpcode::Acknowledgement;
    }
    bool is_error() const {
        return parseable && packet.opcode == TftpOpcode::Error;
    }
    bool reached_peer() const {
        return disposition == TraceDisposition::Delivered ||
               disposition == TraceDisposition::Corrupted ||
               disposition == TraceDisposition::Injected;
    }
};

// The full trace of one transfer, plus the seed that produced it (recorded so a
// failing run reproduces exactly).
struct PacketTrace {
    std::uint64_t seed = 0;
    std::vector<TraceRecord> records;
};

} // namespace tftp_test_harness::net

#endif // TFTP_TEST_HARNESS_NET_PACKET_TRACE_HPP
