#ifndef TFTP_TEST_HARNESS_NET_DETERMINISTIC_PRNG_HPP
#define TFTP_TEST_HARNESS_NET_DETERMINISTIC_PRNG_HPP

#include <cstdint>

namespace tftp_test_harness::net {

// A small, self-contained, fully deterministic pseudo-random number generator.
//
// The prompt requires that "the proxy uses a seeded PRNG; record the seed with
// every test so any failure reproduces exactly." We deliberately do NOT use
// std::mt19937 seeded from std::random_device or the platform default, because
// the exact bit stream of the standard distributions (e.g.
// std::uniform_int_distribution) is implementation-defined and would not
// reproduce identically across compilers/standard libraries. This generator
// (SplitMix64) produces the same stream everywhere from the same seed, so a
// recorded seed reproduces a failing run byte-for-byte on any toolchain.
class DeterministicPrng {
public:
    explicit DeterministicPrng(std::uint64_t seed = 0) : state_(seed) {}

    void reseed(std::uint64_t seed) { state_ = seed; }
    std::uint64_t seed_snapshot() const { return state_; }

    // SplitMix64 core. Public-domain algorithm by Sebastiano Vigna.
    std::uint64_t next_u64() {
        state_ += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Uniform double in [0, 1). 53 bits of mantissa precision.
    double next_unit_double() {
        return static_cast<double>(next_u64() >> 11) *
               (1.0 / 9007199254740992.0);
    }

    // True with the given probability in [0, 1]. Used by every stochastic
    // impairment stage (loss, duplication, corruption, ...).
    bool bernoulli(double probability) {
        if (probability <= 0.0) return false;
        if (probability >= 1.0) return true;
        return next_unit_double() < probability;
    }

    // Uniform integer in [low, high] inclusive. Unbiased rejection sampling so
    // the stream stays deterministic and even across ranges.
    std::uint64_t uniform_in_range(std::uint64_t low, std::uint64_t high) {
        if (high <= low) return low;
        const std::uint64_t span = high - low; // inclusive width - 1
        if (span == UINT64_MAX) return next_u64();
        const std::uint64_t limit = span + 1;
        const std::uint64_t threshold = (UINT64_MAX - limit + 1) % limit;
        for (;;) {
            const std::uint64_t value = next_u64();
            if (value >= threshold) {
                return low + (value % limit);
            }
        }
    }

private:
    std::uint64_t state_;
};

} // namespace tftp_test_harness::net

#endif // TFTP_TEST_HARNESS_NET_DETERMINISTIC_PRNG_HPP
