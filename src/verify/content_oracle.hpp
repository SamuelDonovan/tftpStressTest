#ifndef TFTP_TEST_HARNESS_VERIFY_CONTENT_ORACLE_HPP
#define TFTP_TEST_HARNESS_VERIFY_CONTENT_ORACLE_HPP

// ---------------------------------------------------------------------------
// The content integrity oracle. After a transfer the harness compares the bytes
// that were supposed to move against the bytes that actually arrived, by
// SHA-256 fingerprint. The single most damning outcome in the whole report is a
// transfer the implementation *reported as successful* whose delivered bytes do
// not match the source — silent data corruption. This oracle is what detects it
// (severity CRITICAL, per the matrix).
//
// For netascii transfers the wire form canonicalizes end-of-line sequences, so
// a byte-for-byte comparison against the raw original is wrong. The oracle
// compares against the netascii-normalized original (decode(encode(original))),
// which is the semantic content a conformant netascii transfer must preserve.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tftp_test_harness::verify {

enum class IntegrityVerdict {
    Match,          // delivered bytes match the (normalized) source
    Mismatch,       // delivered bytes differ — data corruption
    MissingArtifact // one side's bytes were unavailable (e.g. no output file)
};

struct IntegrityResult {
    IntegrityVerdict verdict = IntegrityVerdict::MissingArtifact;
    std::string source_hash;    // hex SHA-256 of the (normalized) source
    std::string delivered_hash; // hex SHA-256 of the delivered bytes
    std::uint64_t source_size = 0;
    std::uint64_t delivered_size = 0;
    std::string detail;         // human-readable narrative

    bool matches() const { return verdict == IntegrityVerdict::Match; }
};

// Compare in-memory buffers. When `netascii` is true the source is normalized
// through the netascii codec before hashing.
IntegrityResult compare_bytes(const std::vector<std::uint8_t>& source,
                              const std::vector<std::uint8_t>& delivered,
                              bool netascii);

// Compare two files on disk. Returns MissingArtifact if either cannot be read.
IntegrityResult compare_files(const std::filesystem::path& source_path,
                              const std::filesystem::path& delivered_path,
                              bool netascii);

} // namespace tftp_test_harness::verify

#endif // TFTP_TEST_HARNESS_VERIFY_CONTENT_ORACLE_HPP
