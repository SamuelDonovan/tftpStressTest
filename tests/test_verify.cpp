// Tests for the verification layer: the content integrity oracle and the
// packet-level observer. The decisive cases drive the deliberately buggy
// reference engine through the proxy and assert the observer flags exactly its
// injected defects, while the correct engine passes — a preview of the full
// self-verification.

#include "net/impairment_proxy.hpp"
#include "reference/reference_adapters.hpp"
#include "reference/tftp_reference_engine.hpp"
#include "verify/content_oracle.hpp"
#include "verify/packet_observer.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace tftp_test_harness;
using namespace tftp_test_harness::net;
using namespace tftp_test_harness::reference;
using namespace tftp_test_harness::verify;

namespace {

std::filesystem::path make_temp_directory(const std::string& tag) {
    auto base = std::filesystem::temp_directory_path() /
                ("tftp_verify_test_" + tag + "_" + std::to_string(::getpid()) +
                 "_" + std::to_string(std::rand()));
    std::filesystem::create_directories(base);
    return base;
}

std::vector<std::uint8_t> bytes_of(std::size_t count, std::uint8_t value) {
    return std::vector<std::uint8_t>(count, value);
}

EngineConfig fast(EngineConfig config) {
    config.retransmission_timeout = std::chrono::milliseconds(200);
    config.maximum_retransmissions = 12;
    return config;
}

// Drive a download of `served` bytes through the proxy using the given engine
// config; return the observer report and whether the app reported success.
struct RunResult {
    ObserverReport report;
    bool app_reported_complete = false;
    IntegrityResult integrity;
};

RunResult run_download(const std::vector<std::uint8_t>& served,
                       EngineConfig server_config, EngineConfig client_config,
                       const ClientRequestedOptions& options,
                       const ImpairmentConfig& impairment, std::uint64_t seed,
                       InjectionScript injection = nullptr) {
    auto directory = make_temp_directory("dl");
    write_entire_file(directory / "payload.bin", served);

    ReferenceServer server(server_config);
    const Endpoint server_endpoint = server.start(directory, "127.0.0.1");

    ImpairmentProxy proxy;
    ImpairmentProxy::Config proxy_config;
    proxy_config.server_endpoint = server_endpoint;
    proxy_config.impairment = impairment;
    proxy_config.seed = seed;
    proxy_config.injection = std::move(injection);
    proxy.start(proxy_config);

    ReferenceClient client(client_config);
    const auto destination = directory / "out.bin";
    TransferOutcome outcome = client.read_file(
        proxy.listen_endpoint(), "payload.bin", destination, "octet", options);

    PacketTrace trace = proxy.stop_and_take_trace();
    server.stop();

    ObserverContext context;
    context.direction = TransferDirection::Download;
    context.block_size = options.block_size.value_or(512);
    context.transfer_reported_complete = outcome.completed_successfully;

    RunResult result;
    result.report = observe(trace, context);
    result.app_reported_complete = outcome.completed_successfully;
    auto delivered = reference::read_entire_file(destination);
    result.integrity = compare_bytes(served, delivered.value_or(std::vector<std::uint8_t>{}),
                                     false);
    std::filesystem::remove_all(directory);
    return result;
}

} // namespace

TFTP_TEST_CASE(oracle_detects_match, "Content oracle confirms identical bytes") {
    auto data = bytes_of(1000, 0x5A);
    auto result = compare_bytes(data, data, false);
    TFTP_CHECK_TRUE(result.matches());
}

TFTP_TEST_CASE(oracle_detects_mismatch, "Content oracle flags a single-bit change") {
    auto source = bytes_of(1000, 0x5A);
    auto delivered = source;
    delivered[500] ^= 0x01;
    auto result = compare_bytes(source, delivered, false);
    TFTP_CHECK_FALSE(result.matches());
    TFTP_CHECK_TRUE(result.verdict == IntegrityVerdict::Mismatch);
}

TFTP_TEST_CASE(observer_clean_download,
               "Correct engine: clean download has no retransmissions, fresh TID") {
    auto served = bytes_of(20000, 0xC3);
    ClientRequestedOptions none;
    auto result = run_download(served, fast(make_engine_config(
                                             ReferencePersonality::Correct)),
                               fast(make_engine_config(
                                   ReferencePersonality::Correct)),
                               none, ImpairmentConfig{}, 1);
    TFTP_CHECK_TRUE(result.app_reported_complete);
    TFTP_CHECK_TRUE(result.integrity.matches());
    // Clean channel: no drops, so no legitimate retransmissions.
    TFTP_CHECK_EQUAL(result.report.retransmitted_data_packets, std::uint32_t(0));
    TFTP_CHECK_TRUE(result.report.terminating_short_block_present);
    TFTP_CHECK_TRUE(result.report.server_selected_fresh_tid);
}

