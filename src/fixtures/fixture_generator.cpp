#include "fixtures/fixture_generator.hpp"

#include <fstream>

namespace tftp_test_harness::fixtures {

std::vector<std::uint8_t> deterministic_binary(std::uint64_t size,
                                               std::uint32_t seed) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(size));
    // A linear congruential stream: fully reproducible, cheap, and non-trivial
    // enough that a single flipped byte changes the SHA-256 fingerprint.
    std::uint32_t state = seed * 2654435761u + 1013904223u;
    for (std::uint64_t i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        bytes.push_back(static_cast<std::uint8_t>(state >> 24));
    }
    return bytes;
}

std::vector<std::uint8_t> deterministic_netascii_text(
    std::uint64_t approx_size) {
    // Cycle through content that stresses every netascii translation rule:
    // ordinary text, an LF line ending, a bare CR (encodes as CR NUL), and an
    // explicit CR LF pair (a well-formed end of line).
    static const char* fragments[] = {
        "The quick brown fox", "\n", "carriage\rreturn-in-the-middle",
        "\r\n", "another line of netascii content", "\n",
        "tab\tand more words", "\n"};
    std::vector<std::uint8_t> bytes;
    std::size_t fragment_index = 0;
    while (bytes.size() < approx_size) {
        const char* fragment = fragments[fragment_index % 8];
        for (const char* p = fragment; *p != '\0'; ++p) {
            bytes.push_back(static_cast<std::uint8_t>(*p));
        }
        ++fragment_index;
    }
    return bytes;
}

Fixture materialize(const std::filesystem::path& directory,
                    const std::string& name, FixtureKind kind,
                    std::vector<std::uint8_t> bytes,
                    const std::string& description) {
    std::filesystem::create_directories(directory);
    Fixture fixture;
    fixture.name = name;
    fixture.kind = kind;
    fixture.description = description;
    fixture.size = bytes.size();
    fixture.path = directory / name;
    fixture.bytes = std::move(bytes);

    std::ofstream stream(fixture.path, std::ios::binary | std::ios::trunc);
    if (!fixture.bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(fixture.bytes.data()),
                     static_cast<std::streamsize>(fixture.bytes.size()));
    }
    return fixture;
}

std::vector<Fixture> generate_standard_fixtures(
    const std::filesystem::path& directory, bool include_huge) {
    std::filesystem::create_directories(directory);
    std::vector<Fixture> fixtures;

    struct SizeSpec {
        const char* name;
        std::uint64_t size;
        const char* description;
    };
    // Boundary sizes around the 512-byte default block (RFC 1350 section 2).
    const SizeSpec specs[] = {
        {"empty.bin", 0, "zero-byte file (A-03): single final DATA of length 0"},
        {"one.bin", 1, "single byte"},
        {"just_under_block.bin", 511, "one byte under the block size (A-06)"},
        {"exact_block.bin", 512,
         "exactly one block (A-04): requires a terminating 0-length DATA"},
        {"just_over_block.bin", 513, "one byte over the block size (A-06)"},
        {"two_blocks_exact.bin", 1024,
         "exact multiple of the block size (A-05)"},
        {"odd_medium.bin", 3000, "several blocks, non-multiple size"},
        // Deliberately NOT exact multiples of 512, so that the terminating
        // short-block always exists — this isolates the final-block rule test
        // (A-04/A-05) from the impairment and injection tests that reuse these.
        {"kib_8.bin", 8000, "~8 KB, 16 blocks (adversarial sweeps)"},
        {"kib_64.bin", 65000, "~64 KB, 127 blocks (large multi-block)"},
    };
    std::uint32_t seed = 1;
    for (const auto& spec : specs) {
        fixtures.push_back(materialize(directory, spec.name,
                                       FixtureKind::Binary,
                                       deterministic_binary(spec.size, seed++),
                                       spec.description));
    }

    fixtures.push_back(materialize(
        directory, "netascii_mixed.txt", FixtureKind::Netascii,
        deterministic_netascii_text(4096),
        "mixed CR / LF / CR-NUL netascii content (A-08/A-09)"));

    if (include_huge) {
        // Multi-megabyte (A-07) and a file that crosses the 65535-block line
        // (A-40 / H-02): > 32 MiB at 512-byte blocks.
        fixtures.push_back(materialize(
            directory, "multi_megabyte.bin", FixtureKind::Binary,
            deterministic_binary(4 * 1024 * 1024, 101),
            "4 MiB multi-block transfer (A-07)"));
        fixtures.push_back(materialize(
            directory, "over_65535_blocks.bin", FixtureKind::Binary,
            deterministic_binary(
                static_cast<std::uint64_t>(65536) * 512 + 1024, 202),
            "crosses the 65535-block boundary (A-40 / H-02)"));
    }

    return fixtures;
}

} // namespace tftp_test_harness::fixtures
