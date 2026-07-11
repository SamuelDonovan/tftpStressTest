// End-to-end tests of the in-process reference engine: a correct client and a
// correct server talking to each other directly over loopback (no proxy yet).
// These prove the engine is a faithful TFTP peer across file sizes, transfer
// directions, options, and netascii translation before the proxy and observer
// are layered on top.

#include "net/netascii.hpp"
#include "reference/tftp_reference_engine.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace tftp_test_harness;
using namespace tftp_test_harness::reference;
using namespace tftp_test_harness::net;

namespace {

std::filesystem::path make_temp_directory(const std::string& tag) {
    auto base = std::filesystem::temp_directory_path() /
                ("tftp_engine_test_" + tag + "_" +
                 std::to_string(::getpid()) + "_" +
                 std::to_string(std::rand()));
    std::filesystem::create_directories(base);
    return base;
}

std::vector<std::uint8_t> deterministic_bytes(std::size_t count,
                                              std::uint32_t seed) {
    std::vector<std::uint8_t> bytes(count);
    std::uint32_t state = seed * 2654435761u + 1u;
    for (std::size_t i = 0; i < count; ++i) {
        state = state * 1664525u + 1013904223u;
        bytes[i] = static_cast<std::uint8_t>(state >> 24);
    }
    return bytes;
}

EngineConfig fast_config() {
    EngineConfig config;
    config.retransmission_timeout = std::chrono::milliseconds(300);
    return config;
}

// Run one RRQ (server -> client download) and assert byte-exact delivery.
void expect_download_roundtrip(std::size_t file_size, std::uint32_t seed,
                               const ClientRequestedOptions& options) {
    auto directory = make_temp_directory("rrq");
    const auto source_bytes = deterministic_bytes(file_size, seed);
    TFTP_CHECK_TRUE(write_entire_file(directory / "payload.bin", source_bytes));

    ReferenceServer server(fast_config());
    const Endpoint endpoint = server.start(directory, "127.0.0.1");
    TFTP_CHECK_TRUE(endpoint.valid());

    ReferenceClient client(fast_config());
    const auto destination = directory / "download.out";
    TransferOutcome outcome = client.read_file(endpoint, "payload.bin",
                                               destination, "octet", options);
    server.stop();

    TFTP_CHECK_TRUE(outcome.completed_successfully);
    auto delivered = read_entire_file(destination);
    TFTP_CHECK_TRUE(delivered.has_value());
    TFTP_CHECK_TRUE(*delivered == source_bytes);
    std::filesystem::remove_all(directory);
}

// Run one WRQ (client -> server upload) and assert byte-exact delivery.
void expect_upload_roundtrip(std::size_t file_size, std::uint32_t seed,
                             const ClientRequestedOptions& options) {
    auto directory = make_temp_directory("wrq");
    auto served = directory / "served";
    std::filesystem::create_directories(served);
    const auto source_bytes = deterministic_bytes(file_size, seed);
    const auto source_path = directory / "upload.bin";
    TFTP_CHECK_TRUE(write_entire_file(source_path, source_bytes));

    ReferenceServer server(fast_config());
    const Endpoint endpoint = server.start(served, "127.0.0.1");

    ReferenceClient client(fast_config());
    TransferOutcome outcome = client.write_file(endpoint, source_path,
                                                "stored.bin", "octet", options);
    server.stop();

    TFTP_CHECK_TRUE(outcome.completed_successfully);
    auto delivered = read_entire_file(served / "stored.bin");
    TFTP_CHECK_TRUE(delivered.has_value());
    TFTP_CHECK_TRUE(*delivered == source_bytes);
    std::filesystem::remove_all(directory);
}

} // namespace

TFTP_TEST_CASE(download_boundary_sizes, "RRQ byte-exact at block boundaries") {
    ClientRequestedOptions none;
    // 0, 1, 511, 512 (A-04), 513, exact multiple, and a couple KB.
    for (std::size_t size : {std::size_t(0), std::size_t(1), std::size_t(511),
                             std::size_t(512), std::size_t(513),
                             std::size_t(1024), std::size_t(2000)}) {
        expect_download_roundtrip(size, static_cast<std::uint32_t>(size), none);
    }
}

TFTP_TEST_CASE(upload_boundary_sizes, "WRQ byte-exact at block boundaries") {
    ClientRequestedOptions none;
    for (std::size_t size : {std::size_t(0), std::size_t(1), std::size_t(512),
                             std::size_t(1024), std::size_t(3000)}) {
        expect_upload_roundtrip(size, static_cast<std::uint32_t>(size) + 7,
                                none);
    }
}

TFTP_TEST_CASE(download_large_file, "RRQ of a multi-block file (A-07)") {
    ClientRequestedOptions none;
    expect_download_roundtrip(256 * 1024, 99, none); // 256 KiB, 512 blocks
}

TFTP_TEST_CASE(download_with_blksize, "RRQ honors negotiated blksize (C-03)") {
    ClientRequestedOptions options;
    options.block_size = 1428;
    expect_download_roundtrip(10000, 3, options);
}

