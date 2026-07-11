#ifndef TFTP_TEST_HARNESS_VERIFY_SHA256_HPP
#define TFTP_TEST_HARNESS_VERIFY_SHA256_HPP

// ---------------------------------------------------------------------------
// A vendored, self-contained SHA-256 (FIPS 180-4). The content oracle needs a
// strong hash to compare source and delivered bytes, and the "standard library
// only / no third-party dependency" constraint rules out linking a crypto
// library. Rather than reach for an OS crypto API (which varies across the
// constrained/air-gapped targets the harness must build on), we vendor this
// small public-domain implementation and document it here.
//
// This is used only as a content fingerprint for integrity comparison; it is
// not used for any security purpose.
// ---------------------------------------------------------------------------

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tftp_test_harness::verify {

class Sha256 {
public:
    Sha256() { reset(); }

    void reset();
    void update(const std::uint8_t* data, std::size_t length);
    void update(const std::vector<std::uint8_t>& data) {
        update(data.data(), data.size());
    }

    // Finalize and return the 32-byte digest. The object must be reset() before
    // reuse.
    std::array<std::uint8_t, 32> finalize();

private:
    void process_block(const std::uint8_t* block);

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::uint64_t total_length_ = 0;
    std::size_t buffer_length_ = 0;
};

// Convenience: hash a whole buffer and return the lowercase hex digest.
std::string sha256_hex(const std::vector<std::uint8_t>& data);
std::array<std::uint8_t, 32> sha256_digest(const std::vector<std::uint8_t>& data);
std::string to_hex(const std::array<std::uint8_t, 32>& digest);

} // namespace tftp_test_harness::verify

#endif // TFTP_TEST_HARNESS_VERIFY_SHA256_HPP
