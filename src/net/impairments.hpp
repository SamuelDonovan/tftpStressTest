#ifndef TFTP_TEST_HARNESS_NET_IMPAIRMENTS_HPP
#define TFTP_TEST_HARNESS_NET_IMPAIRMENTS_HPP

// ---------------------------------------------------------------------------
// The stochastic impairment pipeline (F-series). Given one datagram arriving at
// the proxy, it decides — deterministically from a seeded PRNG — whether the
// datagram is dropped, delayed, duplicated, reordered, corrupted, or
// rate-limited, and produces a list of timed delivery actions the proxy then
// executes. Injection (A-21/A-22/G-series) is scripted separately by the proxy;
// this pipeline covers the impairments that are applied to genuine traffic.
//
// Keeping the pipeline free of sockets makes it unit-testable in isolation
// (Phase 3 requires per-stage tests) and keeps the PRNG draw order — and hence
// reproducibility from a recorded seed — fully deterministic.
//
// Note on "corruption passing the UDP checksum" (F-06): the harness runs on
// loopback where the kernel computes the real UDP checksum over whatever
// payload we hand it, so any mutation we make to the TFTP bytes is delivered
// intact and the datagram remains deliverable. Corruption is therefore modeled
// at the layer the conformance test cares about — the bytes the peer parses —
// which is exactly what F-06 ("application-level validation") measures.
// ---------------------------------------------------------------------------

#include "net/deterministic_prng.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace tftp_test_harness::net {

enum class Direction {
    ClientToServer,
    ServerToClient,
};

// Which part of a datagram the corruption stage mutates.
enum class CorruptionTarget {
    AnyByte,      // any byte in the datagram
    OpcodeField,  // the 2-byte opcode (G-03-style illegal opcodes)
    BlockNumber,  // the 2-byte block number (A-41 desync)
    DataPayload,  // a byte in the DATA payload (silent content corruption)
};

struct ImpairmentConfig {
    // ---- F-01 uniform packet loss ----
    double uniform_loss_probability = 0.0; // per-datagram drop probability

    // ---- F-02 Gilbert-Elliott bursty loss ----
    // A two-state Markov chain: a "good" state with low loss and a "bad" state
    // with high loss, producing correlated loss bursts.
    bool gilbert_elliott_enabled = false;
    double good_to_bad_probability = 0.05;  // P(transition good -> bad)
    double bad_to_good_probability = 0.30;  // P(transition bad -> good)
    double loss_in_good_state = 0.0;
    double loss_in_bad_state = 1.0;

    // ---- F-03 duplication ----
    double duplication_probability = 0.0;

    // ---- F-04 reordering ----
    // With this probability a datagram is held back by reorder_extra_delay so a
    // subsequently-arriving datagram overtakes it (bounded displacement).
    double reorder_probability = 0.0;
    std::chrono::milliseconds reorder_extra_delay{60};

    // ---- F-05 fixed delay + jitter ----
    std::chrono::milliseconds fixed_delay{0};
    std::chrono::milliseconds jitter{0}; // uniform additive [0, jitter]

    // ---- F-06 corruption ----
    double corruption_probability = 0.0;
    CorruptionTarget corruption_target = CorruptionTarget::DataPayload;

    // ---- F-07 bandwidth throttle (token bucket) ----
    bool throttle_enabled = false;
    double throttle_bytes_per_second = 0.0;

    // ---- F-09 total blackout after N delivered DATA packets ----
    bool blackout_enabled = false;
    std::uint32_t blackout_after_data_packets = 0;
};

// One scheduled delivery produced by the pipeline: the (possibly corrupted)
// bytes to deliver, and how long after arrival to deliver them.
struct DeliveryAction {
    std::vector<std::uint8_t> bytes;
    std::chrono::milliseconds delay{0};
    bool corrupted = false;
    bool is_duplicate = false;
};

struct PipelineResult {
    bool dropped = false; // the original was lost (no deliveries)
    std::vector<DeliveryAction> deliveries; // 0..2 (normal + optional duplicate)
    std::string drop_reason;                // for the trace narrative
};

// Stateful across datagrams (Gilbert-Elliott channel state, token bucket level,
// blackout counters). One pipeline instance drives one proxy session direction
// pair; the single-threaded proxy event loop keeps PRNG draws ordered.
class ImpairmentPipeline {
public:
    ImpairmentPipeline(ImpairmentConfig config, std::uint64_t seed)
        : config_(config), prng_(seed), last_refill_(Clock::now()) {}

    // Process one datagram travelling in `direction`. `is_data_packet` lets the
    // blackout stage count only DATA packets. `now` is injectable for testing.
    using Clock = std::chrono::steady_clock;
    PipelineResult process(Direction direction,
                           const std::vector<std::uint8_t>& datagram,
                           bool is_data_packet,
                           Clock::time_point now);

    PipelineResult process(Direction direction,
                           const std::vector<std::uint8_t>& datagram,
                           bool is_data_packet) {
        return process(direction, datagram, is_data_packet, Clock::now());
    }

    const ImpairmentConfig& config() const { return config_; }
    std::uint64_t prng_state() const { return prng_.seed_snapshot(); }

private:
    struct ChannelState {
        bool in_bad_state = false;
    };

    ChannelState& channel_for(Direction direction) {
        return direction == Direction::ClientToServer ? client_to_server_
                                                       : server_to_client_;
    }

    // Returns true if the loss stages decide to drop this datagram.
    bool decide_loss(Direction direction, std::string& reason);
    // Applies corruption to a copy of the datagram if the stage fires.
    std::vector<std::uint8_t> maybe_corrupt(
        const std::vector<std::uint8_t>& datagram, bool& corrupted);
    // Returns the throttle-imposed extra delay given the datagram size.
    std::chrono::milliseconds throttle_delay(std::size_t datagram_size,
                                             Clock::time_point now);

    ImpairmentConfig config_;
    DeterministicPrng prng_;
    ChannelState client_to_server_;
    ChannelState server_to_client_;
    std::uint32_t delivered_data_packets_ = 0;
    double token_bucket_bytes_ = 0.0;
    Clock::time_point last_refill_;
};

} // namespace tftp_test_harness::net

#endif // TFTP_TEST_HARNESS_NET_IMPAIRMENTS_HPP