TFTP_TEST_CASE(observer_flags_broken_final_block,
               "Buggy engine omits terminating block on exact-multiple (A-04/A-05)") {
    // Exactly 8 blocks of 512 bytes = 4096, an exact multiple. A correct sender
    // appends a terminating zero-length DATA; the buggy engine omits it.
    auto served = bytes_of(4096, 0x11);
    ClientRequestedOptions none;

    auto correct = run_download(
        served, fast(make_engine_config(ReferencePersonality::Correct)),
        fast(make_engine_config(ReferencePersonality::Correct)), none,
        ImpairmentConfig{}, 2);
    TFTP_CHECK_TRUE(correct.report.terminating_short_block_present);

    auto buggy = run_download(
        served, fast(make_engine_config(ReferencePersonality::Buggy)),
        fast(make_engine_config(ReferencePersonality::Correct)), none,
        ImpairmentConfig{}, 2);
    // The buggy server never emits a DATA block shorter than the block size:
    // the final-block rule is violated (RFC 1350 section 2).
    TFTP_CHECK_FALSE(buggy.report.terminating_short_block_present);
}

TFTP_TEST_CASE(observer_flags_sorcerers_apprentice,
               "Buggy engine retransmits on duplicate ACK (A-32/A-33)") {
    // Inject a duplicate ACK each time the server sends DATA, on a clean channel
    // (no loss). A correct sender ignores duplicate ACKs and never retransmits;
    // the SAS-vulnerable buggy sender retransmits, amplifying the DATA count.
    auto injection = [](const TraceRecord& observed) {
        std::vector<InjectedPacket> result;
        if (observed.direction == Direction::ServerToClient &&
            observed.is_data() && observed.disposition != TraceDisposition::Injected) {
            // Re-send an ACK for the block just seen, as if the client duplicated
            // it. Sent from the legitimate client TID so the server accepts it.
            InjectedPacket packet;
            packet.bytes = serialize_acknowledgement(observed.packet.block_number);
            packet.toward_client = false; // toward the server
            packet.from_stray_tid = false; // as the client's TID
            packet.description = "duplicate ACK (SAS probe)";
            result.push_back(std::move(packet));
        }
        return result;
    };

    auto served = bytes_of(8000, 0x22);
    ClientRequestedOptions none;

    auto correct = run_download(
        served, fast(make_engine_config(ReferencePersonality::Correct)),
        fast(make_engine_config(ReferencePersonality::Correct)), none,
        ImpairmentConfig{}, 3, injection);
    // A correct sender does not retransmit in response to duplicate ACKs.
    TFTP_CHECK_EQUAL(correct.report.retransmitted_data_packets,
                     std::uint32_t(0));

    auto buggy = run_download(
        served, fast(make_engine_config(ReferencePersonality::Buggy)),
        fast(make_engine_config(ReferencePersonality::Correct)), none,
        ImpairmentConfig{}, 3, injection);
    // The SAS-vulnerable sender retransmits on each duplicate ACK.
    TFTP_CHECK_TRUE(buggy.report.retransmitted_data_packets > 0);
}

TFTP_TEST_CASE(observer_stray_tid_rebuffed,
               "Stray-TID datagram is answered ERROR 5, transfer survives (A-21)") {
    // Inject a spoofed packet from a stray TID toward the client once, mid
    // transfer. The correct client must reply ERROR 5 to the stray source and
    // finish the transfer intact.
    bool injected_once = false;
    auto injection = [injected_once](const TraceRecord& observed) mutable {
        std::vector<InjectedPacket> result;
        if (!injected_once && observed.direction == Direction::ServerToClient &&
            observed.is_data() && observed.disposition != TraceDisposition::Injected) {
            injected_once = true;
            InjectedPacket packet;
            packet.bytes = serialize_error(TftpErrorCode::NotDefined,
                                           "spoofed error from stray TID");
            packet.toward_client = true;
            packet.from_stray_tid = true; // unexpected TID
            packet.description = "spoofed ERROR from stray TID";
            result.push_back(std::move(packet));
        }
        return result;
    };

    auto served = bytes_of(12000, 0x33);
    ClientRequestedOptions none;
    auto result = run_download(
        served, fast(make_engine_config(ReferencePersonality::Correct)),
        fast(make_engine_config(ReferencePersonality::Correct)), none,
        ImpairmentConfig{}, 4, injection);

    // The transfer completed byte-exact despite the injected stray ERROR, and
    // the client rebuffed the stray TID with ERROR 5 (A-21/A-22).
    TFTP_CHECK_TRUE(result.app_reported_complete);
    TFTP_CHECK_TRUE(result.integrity.matches());
    TFTP_CHECK_TRUE(result.report.stray_tid_replies > 0);
    TFTP_CHECK_TRUE(result.report.stray_tid_reply_was_error_5);
}

TFTP_TEST_MAIN()
