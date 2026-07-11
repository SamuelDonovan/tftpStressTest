// Per-stage unit tests for the stochastic impairment pipeline. Each stage is
// exercised in isolation, and determinism (same seed => same decisions) is
// verified so a recorded seed reproduces a failing run exactly.

#include "net/impairments.hpp"
#include "net/tftp_packet.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <vector>

using namespace tftp_test_harness::net;

namespace {

std::vector<std::uint8_t> sample_data_packet() {
    // DATA block 1 with a 512-byte payload.
    std::vector<std::uint8_t> payload(512, 0xAB);
    return serialize_data(1, payload);
}

ImpairmentConfig none() { return ImpairmentConfig{}; }

} // namespace

TFTP_TEST_CASE(loss_never_when_zero, "F-01 zero loss delivers every datagram") {
    ImpairmentPipeline pipeline(none(), 1);
    for (int i = 0; i < 100; ++i) {
        auto result = pipeline.process(Direction::ServerToClient,
                                       sample_data_packet(), true);
        TFTP_CHECK_FALSE(result.dropped);
        TFTP_CHECK_EQUAL(result.deliveries.size(), std::size_t(1));
    }
}

TFTP_TEST_CASE(loss_always_when_one, "F-01 loss=1.0 drops every datagram") {
    ImpairmentConfig config = none();
    config.uniform_loss_probability = 1.0;
    ImpairmentPipeline pipeline(config, 1);
    for (int i = 0; i < 20; ++i) {
        auto result = pipeline.process(Direction::ServerToClient,
                                       sample_data_packet(), true);
        TFTP_CHECK_TRUE(result.dropped);
        TFTP_CHECK_TRUE(result.deliveries.empty());
    }
}

TFTP_TEST_CASE(loss_rate_is_approximate, "F-01 50% loss drops roughly half") {
    ImpairmentConfig config = none();
    config.uniform_loss_probability = 0.5;
    ImpairmentPipeline pipeline(config, 12345);
    int dropped = 0;
    const int trials = 10000;
    for (int i = 0; i < trials; ++i) {
        if (pipeline.process(Direction::ClientToServer, sample_data_packet(),
                             true)
                .dropped) {
            ++dropped;
        }
    }
    // Expect near 5000; allow a generous band for the finite sample.
    TFTP_CHECK_TRUE(dropped > 4500 && dropped < 5500);
}

TFTP_TEST_CASE(loss_is_deterministic, "Same seed reproduces the loss sequence") {
    ImpairmentConfig config = none();
    config.uniform_loss_probability = 0.3;
    ImpairmentPipeline a(config, 777);
    ImpairmentPipeline b(config, 777);
    for (int i = 0; i < 500; ++i) {
        const bool drop_a = a.process(Direction::ServerToClient,
                                      sample_data_packet(), true)
                                .dropped;
        const bool drop_b = b.process(Direction::ServerToClient,
                                      sample_data_packet(), true)
                                .dropped;
        TFTP_CHECK_EQUAL(drop_a, drop_b);
    }
}

TFTP_TEST_CASE(gilbert_elliott_bursts, "F-02 bursty loss produces correlated runs") {
    ImpairmentConfig config = none();
    config.gilbert_elliott_enabled = true;
    config.good_to_bad_probability = 0.02;
    config.bad_to_good_probability = 0.2;
    config.loss_in_good_state = 0.0;
    config.loss_in_bad_state = 1.0;
    ImpairmentPipeline pipeline(config, 42);
    int longest_run = 0;
    int current_run = 0;
    int total_dropped = 0;
    for (int i = 0; i < 5000; ++i) {
        const bool dropped = pipeline.process(Direction::ServerToClient,
                                              sample_data_packet(), true)
                                 .dropped;
        if (dropped) {
            ++current_run;
            ++total_dropped;
            longest_run = std::max(longest_run, current_run);
        } else {
            current_run = 0;
        }
    }
    // Correlated loss must produce runs longer than 1 (a burst), and some loss.
    TFTP_CHECK_TRUE(total_dropped > 0);
    TFTP_CHECK_TRUE(longest_run >= 2);
}

TFTP_TEST_CASE(duplication_emits_two, "F-03 duplication yields a second delivery") {
    ImpairmentConfig config = none();
    config.duplication_probability = 1.0;
    ImpairmentPipeline pipeline(config, 5);
    auto result =
        pipeline.process(Direction::ServerToClient, sample_data_packet(), true);
    TFTP_CHECK_FALSE(result.dropped);
    TFTP_CHECK_EQUAL(result.deliveries.size(), std::size_t(2));
    TFTP_CHECK_TRUE(result.deliveries[1].is_duplicate);
    // Both copies carry identical bytes.
    TFTP_CHECK_TRUE(result.deliveries[0].bytes == result.deliveries[1].bytes);
}

