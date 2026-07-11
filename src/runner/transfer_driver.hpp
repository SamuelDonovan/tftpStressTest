#ifndef TFTP_TEST_HARNESS_RUNNER_TRANSFER_DRIVER_HPP
#define TFTP_TEST_HARNESS_RUNNER_TRANSFER_DRIVER_HPP

// ---------------------------------------------------------------------------
// The transfer driver is the plumbing every test case shares: it stands up the
// server-under-test, interposes the impairment proxy, drives one transfer with
// the client-under-test, then tears everything down and hands back the app's
// own result, the proxy trace, the observer report, and the integrity verdict.
//
// A per-transfer watchdog bounds hangs so one stuck implementation cannot stall
// the suite; if the watchdog fires the transfer is reported as an ungraceful
// (hang) failure. Each transfer runs in its own fresh temp directories and on
// fresh ephemeral ports (per-test isolation).
// ---------------------------------------------------------------------------

#include "fixtures/fixture_generator.hpp"
#include "net/impairment_proxy.hpp"
#include "net/packet_trace.hpp"
#include "tftp_test_harness/adapter_interface.hpp"
#include "verify/content_oracle.hpp"
#include "verify/packet_observer.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace tftp_test_harness::runner {

struct DriveRequest {
    ClientAdapter* client = nullptr;
    ServerAdapter* server = nullptr;
    verify::TransferDirection direction = verify::TransferDirection::Download;
    const fixtures::Fixture* fixture = nullptr;
    RequestedOptions options;
    TransferMode mode = TransferMode::Octet;
    net::ImpairmentConfig impairment;
    std::uint64_t seed = 0;
    net::InjectionScript injection;
    std::filesystem::path work_root;
    std::chrono::milliseconds watchdog{20000};
    std::uint32_t effective_block_size = 512; // for the observer's final-block check
    // Remote filename to request; defaults to the fixture's name when empty.
    std::string remote_filename;
    // When true a pre-existing file is planted at the upload destination to
    // exercise the "file already exists" path (A-12).
    bool plant_existing_destination = false;
    // When false the requested file is NOT staged in the served directory, so a
    // download hits the "file not found" path (A-11) or a traversal target
    // (A-13). Default true (normal transfers need the file present).
    bool stage_served_fixture = true;
};

struct DriveResult {
    bool setup_failed = false;
    std::string setup_error;

    TransferResult app_result;         // the implementation's own view
    net::PacketTrace trace;
    verify::ObserverReport observer;
    verify::IntegrityResult integrity;

    bool watchdog_fired = false;       // the transfer hung past the watchdog
    double duration_ms = 0.0;
};

// Run one transfer end to end. Never throws; setup failures are reported via
// setup_failed.
DriveResult drive_transfer(const DriveRequest& request);

} // namespace tftp_test_harness::runner

#endif // TFTP_TEST_HARNESS_RUNNER_TRANSFER_DRIVER_HPP
