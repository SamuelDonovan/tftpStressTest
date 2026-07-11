#ifndef TFTP_TEST_HARNESS_NET_TFTP_PACKET_HPP
#define TFTP_TEST_HARNESS_NET_TFTP_PACKET_HPP

// ---------------------------------------------------------------------------
// TFTP packet parse / serialize.
//
// Wire formats (RFC 1350 section 5, extended by RFC 2347/2348/2349/7440):
//
//   RRQ / WRQ  | 2 opcode | filename | 0 | mode | 0 | (opt | 0 | value | 0)* |
//   DATA       | 2 opcode | 2 block# | data (0..blocksize bytes)             |
//   ACK        | 2 opcode | 2 block#                                         |
//   ERROR      | 2 opcode | 2 error code | error message | 0                 |
//   OACK       | 2 opcode | (opt | 0 | value | 0)*                           |
//
// All 16-bit fields are network byte order (big-endian). This module is the one
// place that knows the wire layout; the proxy, observer, reference engine and
// tests all speak in terms of the parsed structures below.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tftp_test_harness::net {

// RFC 1350 section 5 opcodes, extended by RFC 2347 (OACK = 6).
enum class TftpOpcode : std::uint16_t {
    ReadRequest = 1,   // RRQ
    WriteRequest = 2,  // WRQ
    Data = 3,          // DATA
    Acknowledgement = 4, // ACK
    Error = 5,         // ERROR
    OptionAcknowledgement = 6, // OACK (RFC 2347)
};

// RFC 1350 section 5 defines error codes 0..7; RFC 2347 adds code 8.
enum class TftpErrorCode : std::uint16_t {
    NotDefined = 0,
    FileNotFound = 1,
    AccessViolation = 2,
    DiskFull = 3,
    IllegalOperation = 4,
    UnknownTransferId = 5,
    FileAlreadyExists = 6,
    NoSuchUser = 7,
    OptionNegotiationFailed = 8, // RFC 2347 section 2
};

// One negotiated option name/value pair (RFC 2347). Names are compared
// case-insensitively per RFC 2347 section 2 (see equals_ignore_ascii_case).
struct TftpOption {
    std::string name;
    std::string value;
};

// A fully parsed TFTP packet. Only the fields relevant to the packet's opcode
// are populated. The raw bytes are retained so the observer and proxy can
// forward or re-inspect the exact datagram that was on the wire.
struct TftpPacket {
    TftpOpcode opcode{};

    // RRQ / WRQ
    std::string filename;
    std::string mode;                 // "octet" / "netascii" / "mail"
    std::vector<TftpOption> options;  // also used by OACK

    // DATA / ACK
    std::uint16_t block_number{0};
    std::vector<std::uint8_t> data;   // DATA payload only

    // ERROR
    TftpErrorCode error_code{};
    std::string error_message;

    // Convenience lookups.
    const TftpOption* find_option(const std::string& name) const;
    bool has_option(const std::string& name) const {
        return find_option(name) != nullptr;
    }
};

// Result of a parse attempt. A malformed datagram yields ok=false with a
// human-readable reason; this is exactly what the G-series malformed-input
// tests assert on, so the reason strings are stable and specific.
struct ParseResult {
    bool ok{false};
    TftpPacket packet;
    std::string failure_reason;
    static ParseResult success(TftpPacket packet);
    static ParseResult failure(std::string reason);
};

// ASCII case-insensitive string equality (RFC 2347 option-name comparison).
bool equals_ignore_ascii_case(const std::string& left, const std::string& right);

// Parse a raw datagram into a TftpPacket. Never throws; malformed input is
// reported through ParseResult::ok == false.
ParseResult parse_tftp_packet(const std::uint8_t* data, std::size_t length);
inline ParseResult parse_tftp_packet(const std::vector<std::uint8_t>& datagram) {
    return parse_tftp_packet(datagram.data(), datagram.size());
}

// Lightweight opcode peek without a full parse: reads the first two bytes.
// Returns nullopt if fewer than two bytes are present.
std::optional<std::uint16_t> peek_opcode(const std::uint8_t* data,
                                         std::size_t length);
inline std::optional<std::uint16_t> peek_opcode(
    const std::vector<std::uint8_t>& datagram) {
    return peek_opcode(datagram.data(), datagram.size());
}

// Serializers. Each produces a ready-to-send datagram.
std::vector<std::uint8_t> serialize_read_request(
    const std::string& filename, const std::string& mode,
    const std::vector<TftpOption>& options = {});
std::vector<std::uint8_t> serialize_write_request(
    const std::string& filename, const std::string& mode,
    const std::vector<TftpOption>& options = {});
std::vector<std::uint8_t> serialize_data(
    std::uint16_t block_number, const std::uint8_t* payload,
    std::size_t payload_length);
std::vector<std::uint8_t> serialize_data(
    std::uint16_t block_number, const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> serialize_acknowledgement(std::uint16_t block_number);
std::vector<std::uint8_t> serialize_error(TftpErrorCode code,
                                          const std::string& message);
std::vector<std::uint8_t> serialize_option_acknowledgement(
    const std::vector<TftpOption>& options);

// Human-readable one-line summary used in traces and failure narratives, e.g.
// "DATA block=3 len=512" or "ERROR code=1 'File not found'".
std::string describe_packet(const TftpPacket& packet);

const char* opcode_name(TftpOpcode opcode);
const char* error_code_name(TftpErrorCode code);

} // namespace tftp_test_harness::net

#endif // TFTP_TEST_HARNESS_NET_TFTP_PACKET_HPP
