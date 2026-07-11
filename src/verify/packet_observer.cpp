#include "verify/packet_observer.hpp"

#include <algorithm>
#include <set>

namespace tftp_test_harness::verify {

using net::Direction;
using net::TftpErrorCode;
using net::TftpOpcode;
using net::TraceDisposition;
using net::TraceRecord;

namespace {

// The direction datagrams flow when the transfer's DATA stream travels, and the
// opposite direction where its ACKs travel.
Direction data_direction(TransferDirection direction) {
    return direction == TransferDirection::Download ? Direction::ServerToClient
                                                    : Direction::ClientToServer;
}
Direction ack_direction(TransferDirection direction) {
    return direction == TransferDirection::Download ? Direction::ClientToServer
                                                    : Direction::ServerToClient;
}

// Only datagrams that actually left a peer count as "sent" for the sender-side
// checks: delivered, corrupted-but-forwarded, and dropped all represent a peer
// transmission the proxy observed (drops are still peer sends). Injected
// datagrams are proxy synthesized and excluded from peer-send accounting.
bool is_peer_send(const TraceRecord& record) {
    return record.disposition != TraceDisposition::Injected;
}

} // namespace

ObserverReport observe(const net::PacketTrace& trace,
                       const ObserverContext& context) {
    ObserverReport report;
    report.total_records = static_cast<std::uint32_t>(trace.records.size());

    const Direction data_dir = data_direction(context.direction);
    const Direction ack_dir = ack_direction(context.direction);

    std::set<std::uint16_t> distinct_blocks;
    std::map<std::uint16_t, std::uint32_t> data_send_count;
    std::size_t last_data_length = 0;
    std::uint16_t last_data_block = 0;
    bool have_data = false;

    std::uint16_t previous_block_for_wrap = 0;
    bool have_previous_block = false;

    // ACK duplicate tracking (a duplicate ACK repeats the highest block already
    // acknowledged).
    std::uint16_t highest_ack = 0;
    bool have_ack = false;

    bool found_request = false;

    for (const auto& record : trace.records) {
        switch (record.disposition) {
            case TraceDisposition::Dropped:
                ++report.datagrams_dropped;
                break;
            case TraceDisposition::Corrupted:
                ++report.datagrams_corrupted;
                break;
            case TraceDisposition::Injected:
                ++report.datagrams_injected;
                break;
            case TraceDisposition::Delivered:
                break;
        }

        // Record the request's destination TID (the server's well-known port),
        // and later compare it against the server's chosen source TID.
        if (!found_request && is_peer_send(record) && record.parseable &&
            (record.packet.opcode == TftpOpcode::ReadRequest ||
             record.packet.opcode == TftpOpcode::WriteRequest)) {
            report.request_destination_tid = record.destination_tid;
            found_request = true;
        }

        // The server's first DATA/OACK reveals its chosen TID (A-20).
        if (report.server_source_tid == 0 && is_peer_send(record) &&
            record.direction == Direction::ServerToClient && record.parseable &&
            (record.packet.opcode == TftpOpcode::Data ||
             record.packet.opcode == TftpOpcode::OptionAcknowledgement)) {
            report.server_source_tid = record.source_tid;
        }

        // Stray-TID victim replies recorded by the proxy.
        if (record.disposition == TraceDisposition::Injected &&
            record.note.find("reply to stray TID") != std::string::npos) {
            ++report.stray_tid_replies;
            if (record.parseable &&
                record.packet.opcode == TftpOpcode::Error &&
                record.packet.error_code == TftpErrorCode::UnknownTransferId) {
                report.stray_tid_reply_was_error_5 = true;
            }
        }

        // DATA-direction accounting (peer sends only).
        if (record.direction == data_dir && is_peer_send(record) &&
            record.is_data()) {
            ++report.data_packets_sent;
            distinct_blocks.insert(record.packet.block_number);
            data_send_count[record.packet.block_number] += 1;
            last_data_length = record.packet.data.size();
            last_data_block = record.packet.block_number;
            have_data = true;
            report.highest_block_number =
                std::max(report.highest_block_number, record.packet.block_number);
            if (have_previous_block && previous_block_for_wrap == 0xFFFF &&
                record.packet.block_number < 100) {
                report.block_number_wrapped = true;
            }
            previous_block_for_wrap = record.packet.block_number;
            have_previous_block = true;
            if (record.packet.data.size() < context.block_size) {
                report.terminating_short_block_present = true;
            }
        }

        // ACK-direction accounting (peer sends only).
        if (record.direction == ack_dir && is_peer_send(record) &&
            record.is_ack()) {
            ++report.acknowledgements_sent;
            if (have_ack && record.packet.block_number == highest_ack) {
                ++report.duplicate_acknowledgements;
            }
            if (!have_ack || record.packet.block_number > highest_ack) {
                highest_ack = record.packet.block_number;
            }
            have_ack = true;
        }
    }

    report.distinct_data_blocks =
        static_cast<std::uint32_t>(distinct_blocks.size());
    report.retransmitted_data_packets =
        report.data_packets_sent - report.distinct_data_blocks;
    if (have_data) {
        report.final_data_block_length = last_data_length;
        (void)last_data_block;
    }

    report.server_selected_fresh_tid =
        found_request && report.server_source_tid != 0 &&
        report.server_source_tid != report.request_destination_tid;

    return report;
}

std::string ObserverReport::narrative() const {
    std::string text;
    text += "DATA sent=" + std::to_string(data_packets_sent) +
            " distinct=" + std::to_string(distinct_data_blocks) +
            " retransmit=" + std::to_string(retransmitted_data_packets);
    text += "; ACK sent=" + std::to_string(acknowledgements_sent) +
            " dup=" + std::to_string(duplicate_acknowledgements);
    text += "; dropped=" + std::to_string(datagrams_dropped) +
            " corrupted=" + std::to_string(datagrams_corrupted) +
            " injected=" + std::to_string(datagrams_injected);
    text += "; final_block_len=" + std::to_string(final_data_block_length) +
            " terminating_short=" +
            (terminating_short_block_present ? "yes" : "no");
    text += "; server_fresh_tid=" +
            std::string(server_selected_fresh_tid ? "yes" : "no");
    if (stray_tid_replies > 0) {
        text += "; stray_tid_replies=" + std::to_string(stray_tid_replies) +
                (stray_tid_reply_was_error_5 ? " (ERROR 5)" : "");
    }
    if (block_number_wrapped) {
        text += "; block-number wraparound observed";
    }
    return text;
}

} // namespace tftp_test_harness::verify
