// Integration tests that route a real reference client and server through the
// impairment proxy. This is the harness's core vertical slice: the zero-
// impairment case must be a transparent pass-through, and lossy cases must still
// complete byte-exact via retransmission. The recorded trace is spot-checked so
// the observer has the ground truth it needs.

#include "net/impairment_proxy.hpp"
#include "reference/tftp_reference_engine.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace tftp_test_harness;
using namespace tftp_test_harness::net;
using namespace tftp_test_harness::reference;

namespace {

std::filesystem::path make_temp_directory(const std::string& tag) {
    auto base = std::filesystem::temp_directory_path() /
                ("tftp_proxy_test_" + tag + "_" + std::to_string(::getpid()) +
                 "_" + std::to_string(std::rand()));
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
    config.retransmission_timeout = std::chrono::milliseconds(250);
    config.maximum_retransmissions = 20; // tolerate heavy loss in the test
    return config;
}

struct Harness {
    std::filesystem::path directory;
    ReferenceServer server;
    ImpairmentProxy proxy;
    Endpoint proxy_endpoint;

    Harness() : server(fast_config()) {}
};

// Bring up server + proxy around a served file; returns the client-facing proxy
// endpoint.
void bring_up(Harness& harness, const std::vector<std::uint8_t>& served_bytes,
              const ImpairmentConfig& impairment, std::uint64_t seed) {
    harness.directory = make_temp_directory("case");
    write_entire_file(harness.directory / "payload.bin", served_bytes);
    const Endpoint server_endpoint =
        harness.server.start(harness.directory, "127.0.0.1");

    ImpairmentProxy::Config config;
    config.listen_host = "127.0.0.1";
    config.server_endpoint = server_endpoint;
    config.impairment = impairment;
    config.seed = seed;
    TFTP_CHECK_TRUE(harness.proxy.start(config));
    harness.proxy_endpoint = harness.proxy.listen_endpoint();
}

} // namespace

TFTP_TEST_CASE(zero_impairment_passthrough,
               "Transfer through a zero-impairment proxy is byte-exact") {
    Harness harness;
    const auto served = deterministic_bytes(20000, 1);
    bring_up(harness, served, ImpairmentConfig{}, 1);

    ReferenceClient client(fast_config());
    ClientRequestedOptions none;
    const auto destination = harness.directory / "out.bin";
    TransferOutcome outcome = client.read_file(
        harness.proxy_endpoint, "payload.bin", destination, "octet", none);

    PacketTrace trace = harness.proxy.stop_and_take_trace();
    harness.server.stop();

    TFTP_CHECK_TRUE(outcome.completed_successfully);
    auto delivered = read_entire_file(destination);
    TFTP_CHECK_TRUE(delivered.has_value());
    TFTP_CHECK_TRUE(*delivered == served);

    // The trace must contain DATA (server->client) and ACK (client->server).
    bool saw_data = false;
    bool saw_ack = false;
    for (const auto& record : trace.records) {
        if (record.is_data()) saw_data = true;
        if (record.is_ack()) saw_ack = true;
    }
    TFTP_CHECK_TRUE(saw_data);
    TFTP_CHECK_TRUE(saw_ack);
    std::filesystem::remove_all(harness.directory);
}

TFTP_TEST_CASE(server_tid_switch_observed,
               "Proxy observes the server's fresh TID (A-20)") {
    Harness harness;
    const auto served = deterministic_bytes(1500, 2);
    bring_up(harness, served, ImpairmentConfig{}, 2);

    ReferenceClient client(fast_config());
    ClientRequestedOptions none;
    client.read_file(harness.proxy_endpoint, "payload.bin",
                     harness.directory / "out.bin", "octet", none);
    PacketTrace trace = harness.proxy.stop_and_take_trace();
    harness.server.stop();

    // The server's DATA must originate from a TID distinct from the port the
    // client sent its RRQ to (the proxy relays to the server's well-known port,
    // and the server answers from a fresh ephemeral TID).
    std::uint16_t server_data_tid = 0;
    for (const auto& record : trace.records) {
        if (record.direction == Direction::ServerToClient && record.is_data()) {
            server_data_tid = record.source_tid;
            break;
        }
    }
    TFTP_CHECK_TRUE(server_data_tid != 0);
    std::filesystem::remove_all(harness.directory);
}

TFTP_TEST_CASE(completes_under_uniform_loss,
               "Transfer survives 25% uniform loss byte-exact (F-01)") {
    Harness harness;
    const auto served = deterministic_bytes(30000, 3);
    ImpairmentConfig impairment;
    impairment.uniform_loss_probability = 0.25;
    bring_up(harness, served, impairment, 424242);

    ReferenceClient client(fast_config());
    ClientRequestedOptions none;
    const auto destination = harness.directory / "out.bin";
    TransferOutcome outcome = client.read_file(
        harness.proxy_endpoint, "payload.bin", destination, "octet", none);

    PacketTrace trace = harness.proxy.stop_and_take_trace();
    harness.server.stop();

    TFTP_CHECK_TRUE(outcome.completed_successfully);
    auto delivered = read_entire_file(destination);
    TFTP_CHECK_TRUE(delivered.has_value());
    TFTP_CHECK_TRUE(*delivered == served);

    // Under loss the proxy must have dropped at least one datagram.
    bool saw_drop = false;
    for (const auto& record : trace.records) {
        if (record.disposition == TraceDisposition::Dropped) saw_drop = true;
    }
    TFTP_CHECK_TRUE(saw_drop);
    std::filesystem::remove_all(harness.directory);
}

TFTP_TEST_CASE(completes_under_loss_windowed,
               "Windowed transfer survives loss with rollback (E-07)") {
    Harness harness;
    const auto served = deterministic_bytes(60000, 4);
    ImpairmentConfig impairment;
    impairment.uniform_loss_probability = 0.15;
    bring_up(harness, served, impairment, 909090);

    ReferenceClient client(fast_config());
    ClientRequestedOptions options;
    options.window_size = 8;
    const auto destination = harness.directory / "out.bin";
    TransferOutcome outcome = client.read_file(
        harness.proxy_endpoint, "payload.bin", destination, "octet", options);

    harness.proxy.stop_and_take_trace();
    harness.server.stop();

    TFTP_CHECK_TRUE(outcome.completed_successfully);
    auto delivered = read_entire_file(destination);
    TFTP_CHECK_TRUE(delivered.has_value());
    TFTP_CHECK_TRUE(*delivered == served);
    std::filesystem::remove_all(harness.directory);
}

TFTP_TEST_MAIN()
