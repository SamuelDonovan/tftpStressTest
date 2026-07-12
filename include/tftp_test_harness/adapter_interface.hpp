#ifndef TFTP_TEST_HARNESS_ADAPTER_INTERFACE_HPP
#define TFTP_TEST_HARNESS_ADAPTER_INTERFACE_HPP

// ---------------------------------------------------------------------------
// Single plug-in point for the TFTP conformance and robustness test harness.
//
// A person evaluating a TFTP implementation implements ClientAdapter and/or
// ServerAdapter exactly once for that implementation. Every test in the suite
// then drives the implementation through this interface and through the
// harness's network-impairment proxy. Nothing else in the harness is aware of
// which implementation is under test; this is the only file a new integrator
// should need to touch.
//
// Design intent: distinguish clearly between three outcomes so the generated
// report is objective:
//   1. Correct behavior (transfer completed and verified, or failed cleanly).
//   2. Unsupported feature (declared via supports_option / supports_capability).
//   3. Incorrect behavior (a genuine conformance or resilience failure).
//
// This is a design anchor written during preplanning. Claude Code may refine
// signatures during implementation, but the "one plug-in point" property and
// the capability-declaration mechanism must be preserved.
// ---------------------------------------------------------------------------

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace tftp_test_harness {

// The address the implementation under test must talk to. The harness always
// interposes its impairment proxy at this endpoint, so adapters MUST direct
// traffic here rather than contacting the peer directly. All impairments
// (loss, duplication, reordering, delay, corruption, saturation, injection)
// are applied by that proxy on the local host only.
struct EndpointConfiguration {
    std::string proxy_host;      // typically "127.0.0.1" or "::1"
    std::uint16_t proxy_port{0}; // the impairment proxy's listening port
};

enum class TransferMode {
    Octet,     // RFC 1350 "octet" (binary)
    Netascii,  // RFC 1350 "netascii"
};

// Options an integrator may request per transfer. std::nullopt means the option
// is not requested at all, which is distinct from requesting a specific value.
struct RequestedOptions {
    std::optional<std::uint32_t> block_size;        // RFC 2348 "blksize"
    std::optional<std::uint32_t> timeout_seconds;   // RFC 2349 "timeout"
    std::optional<std::uint64_t> transfer_size;     // RFC 2349 "tsize"
    std::optional<std::uint16_t> window_size;       // RFC 7440 "windowsize"
};

// What the implementation itself believes happened. Packet-level conformance is
// judged independently by the proxy observer; this captures the application's
// own view (its exit status, the error code it surfaced, timing).
struct TransferResult {
    bool completed_successfully{false};
    std::optional<std::uint16_t> reported_error_code;   // RFC 1350 codes 0..7
    std::string reported_error_message;
    std::optional<int> process_exit_code;               // subprocess adapters
    std::chrono::milliseconds wall_clock_duration{0};
    RequestedOptions options_confirmed_in_use;          // best effort, if known

    // Set this when the implementation cannot express the requested transfer at
    // all -- not when it tried and failed. supports_capability() answers per
    // capability ("does it do blksize?"), but some implementations support a
    // capability only at particular values: Tftpd64's GUI client, for example,
    // offers blksize from a fixed dropdown, so blksize=1428 is not a defect it
    // can fail, it is a request it cannot make. Such a transfer is recorded as
    // SKIPPED (unsupported), never FAILED -- the same rule supports_capability
    // applies, at per-request granularity. Explain the limitation in
    // reported_error_message; it appears in the report.
    bool unsupported_configuration{false};
};

// Client downloads FROM the server (Read Request, RFC 1350 opcode 1).
struct ReadRequestSpecification {
    std::string remote_filename;
    std::filesystem::path local_destination_path;
    TransferMode mode{TransferMode::Octet};
    RequestedOptions requested_options;
};

// Client uploads TO the server (Write Request, RFC 1350 opcode 2).
struct WriteRequestSpecification {
    std::filesystem::path local_source_path;
    std::string remote_filename;
    TransferMode mode{TransferMode::Octet};
    RequestedOptions requested_options;
};

// Named capabilities used to separate "unsupported" from "failed" in reports.
// Extend as needed; keep names stable because they appear in the report.
namespace capability {
inline constexpr char option_negotiation[] = "rfc2347_option_negotiation";
inline constexpr char block_size[]          = "rfc2348_blksize";
inline constexpr char timeout_option[]      = "rfc2349_timeout";
inline constexpr char transfer_size[]       = "rfc2349_tsize";
inline constexpr char window_size[]         = "rfc7440_windowsize";
inline constexpr char netascii_mode[]       = "rfc1350_netascii";
inline constexpr char write_request[]       = "rfc1350_wrq";
inline constexpr char read_request[]        = "rfc1350_rrq";
} // namespace capability

// Implement this for the CLIENT you are evaluating. For a subprocess-based
// implementation (for example a system tftp binary), a helper base class in the
// harness handles launching the process, pointing it at proxy_host/proxy_port,
// and capturing its exit code; you only supply the argument construction.
class ClientAdapter {
public:
    virtual ~ClientAdapter() = default;

    // Identity that appears throughout the generated report.
    virtual std::string implementation_name() const = 0;
    virtual std::string implementation_version() const = 0;

    // Return true only if the implementation genuinely supports the capability.
    // A false return causes the relevant tests to be recorded as SKIPPED
    // (unsupported) rather than FAILED.
    virtual bool supports_capability(const std::string& capability_name) const = 0;

    virtual TransferResult perform_read_request(
        const EndpointConfiguration& endpoint,
        const ReadRequestSpecification& specification) = 0;

    virtual TransferResult perform_write_request(
        const EndpointConfiguration& endpoint,
        const WriteRequestSpecification& specification) = 0;
};

// Implement this for the SERVER you are evaluating.
class ServerAdapter {
public:
    virtual ~ServerAdapter() = default;

    virtual std::string implementation_name() const = 0;
    virtual std::string implementation_version() const = 0;
    virtual bool supports_capability(const std::string& capability_name) const = 0;

    // Start the server serving files out of served_directory. Return the real
    // endpoint it listens on; the harness then inserts its impairment proxy in
    // front of that endpoint and hands the proxy address to the client side.
    virtual EndpointConfiguration start(
        const std::filesystem::path& served_directory) = 0;

    virtual void stop() = 0;
};

} // namespace tftp_test_harness

#endif // TFTP_TEST_HARNESS_ADAPTER_INTERFACE_HPP
