#ifndef TFTP_TEST_HARNESS_NET_NETASCII_HPP
#define TFTP_TEST_HARNESS_NET_NETASCII_HPP

// ---------------------------------------------------------------------------
// netascii end-of-line translation (RFC 1350 section 1 and Appendix, which
// defers to the Telnet NVT definition in RFC 764 / RFC 854).
//
// netascii represents an end of line as the two-octet sequence CR LF. A bare
// carriage return that is NOT part of an end of line is represented as the
// two-octet sequence CR NUL, so that a lone CR survives the round trip. On the
// wire every LF is preceded by CR, and every CR is followed by either LF or
// NUL.
//
// These functions convert between the host's local text representation (here we
// take the POSIX convention: LF is the line separator, a lone CR is just data)
// and the netascii wire form. Tests A-08 and A-09 exercise both directions,
// including files containing mixed CR / LF / CR-NUL sequences.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <vector>

namespace tftp_test_harness::net {

// Local (POSIX: LF-separated) bytes  ->  netascii wire bytes.
//   LF        -> CR LF
//   lone CR   -> CR NUL
//   CR LF     -> CR LF (already an EOL; preserved)
std::vector<std::uint8_t> encode_local_to_netascii(
    const std::vector<std::uint8_t>& local_bytes);

// netascii wire bytes  ->  local (POSIX: LF-separated) bytes.
//   CR LF  -> LF
//   CR NUL -> CR
//   CR <other> -> CR <other> (tolerated; malformed netascii in practice)
std::vector<std::uint8_t> decode_netascii_to_local(
    const std::vector<std::uint8_t>& netascii_bytes);

} // namespace tftp_test_harness::net

#endif // TFTP_TEST_HARNESS_NET_NETASCII_HPP
