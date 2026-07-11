#ifndef TFTP_TEST_HARNESS_VERIFY_PACKET_OBSERVER_HPP
#define TFTP_TEST_HARNESS_VERIFY_PACKET_OBSERVER_HPP

// ---------------------------------------------------------------------------
// The packet-level conformance observer. It parses the proxy's packet trace and
// asserts behaviors the adapters cannot see from the outside:
//   * final-block rule (A-04/A-05): a transfer ends with a DATA block shorter
//     than the block size (RFC 1350 section 2);
//   * lock-step / windowed discipline and Sorcerer's Apprentice amplification
//     (A-32/A-33, RFC 1123 section 4.2): the sender must not retransmit in
//     response to a duplicate ACK;
//   * TID discipline (A-20/A-21/A-22, RFC 1350 section 4): the server chooses a
//     fresh TID, and a stray-TID datagram is rebuffed (ERROR 5) without
//     aborting the legitimate transfer;
//   * block-number monotonicity and wraparound (A-40/A-41).
//
// The observer computes an ObserverReport of measurable facts; each test case
// interprets the fields it cares about against its RFC-derived expectation. This
// keeps the observer objective (it measures) and the tests declarative (they
// judge).
// ---------------------------------------------------------------------------

#include "net/packet_trace.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tftp_test_harness::verify {

// Which side sends the DATA stream. On a download (RRQ) the server sends DATA;
// on an upload (WRQ) the client does. The observer needs this to know which
// direction is "the sender" for final-block and amplification checks.
enum class TransferDirection {
    Download, // server -> client DATA
    Upload,   // client -> server DATA
};

struct ObserverContext {
    TransferDirection direction = TransferDirection::Download;
    std::uint32_t block_size = 512;
    bool transfer_reported_complete = false; // the app's own view
};

struct ObserverReport {
    // Counts over the DATA-bearing direction.
    std::uint32_t data_packets_sent = 0;
    std::uint32_t distinct_data_blocks = 0;
    std::uint32_t retransmitted_data_packets = 0; // sent - distinct
    std::size_t final_data_block_length = 0;      // length of the last DATA sent
    bool terminating_short_block_present = false; // any DATA with len < blksize

    // Counts over the ACK-bearing direction.
    std::uint32_t acknowledgements_sent = 0;
    std::uint32_t duplicate_acknowledgements = 0;

    // Impairment / injection visibility.
    std::uint32_t datagrams_dropped = 0;
    std::uint32_t datagrams_corrupted = 0;
    std::uint32_t datagrams_injected = 0;
    std::uint32_t stray_tid_replies = 0; // victim ERROR replies to stray TIDs
    bool stray_tid_reply_was_error_5 = false;

    // TID discipline.
    bool server_selected_fresh_tid = false;
    std::uint16_t request_destination_tid = 0;
    std::uint16_t server_source_tid = 0;

    // Block numbering.
    std::uint16_t highest_block_number = 0;
    bool block_number_wrapped = false; // observed a 65535 -> low transition

    // Total datagrams observed (both directions, all dispositions).
    std::uint32_t total_records = 0;

    std::string narrative() const;
};

ObserverReport observe(const net::PacketTrace& trace,
                       const ObserverContext& context);

} // namespace tftp_test_harness::verify

#endif // TFTP_TEST_HARNESS_VERIFY_PACKET_OBSERVER_HPP
