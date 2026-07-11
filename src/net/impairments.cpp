#include "net/impairments.hpp"

#include <algorithm>

namespace tftp_test_harness::net {

bool ImpairmentPipeline::decide_loss(Direction direction, std::string& reason) {
    // F-01 uniform loss.
    if (prng_.bernoulli(config_.uniform_loss_probability)) {
        reason = "uniform loss";
        return true;
    }
    // F-02 Gilbert-Elliott bursty loss: advance the channel state first, then
    // apply the state-dependent loss probability.
    if (config_.gilbert_elliott_enabled) {
        ChannelState& state = channel_for(direction);
        if (state.in_bad_state) {
            if (prng_.bernoulli(config_.bad_to_good_probability)) {
                state.in_bad_state = false;
            }
        } else {
            if (prng_.bernoulli(config_.good_to_bad_probability)) {
                state.in_bad_state = true;
            }
        }
        const double loss = state.in_bad_state ? config_.loss_in_bad_state
                                               : config_.loss_in_good_state;
        if (prng_.bernoulli(loss)) {
            reason = state.in_bad_state ? "Gilbert-Elliott burst loss (bad)"
                                        : "Gilbert-Elliott loss (good)";
            return true;
        }
    }
    return false;
}

std::vector<std::uint8_t> ImpairmentPipeline::maybe_corrupt(
    const std::vector<std::uint8_t>& datagram, bool& corrupted) {
    corrupted = false;
    std::vector<std::uint8_t> bytes = datagram;
    if (bytes.empty() || !prng_.bernoulli(config_.corruption_probability)) {
        return bytes;
    }

    // Choose a byte index appropriate to the configured target. Indices follow
    // the wire layout in tftp_packet.hpp.
    auto flip_at = [&](std::size_t index) {
        if (index < bytes.size()) {
            // Flip a single pseudo-random bit so the datagram stays the same
            // length and remains parseable-but-different where possible.
            const std::uint8_t bit = static_cast<std::uint8_t>(
                1u << (prng_.uniform_in_range(0, 7)));
            bytes[index] ^= bit;
            corrupted = true;
        }
    };

    switch (config_.corruption_target) {
        case CorruptionTarget::OpcodeField:
            flip_at(static_cast<std::size_t>(prng_.uniform_in_range(0, 1)));
            break;
        case CorruptionTarget::BlockNumber:
            // DATA/ACK block number occupies bytes [2,3].
            flip_at(2 + static_cast<std::size_t>(prng_.uniform_in_range(0, 1)));
            break;
        case CorruptionTarget::DataPayload:
            if (bytes.size() > 4) {
                flip_at(4 + static_cast<std::size_t>(prng_.uniform_in_range(
                                0, bytes.size() - 5)));
            } else {
                flip_at(bytes.size() - 1);
            }
            break;
        case CorruptionTarget::AnyByte:
            flip_at(static_cast<std::size_t>(
                prng_.uniform_in_range(0, bytes.size() - 1)));
            break;
    }
    return bytes;
}

std::chrono::milliseconds ImpairmentPipeline::throttle_delay(
    std::size_t datagram_size, Clock::time_point now) {
    if (!config_.throttle_enabled || config_.throttle_bytes_per_second <= 0.0) {
        return std::chrono::milliseconds(0);
    }
    // Token bucket: refill at the configured byte rate, charge the datagram's
    // size, and if the bucket is short, delay long enough to accrue the deficit.
    const double elapsed_seconds =
        std::chrono::duration<double>(now - last_refill_).count();
    last_refill_ = now;
    token_bucket_bytes_ += elapsed_seconds * config_.throttle_bytes_per_second;
    // Cap the bucket at one second of traffic to bound bursts.
    token_bucket_bytes_ = std::min(token_bucket_bytes_,
                                   config_.throttle_bytes_per_second);
    token_bucket_bytes_ -= static_cast<double>(datagram_size);
    if (token_bucket_bytes_ >= 0.0) {
        return std::chrono::milliseconds(0);
    }
    const double deficit = -token_bucket_bytes_;
    const double seconds_needed = deficit / config_.throttle_bytes_per_second;
    return std::chrono::milliseconds(
        static_cast<long long>(seconds_needed * 1000.0));
}

PipelineResult ImpairmentPipeline::process(
    Direction direction, const std::vector<std::uint8_t>& datagram,
    bool is_data_packet, Clock::time_point now) {
    PipelineResult result;

    // F-09 total blackout after N delivered DATA packets: once the threshold is
    // crossed, every subsequent datagram is dropped (bounded-termination test).
    if (config_.blackout_enabled &&
        delivered_data_packets_ >= config_.blackout_after_data_packets) {
        result.dropped = true;
        result.drop_reason = "blackout after N data packets";
        return result;
    }

    // Loss stages.
    std::string loss_reason;
    if (decide_loss(direction, loss_reason)) {
        result.dropped = true;
        result.drop_reason = loss_reason;
        return result;
    }

    // The datagram survives: count it if it is a DATA packet (for blackout).
    if (is_data_packet) {
        ++delivered_data_packets_;
    }

    // Corruption (F-06).
    bool corrupted = false;
    std::vector<std::uint8_t> bytes = maybe_corrupt(datagram, corrupted);

    // Base delay: fixed delay + uniform jitter (F-05) + throttle (F-07).
    std::chrono::milliseconds delay = config_.fixed_delay;
    if (config_.jitter.count() > 0) {
        delay += std::chrono::milliseconds(static_cast<long long>(
            prng_.uniform_in_range(0,
                                   static_cast<std::uint64_t>(
                                       config_.jitter.count()))));
    }
    delay += throttle_delay(bytes.size(), now);

    // Reordering (F-04): hold this datagram back so a later one overtakes it.
    if (prng_.bernoulli(config_.reorder_probability)) {
        delay += config_.reorder_extra_delay;
    }

    DeliveryAction primary;
    primary.bytes = bytes;
    primary.delay = delay;
    primary.corrupted = corrupted;
    result.deliveries.push_back(std::move(primary));

    // Duplication (F-03): emit a second copy shortly after the first.
    if (prng_.bernoulli(config_.duplication_probability)) {
        DeliveryAction duplicate;
        duplicate.bytes = bytes;
        duplicate.delay = delay + std::chrono::milliseconds(1);
        duplicate.corrupted = corrupted;
        duplicate.is_duplicate = true;
        result.deliveries.push_back(std::move(duplicate));
    }

    return result;
}

} // namespace tftp_test_harness::net
