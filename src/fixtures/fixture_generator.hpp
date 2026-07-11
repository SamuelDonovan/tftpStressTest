#ifndef TFTP_TEST_HARNESS_FIXTURES_FIXTURE_GENERATOR_HPP
#define TFTP_TEST_HARNESS_FIXTURES_FIXTURE_GENERATOR_HPP

// ---------------------------------------------------------------------------
// Deterministic fixture generation. Every fixture is produced from a fixed
// formula so a run reproduces byte-for-byte. The catalogue deliberately covers
// the size boundaries the matrix cares about — 0, 1, 511, 512, 513, exact
// multiples of the block size, MB-scale, and a file that crosses the 65535-block
// line — plus netascii content with mixed CR / LF / CR-NUL sequences (A-08/A-09).
// ---------------------------------------------------------------------------

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tftp_test_harness::fixtures {

enum class FixtureKind {
    Binary,   // octet-mode arbitrary bytes
    Netascii, // text with mixed end-of-line sequences
};

struct Fixture {
    std::string name;                  // filename within the served directory
    std::uint64_t size = 0;            // byte count
    FixtureKind kind = FixtureKind::Binary;
    std::string description;           // why this size matters
    std::filesystem::path path;        // absolute path once materialized
    std::vector<std::uint8_t> bytes;   // the exact content (kept for the oracle)
};

// Generate the standard fixture catalogue into `directory` (created if needed).
// `include_huge` controls whether the multi-megabyte and >65535-block fixtures
// are materialized (they are large and slow; the runner enables them only for
// the tests that need them).
std::vector<Fixture> generate_standard_fixtures(
    const std::filesystem::path& directory, bool include_huge = false);

// Produce a single deterministic binary buffer of the given size and seed.
std::vector<std::uint8_t> deterministic_binary(std::uint64_t size,
                                               std::uint32_t seed);

// Produce deterministic netascii-exercising text of approximately the given
// size, containing LF line endings, lone CR bytes, and CR LF pairs.
std::vector<std::uint8_t> deterministic_netascii_text(std::uint64_t approx_size);

// Materialize one fixture's bytes to disk under `directory`.
Fixture materialize(const std::filesystem::path& directory,
                    const std::string& name, FixtureKind kind,
                    std::vector<std::uint8_t> bytes,
                    const std::string& description);

} // namespace tftp_test_harness::fixtures

#endif // TFTP_TEST_HARNESS_FIXTURES_FIXTURE_GENERATOR_HPP
