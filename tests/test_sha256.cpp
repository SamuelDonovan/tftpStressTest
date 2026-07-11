// SHA-256 known-answer tests (FIPS 180-4). If the vendored hash is wrong the
// content oracle's integrity verdicts are meaningless, so this is load-bearing.

#include "verify/sha256.hpp"
#include "test_support.hpp"

#include <string>
#include <vector>

using namespace tftp_test_harness::verify;

namespace {
std::vector<std::uint8_t> from_text(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}
} // namespace

TFTP_TEST_CASE(sha256_empty, "SHA-256 of empty input") {
    TFTP_CHECK_EQUAL(
        sha256_hex(from_text("")),
        std::string(
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

TFTP_TEST_CASE(sha256_abc, "SHA-256 of 'abc'") {
    TFTP_CHECK_EQUAL(
        sha256_hex(from_text("abc")),
        std::string(
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

TFTP_TEST_CASE(sha256_fox, "SHA-256 of the pangram") {
    TFTP_CHECK_EQUAL(
        sha256_hex(from_text("The quick brown fox jumps over the lazy dog")),
        std::string(
            "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"));
}

TFTP_TEST_CASE(sha256_two_block, "SHA-256 across a 56-byte padding boundary") {
    // 56 bytes forces a second padding block (message length field overflows the
    // first block), exercising the multi-block path.
    TFTP_CHECK_EQUAL(
        sha256_hex(from_text(
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
        std::string(
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

TFTP_TEST_CASE(sha256_incremental_equals_oneshot,
               "Incremental updates match a one-shot hash") {
    auto data = from_text("streamed-in-two-halves-must-match");
    Sha256 hasher;
    hasher.update(data.data(), 10);
    hasher.update(data.data() + 10, data.size() - 10);
    const auto streamed = to_hex(hasher.finalize());
    TFTP_CHECK_EQUAL(streamed, sha256_hex(data));
}

TFTP_TEST_MAIN()
