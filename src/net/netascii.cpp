#include "net/netascii.hpp"

namespace tftp_test_harness::net {

namespace {
constexpr std::uint8_t carriage_return = 0x0D; // CR
constexpr std::uint8_t line_feed = 0x0A;       // LF
constexpr std::uint8_t nul = 0x00;             // NUL
} // namespace

std::vector<std::uint8_t> encode_local_to_netascii(
    const std::vector<std::uint8_t>& local_bytes) {
    std::vector<std::uint8_t> out;
    out.reserve(local_bytes.size() + local_bytes.size() / 8 + 1);
    for (std::size_t i = 0; i < local_bytes.size(); ++i) {
        const std::uint8_t byte = local_bytes[i];
        if (byte == line_feed) {
            // Local LF becomes the netascii end-of-line CR LF.
            out.push_back(carriage_return);
            out.push_back(line_feed);
        } else if (byte == carriage_return) {
            // A CR already followed by LF is a well-formed EOL: pass CR through
            // and let the LF be handled on the next iteration (it will emit
            // CR LF, i.e. the CR is duplicated) -- so instead we peek ahead and
            // preserve an existing CR LF verbatim.
            if (i + 1 < local_bytes.size() && local_bytes[i + 1] == line_feed) {
                out.push_back(carriage_return);
                out.push_back(line_feed);
                ++i; // consumed the LF as part of this EOL
            } else {
                // A lone CR must be encoded as CR NUL (RFC 1350 Appendix).
                out.push_back(carriage_return);
                out.push_back(nul);
            }
        } else {
            out.push_back(byte);
        }
    }
    return out;
}

std::vector<std::uint8_t> decode_netascii_to_local(
    const std::vector<std::uint8_t>& netascii_bytes) {
    std::vector<std::uint8_t> out;
    out.reserve(netascii_bytes.size());
    for (std::size_t i = 0; i < netascii_bytes.size(); ++i) {
        const std::uint8_t byte = netascii_bytes[i];
        if (byte == carriage_return && i + 1 < netascii_bytes.size()) {
            const std::uint8_t following = netascii_bytes[i + 1];
            if (following == line_feed) {
                out.push_back(line_feed); // CR LF -> local LF
                ++i;
                continue;
            }
            if (following == nul) {
                out.push_back(carriage_return); // CR NUL -> lone CR
                ++i;
                continue;
            }
            // Malformed netascii (CR not followed by LF or NUL). Tolerate it by
            // passing the CR through; the following byte is handled next loop.
            out.push_back(carriage_return);
            continue;
        }
        out.push_back(byte);
    }
    return out;
}

} // namespace tftp_test_harness::net
