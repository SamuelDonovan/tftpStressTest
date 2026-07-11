#include "net/tftp_packet.hpp"

#include <cctype>
#include <cstring>

namespace tftp_test_harness::net {

namespace {

// Append a big-endian 16-bit value (network byte order, RFC 1350 section 5).
void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void append_string_with_nul(std::vector<std::uint8_t>& out,
                            const std::string& text) {
    out.insert(out.end(), text.begin(), text.end());
    out.push_back(0);
}

std::uint16_t read_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}

// Read a NUL-terminated string starting at offset. On success advances offset
// past the terminator. Returns false if no terminator is found before the end
// (RFC 1350 requires every RRQ/WRQ string be NUL-terminated; a missing
// terminator is exactly the A-15/G-05 malformed case).
bool read_c_string(const std::uint8_t* data, std::size_t length,
                   std::size_t& offset, std::string& out) {
    const std::size_t start = offset;
    while (offset < length && data[offset] != 0) {
        ++offset;
    }
    if (offset >= length) {
        return false; // ran off the end without a NUL terminator
    }
    out.assign(reinterpret_cast<const char*>(data + start), offset - start);
    ++offset; // consume the NUL
    return true;
}

} // namespace

bool equals_ignore_ascii_case(const std::string& left,
                              const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(left[i]);
        const unsigned char b = static_cast<unsigned char>(right[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

const TftpOption* TftpPacket::find_option(const std::string& name) const {
    for (const auto& option : options) {
        if (equals_ignore_ascii_case(option.name, name)) {
            return &option;
        }
    }
    return nullptr;
}

ParseResult ParseResult::success(TftpPacket packet) {
    ParseResult result;
    result.ok = true;
    result.packet = std::move(packet);
    return result;
}

ParseResult ParseResult::failure(std::string reason) {
    ParseResult result;
    result.ok = false;
    result.failure_reason = std::move(reason);
    return result;
}

std::optional<std::uint16_t> peek_opcode(const std::uint8_t* data,
                                         std::size_t length) {
    if (data == nullptr || length < 2) {
        return std::nullopt;
    }
    return read_u16(data);
}

namespace {

// Parse the trailing (name\0value\0)* option list shared by RRQ/WRQ and OACK.
bool parse_option_list(const std::uint8_t* data, std::size_t length,
                       std::size_t offset, std::vector<TftpOption>& out,
                       std::string& failure_reason) {
    while (offset < length) {
        TftpOption option;
        if (!read_c_string(data, length, offset, option.name)) {
            failure_reason = "option name not NUL-terminated";
            return false;
        }
        if (!read_c_string(data, length, offset, option.value)) {
            failure_reason = "option value not NUL-terminated";
            return false;
        }
        if (option.name.empty()) {
            failure_reason = "empty option name";
            return false;
        }
        out.push_back(std::move(option));
    }
    return true;
}

} // namespace

ParseResult parse_tftp_packet(const std::uint8_t* data, std::size_t length) {
    if (data == nullptr || length < 2) {
        return ParseResult::failure(
            "datagram shorter than the 2-byte opcode field");
    }
    const std::uint16_t raw_opcode = read_u16(data);
    TftpPacket packet;

    switch (raw_opcode) {
        case static_cast<std::uint16_t>(TftpOpcode::ReadRequest):
        case static_cast<std::uint16_t>(TftpOpcode::WriteRequest): {
            packet.opcode = static_cast<TftpOpcode>(raw_opcode);
            std::size_t offset = 2;
            if (!read_c_string(data, length, offset, packet.filename)) {
                return ParseResult::failure(
                    "RRQ/WRQ filename not NUL-terminated");
            }
            if (!read_c_string(data, length, offset, packet.mode)) {
                return ParseResult::failure("RRQ/WRQ mode not NUL-terminated");
            }
            std::string reason;
            if (!parse_option_list(data, length, offset, packet.options,
                                   reason)) {
                return ParseResult::failure("RRQ/WRQ " + reason);
            }
            return ParseResult::success(std::move(packet));
        }

        case static_cast<std::uint16_t>(TftpOpcode::Data): {
            if (length < 4) {
                return ParseResult::failure(
                    "DATA shorter than the 4-byte header");
            }
            packet.opcode = TftpOpcode::Data;
            packet.block_number = read_u16(data + 2);
            packet.data.assign(data + 4, data + length);
            return ParseResult::success(std::move(packet));
        }

        case static_cast<std::uint16_t>(TftpOpcode::Acknowledgement): {
            if (length < 4) {
                return ParseResult::failure("ACK shorter than 4 bytes");
            }
            packet.opcode = TftpOpcode::Acknowledgement;
            packet.block_number = read_u16(data + 2);
            // Trailing bytes beyond the 4-byte ACK are ignored per common
            // practice; a strict harness records the length elsewhere.
            return ParseResult::success(std::move(packet));
        }

        case static_cast<std::uint16_t>(TftpOpcode::Error): {
            if (length < 4) {
                return ParseResult::failure(
                    "ERROR shorter than the 4-byte header");
            }
            packet.opcode = TftpOpcode::Error;
            packet.error_code = static_cast<TftpErrorCode>(read_u16(data + 2));
            std::size_t offset = 4;
            if (!read_c_string(data, length, offset, packet.error_message)) {
                return ParseResult::failure(
                    "ERROR message not NUL-terminated");
            }
            return ParseResult::success(std::move(packet));
        }

        case static_cast<std::uint16_t>(TftpOpcode::OptionAcknowledgement): {
            packet.opcode = TftpOpcode::OptionAcknowledgement;
            std::string reason;
            if (!parse_option_list(data, length, 2, packet.options, reason)) {
                return ParseResult::failure("OACK " + reason);
            }
            return ParseResult::success(std::move(packet));
        }

        default:
            return ParseResult::failure(
                "unknown opcode " + std::to_string(raw_opcode));
    }
}

std::vector<std::uint8_t> serialize_read_request(
    const std::string& filename, const std::string& mode,
    const std::vector<TftpOption>& options) {
    std::vector<std::uint8_t> out;
    append_u16(out, static_cast<std::uint16_t>(TftpOpcode::ReadRequest));
    append_string_with_nul(out, filename);
    append_string_with_nul(out, mode);
    for (const auto& option : options) {
        append_string_with_nul(out, option.name);
        append_string_with_nul(out, option.value);
    }
    return out;
}

std::vector<std::uint8_t> serialize_write_request(
    const std::string& filename, const std::string& mode,
    const std::vector<TftpOption>& options) {
    std::vector<std::uint8_t> out;
    append_u16(out, static_cast<std::uint16_t>(TftpOpcode::WriteRequest));
    append_string_with_nul(out, filename);
    append_string_with_nul(out, mode);
    for (const auto& option : options) {
        append_string_with_nul(out, option.name);
        append_string_with_nul(out, option.value);
    }
    return out;
}

std::vector<std::uint8_t> serialize_data(std::uint16_t block_number,
                                         const std::uint8_t* payload,
                                         std::size_t payload_length) {
    std::vector<std::uint8_t> out;
    out.reserve(4 + payload_length);
    append_u16(out, static_cast<std::uint16_t>(TftpOpcode::Data));
    append_u16(out, block_number);
    if (payload != nullptr && payload_length > 0) {
        out.insert(out.end(), payload, payload + payload_length);
    }
    return out;
}

std::vector<std::uint8_t> serialize_data(
    std::uint16_t block_number, const std::vector<std::uint8_t>& payload) {
    return serialize_data(block_number, payload.data(), payload.size());
}

std::vector<std::uint8_t> serialize_acknowledgement(std::uint16_t block_number) {
    std::vector<std::uint8_t> out;
    append_u16(out, static_cast<std::uint16_t>(TftpOpcode::Acknowledgement));
    append_u16(out, block_number);
    return out;
}

std::vector<std::uint8_t> serialize_error(TftpErrorCode code,
                                          const std::string& message) {
    std::vector<std::uint8_t> out;
    append_u16(out, static_cast<std::uint16_t>(TftpOpcode::Error));
    append_u16(out, static_cast<std::uint16_t>(code));
    append_string_with_nul(out, message);
    return out;
}

std::vector<std::uint8_t> serialize_option_acknowledgement(
    const std::vector<TftpOption>& options) {
    std::vector<std::uint8_t> out;
    append_u16(out,
               static_cast<std::uint16_t>(TftpOpcode::OptionAcknowledgement));
    for (const auto& option : options) {
        append_string_with_nul(out, option.name);
        append_string_with_nul(out, option.value);
    }
    return out;
}

const char* opcode_name(TftpOpcode opcode) {
    switch (opcode) {
        case TftpOpcode::ReadRequest: return "RRQ";
        case TftpOpcode::WriteRequest: return "WRQ";
        case TftpOpcode::Data: return "DATA";
        case TftpOpcode::Acknowledgement: return "ACK";
        case TftpOpcode::Error: return "ERROR";
        case TftpOpcode::OptionAcknowledgement: return "OACK";
    }
    return "UNKNOWN";
}

const char* error_code_name(TftpErrorCode code) {
    switch (code) {
        case TftpErrorCode::NotDefined: return "not defined";
        case TftpErrorCode::FileNotFound: return "file not found";
        case TftpErrorCode::AccessViolation: return "access violation";
        case TftpErrorCode::DiskFull: return "disk full / allocation exceeded";
        case TftpErrorCode::IllegalOperation: return "illegal TFTP operation";
        case TftpErrorCode::UnknownTransferId: return "unknown transfer ID";
        case TftpErrorCode::FileAlreadyExists: return "file already exists";
        case TftpErrorCode::NoSuchUser: return "no such user";
        case TftpErrorCode::OptionNegotiationFailed:
            return "option negotiation failed";
    }
    return "unknown error code";
}

std::string describe_packet(const TftpPacket& packet) {
    std::string text = opcode_name(packet.opcode);
    switch (packet.opcode) {
        case TftpOpcode::ReadRequest:
        case TftpOpcode::WriteRequest:
            text += " file='" + packet.filename + "' mode=" + packet.mode;
            if (!packet.options.empty()) {
                text += " opts=[";
                for (std::size_t i = 0; i < packet.options.size(); ++i) {
                    if (i != 0) text += ",";
                    text += packet.options[i].name + "=" +
                            packet.options[i].value;
                }
                text += "]";
            }
            break;
        case TftpOpcode::Data:
            text += " block=" + std::to_string(packet.block_number) +
                    " len=" + std::to_string(packet.data.size());
            break;
        case TftpOpcode::Acknowledgement:
            text += " block=" + std::to_string(packet.block_number);
            break;
        case TftpOpcode::Error:
            text += " code=" +
                    std::to_string(
                        static_cast<std::uint16_t>(packet.error_code)) +
                    " '" + packet.error_message + "'";
            break;
        case TftpOpcode::OptionAcknowledgement:
            text += " opts=[";
            for (std::size_t i = 0; i < packet.options.size(); ++i) {
                if (i != 0) text += ",";
                text += packet.options[i].name + "=" + packet.options[i].value;
            }
            text += "]";
            break;
    }
    return text;
}

} // namespace tftp_test_harness::net