TFTP_TEST_CASE(download_with_small_blksize, "RRQ with blksize 8 (C-02)") {
    ClientRequestedOptions options;
    options.block_size = 8;
    expect_download_roundtrip(100, 5, options);
}

TFTP_TEST_CASE(download_with_windowsize, "RRQ honors windowsize (E-02)") {
    for (std::uint16_t window : {std::uint16_t(4), std::uint16_t(16),
                                 std::uint16_t(64)}) {
        ClientRequestedOptions options;
        options.window_size = window;
        expect_download_roundtrip(50000, window, options);
    }
}

TFTP_TEST_CASE(upload_with_windowsize, "WRQ honors windowsize (E-02)") {
    ClientRequestedOptions options;
    options.window_size = 8;
    expect_upload_roundtrip(40000, 11, options);
}

TFTP_TEST_CASE(download_with_tsize, "RRQ tsize=0 returns file size (D-03)") {
    ClientRequestedOptions options;
    options.transfer_size = 0;
    auto directory = make_temp_directory("tsize");
    const auto source_bytes = deterministic_bytes(1234, 7);
    write_entire_file(directory / "sized.bin", source_bytes);

    ReferenceServer server(fast_config());
    const Endpoint endpoint = server.start(directory, "127.0.0.1");
    ReferenceClient client(fast_config());
    TransferOutcome outcome = client.read_file(
        endpoint, "sized.bin", directory / "out.bin", "octet", options);
    server.stop();

    TFTP_CHECK_TRUE(outcome.completed_successfully);
    TFTP_CHECK_TRUE(outcome.negotiated.transfer_size.has_value());
    TFTP_CHECK_EQUAL(*outcome.negotiated.transfer_size, std::uint64_t(1234));
    std::filesystem::remove_all(directory);
}

TFTP_TEST_CASE(file_not_found_error, "RRQ of missing file yields ERROR 1 (A-11)") {
    auto directory = make_temp_directory("missing");
    ReferenceServer server(fast_config());
    const Endpoint endpoint = server.start(directory, "127.0.0.1");
    ReferenceClient client(fast_config());
    ClientRequestedOptions none;
    TransferOutcome outcome = client.read_file(
        endpoint, "nope.bin", directory / "out.bin", "octet", none);
    server.stop();

    TFTP_CHECK_FALSE(outcome.completed_successfully);
    TFTP_CHECK_TRUE(outcome.error_code.has_value());
    TFTP_CHECK_TRUE(*outcome.error_code == TftpErrorCode::FileNotFound);
    std::filesystem::remove_all(directory);
}

TFTP_TEST_CASE(overwrite_rejected, "WRQ to existing file yields ERROR 6 (A-12)") {
    auto directory = make_temp_directory("exists");
    auto served = directory / "served";
    std::filesystem::create_directories(served);
    write_entire_file(served / "there.bin", deterministic_bytes(10, 1));
    const auto source_path = directory / "src.bin";
    write_entire_file(source_path, deterministic_bytes(20, 2));

    ReferenceServer server(fast_config());
    const Endpoint endpoint = server.start(served, "127.0.0.1");
    ReferenceClient client(fast_config());
    ClientRequestedOptions none;
    TransferOutcome outcome = client.write_file(endpoint, source_path,
                                                "there.bin", "octet", none);
    server.stop();

    TFTP_CHECK_FALSE(outcome.completed_successfully);
    TFTP_CHECK_TRUE(outcome.error_code.has_value());
    TFTP_CHECK_TRUE(*outcome.error_code == TftpErrorCode::FileAlreadyExists);
    std::filesystem::remove_all(directory);
}

TFTP_TEST_CASE(netascii_roundtrip, "netascii RRQ round-trips EOLs (A-08/A-09)") {
    auto directory = make_temp_directory("netascii");
    // Mixed content: LF lines, a lone CR, and an existing CRLF.
    std::string text = "line one\nline two\rmid-cr\r\ncrlf-line\nlast";
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    write_entire_file(directory / "text.txt", bytes);

    ReferenceServer server(fast_config());
    const Endpoint endpoint = server.start(directory, "127.0.0.1");
    ReferenceClient client(fast_config());
    ClientRequestedOptions none;
    TransferOutcome outcome = client.read_file(
        endpoint, "text.txt", directory / "text.out", "netascii", none);
    server.stop();

    TFTP_CHECK_TRUE(outcome.completed_successfully);
    auto delivered = read_entire_file(directory / "text.out");
    TFTP_CHECK_TRUE(delivered.has_value());
    // netascii canonicalizes EOLs, so the fidelity check is against the
    // normalized original (decode of encode), which is what the content oracle
    // will also use.
    auto normalized = decode_netascii_to_local(encode_local_to_netascii(bytes));
    TFTP_CHECK_TRUE(*delivered == normalized);
    std::filesystem::remove_all(directory);
}

TFTP_TEST_MAIN()
