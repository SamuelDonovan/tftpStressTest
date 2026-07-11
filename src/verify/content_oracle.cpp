#include "verify/content_oracle.hpp"

#include "net/netascii.hpp"
#include "verify/sha256.hpp"

#include <fstream>

namespace tftp_test_harness::verify {

namespace {

std::optional<std::vector<std::uint8_t>> read_file(
    const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
}

} // namespace

IntegrityResult compare_bytes(const std::vector<std::uint8_t>& source,
                              const std::vector<std::uint8_t>& delivered,
                              bool netascii) {
    // For netascii, the fidelity target is the netascii-normalized source: the
    // wire form canonicalizes EOLs, so decode(encode(x)) is the semantic
    // content a conformant transfer preserves.
    const std::vector<std::uint8_t> reference =
        netascii ? net::decode_netascii_to_local(
                       net::encode_local_to_netascii(source))
                 : source;

    IntegrityResult result;
    result.source_hash = sha256_hex(reference);
    result.delivered_hash = sha256_hex(delivered);
    result.source_size = reference.size();
    result.delivered_size = delivered.size();

    if (result.source_hash == result.delivered_hash) {
        result.verdict = IntegrityVerdict::Match;
        result.detail = "delivered bytes match the source fingerprint";
    } else {
        result.verdict = IntegrityVerdict::Mismatch;
        result.detail = "delivered bytes DIFFER from the source (" +
                        std::to_string(result.source_size) + " source bytes vs " +
                        std::to_string(result.delivered_size) +
                        " delivered bytes)";
    }
    return result;
}

IntegrityResult compare_files(const std::filesystem::path& source_path,
                              const std::filesystem::path& delivered_path,
                              bool netascii) {
    auto source = read_file(source_path);
    auto delivered = read_file(delivered_path);
    if (!source || !delivered) {
        IntegrityResult result;
        result.verdict = IntegrityVerdict::MissingArtifact;
        result.detail = !source ? "source artifact missing: " +
                                       source_path.string()
                                 : "delivered artifact missing: " +
                                       delivered_path.string();
        return result;
    }
    return compare_bytes(*source, *delivered, netascii);
}

} // namespace tftp_test_harness::verify