TFTP_TEST_CASE(corruption_changes_bytes, "F-06 corruption mutates the datagram") {
    ImpairmentConfig config = none();
    config.corruption_probability = 1.0;
    config.corruption_target = CorruptionTarget::DataPayload;
    ImpairmentPipeline pipeline(config, 9);
    auto original = sample_data_packet();
    auto result = pipeline.process(Direction::ServerToClient, original, true);
    TFTP_CHECK_EQUAL(result.deliveries.size(), std::size_t(1));
    TFTP_CHECK_TRUE(result.deliveries[0].corrupted);
    TFTP_CHECK_FALSE(result.deliveries[0].bytes == original);
    // Same length (single bit flip preserves size).
    TFTP_CHECK_EQUAL(result.deliveries[0].bytes.size(), original.size());
}

TFTP_TEST_CASE(corruption_block_number_target,
               "F-06 block-number corruption touches bytes 2-3 (A-41)") {
    ImpairmentConfig config = none();
    config.corruption_probability = 1.0;
    config.corruption_target = CorruptionTarget::BlockNumber;
    ImpairmentPipeline pipeline(config, 3);
    auto original = sample_data_packet();
    auto result = pipeline.process(Direction::ServerToClient, original, true);
    const auto& mutated = result.deliveries[0].bytes;
    // Only a block-number byte differs.
    TFTP_CHECK_TRUE(mutated[0] == original[0] && mutated[1] == original[1]);
    const bool block_bytes_differ =
        mutated[2] != original[2] || mutated[3] != original[3];
    TFTP_CHECK_TRUE(block_bytes_differ);
}

TFTP_TEST_CASE(fixed_delay_applied, "F-05 fixed delay is reflected on delivery") {
    ImpairmentConfig config = none();
    config.fixed_delay = std::chrono::milliseconds(50);
    ImpairmentPipeline pipeline(config, 1);
    auto result =
        pipeline.process(Direction::ServerToClient, sample_data_packet(), true);
    TFTP_CHECK_TRUE(result.deliveries[0].delay >= std::chrono::milliseconds(50));
}

TFTP_TEST_CASE(reorder_adds_delay, "F-04 reordering delays the datagram") {
    ImpairmentConfig config = none();
    config.reorder_probability = 1.0;
    config.reorder_extra_delay = std::chrono::milliseconds(80);
    ImpairmentPipeline pipeline(config, 1);
    auto result =
        pipeline.process(Direction::ServerToClient, sample_data_packet(), true);
    TFTP_CHECK_TRUE(result.deliveries[0].delay >= std::chrono::milliseconds(80));
}

TFTP_TEST_CASE(blackout_after_n, "F-09 blackout drops everything after N data") {
    ImpairmentConfig config = none();
    config.blackout_enabled = true;
    config.blackout_after_data_packets = 3;
    ImpairmentPipeline pipeline(config, 1);
    int delivered = 0;
    int dropped = 0;
    for (int i = 0; i < 10; ++i) {
        if (pipeline.process(Direction::ServerToClient, sample_data_packet(),
                             true)
                .dropped) {
            ++dropped;
        } else {
            ++delivered;
        }
    }
    // Exactly the first 3 DATA packets pass; the rest are blacked out.
    TFTP_CHECK_EQUAL(delivered, 3);
    TFTP_CHECK_EQUAL(dropped, 7);
}

TFTP_TEST_CASE(throttle_delays_when_over_rate,
               "F-07 throttle delays traffic above the byte rate") {
    ImpairmentConfig config = none();
    config.throttle_enabled = true;
    config.throttle_bytes_per_second = 1024.0; // ~2 blocks/sec
    ImpairmentPipeline pipeline(config, 1);
    // Feed several 516-byte datagrams back-to-back at t0; later ones must be
    // delayed because the token bucket is exhausted.
    auto base = ImpairmentPipeline::Clock::now();
    bool saw_delay = false;
    for (int i = 0; i < 6; ++i) {
        auto result = pipeline.process(Direction::ServerToClient,
                                       sample_data_packet(), true, base);
        if (result.deliveries[0].delay > std::chrono::milliseconds(0)) {
            saw_delay = true;
        }
    }
    TFTP_CHECK_TRUE(saw_delay);
}

TFTP_TEST_MAIN()
