// Unit tests for the TFTP packet codec (RFC 1350 section 5 wire formats plus
// the RFC 2347 option list). These lock down the one module that knows the wire
// layout; every other component trusts it.

#include "net/tftp_packet.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace tftp_test_harness::net;

namespace {

std::vector<std::uint8_t> bytes(std::initializer_list<int> values) {
    std::vector<std::uint8_t> out;
    for (int value : values) {
        out.push_back(static_cast<std::uint8_t>(value));
    }
    return out;
}

} // namespace

TFTP_TEST_CASE(rrq_round_trips, "RRQ serialize/parse round trip") {
    auto wire = serialize_read_request("boot.img", "octet");
    auto parsed = parse_tftp_packet(wire);
    TFTP_CHECK_TRUE(parsed.ok);
    TFTP_CHECK_TRUE(parsed.packet.opcode == TftpOpcode::ReadRequest);
    TFTP_CHECK_EQUAL(parsed.packet.filename, std::string("boot.img"));
    TFTP_CHECK_EQUAL(parsed.packet.mode, std::string("octet"));
    TFTP_CHECK_TRUE(parsed.packet.options.empty());
}

TFTP_TEST_CASE(rrq_with_options, "RRQ carries an RFC 2347 option list") {
    std::vector<TftpOption> options{{"blksize", "1428"}, {"tsize", "0"}};
    auto wire = serialize_read_request("f", "octet", options);
    auto parsed = parse_tftp_packet(wire);
    TFTP_CHECK_TRUE(parsed.ok);
    TFTP_CHECK_EQUAL(parsed.packet.options.size(), std::size_t(2));
    // RFC 2347 section 2: option names are case-insensitive.
    const TftpOption* blocksize = parsed.packet.find_option("BLKSIZE");
    TFTP_CHECK_TRUE(blocksize != nullptr);
    TFTP_CHECK_EQUAL(blocksize->value, std::string("1428"));
}

TFTP_TEST_CASE(data_round_trips, "DATA serialize/parse preserves payload") {
    std::vector<std::uint8_t> payload{0x00, 0xFF, 0x10, 0x42};
    auto wire = serialize_data(7, payload);
    auto parsed = parse_tftp_packet(wire);
    TFTP_CHECK_TRUE(parsed.ok);
    TFTP_CHECK_TRUE(parsed.packet.opcode == TftpOpcode::Data);
    TFTP_CHECK_EQUAL(parsed.packet.block_number, std::uint16_t(7));
    TFTP_CHECK_TRUE(parsed.packet.data == payload);
}

TFTP_TEST_CASE(zero_length_final_data, "Zero-length final DATA parses (A-04/A-05)") {
    // RFC 1350 section 2: a DATA packet with a payload shorter than the block
    // size terminates the transfer; a length-0 final DATA is legal and common.
    auto wire = serialize_data(4, nullptr, 0);
    TFTP_CHECK_EQUAL(wire.size(), std::size_t(4));
    auto parsed = parse_tftp_packet(wire);
    TFTP_CHECK_TRUE(parsed.ok);
    TFTP_CHECK_TRUE(parsed.packet.data.empty());
}

TFTP_TEST_CASE(ack_round_trips, "ACK serialize/parse") {
    auto wire = serialize_acknowledgement(0);
    TFTP_CHECK_EQUAL(wire.size(), std::size_t(4));
    auto parsed = parse_tftp_packet(wire);
    TFTP_CHECK_TRUE(parsed.ok);
    TFTP_CHECK_TRUE(parsed.packet.opcode == TftpOpcode::Acknowledgement);
    TFTP_CHECK_EQUAL(parsed.packet.block_number, std::uint16_t(0));
}

TFTP_TEST_CASE(error_round_trips, "ERROR serialize/parse (RFC 1350 section 5)") {
    auto wire = serialize_error(TftpErrorCode::FileNotFound, "no such file");
    auto parsed = parse_tftp_packet(wire);
    TFTP_CHECK_TRUE(parsed.ok);
    TFTP_CHECK_TRUE(parsed.packet.opcode == TftpOpcode::Error);
    TFTP_CHECK_TRUE(parsed.packet.error_code == TftpErrorCode::FileNotFound);
    TFTP_CHECK_EQUAL(parsed.packet.error_message, std::string("no such file"));
}

TFTP_TEST_CASE(oack_round_trips, "OACK serialize/parse (RFC 2347)") {
    std::vector<TftpOption> options{{"blksize", "512"}, {"windowsize", "4"}};
    auto wire = serialize_option_acknowledgement(options);
    auto parsed = parse_tftp_packet(wire);
    TFTP_CHECK_TRUE(parsed.ok);
    TFTP_CHECK_TRUE(parsed.packet.opcode == TftpOpcode::OptionAcknowledgement);
    TFTP_CHECK_EQUAL(parsed.packet.options.size(), std::size_t(2));
}

TFTP_TEST_CASE(reject_short_datagram, "Datagram under 2 bytes is rejected (G-01)") {
    auto parsed = parse_tftp_packet(bytes({0x00}));
    TFTP_CHECK_FALSE(parsed.ok);
}

TFTP_TEST_CASE(reject_unterminated_filename,
               "RRQ without NUL-terminated filename is rejected (A-15/G-05)") {
    // opcode 1 then "abc" with no terminating NUL at all.
    auto parsed = parse_tftp_packet(bytes({0x00, 0x01, 'a', 'b', 'c'}));
    TFTP_CHECK_FALSE(parsed.ok);
    TFTP_CHECK_TRUE(parsed.failure_reason.find("NUL") != std::string::npos);
}

TFTP_TEST_CASE(reject_unknown_opcode, "Unknown opcode is rejected (G-03)") {
    auto parsed = parse_tftp_packet(bytes({0x00, 0x09, 0x00, 0x00}));
    TFTP_CHECK_FALSE(parsed.ok);
    auto zero = parse_tftp_packet(bytes({0x00, 0x00, 0x00, 0x00}));
    TFTP_CHECK_FALSE(zero.ok);
    auto max = parse_tftp_packet(bytes({0xFF, 0xFF, 0x00, 0x00}));
    TFTP_CHECK_FALSE(max.ok);
}

TFTP_TEST_CASE(peek_opcode_works, "peek_opcode reads the leading opcode") {
    auto wire = serialize_acknowledgement(3);
    auto opcode = peek_opcode(wire);
    TFTP_CHECK_TRUE(opcode.has_value());
    TFTP_CHECK_EQUAL(*opcode, std::uint16_t(4));
    TFTP_CHECK_FALSE(peek_opcode(bytes({0x00})).has_value());
}

TFTP_TEST_CASE(case_insensitive_option_compare,
               "RFC 2347 option-name comparison is case-insensitive") {
    TFTP_CHECK_TRUE(equals_ignore_ascii_case("BLKSIZE", "blksize"));
    TFTP_CHECK_TRUE(equals_ignore_ascii_case("TSize", "tsize"));
    TFTP_CHECK_FALSE(equals_ignore_ascii_case("blksize", "tsize"));
}

TFTP_TEST_MAIN()
