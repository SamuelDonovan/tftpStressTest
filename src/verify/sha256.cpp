#include "verify/sha256.hpp"

#include <algorithm>
#include <cstring>

// SHA-256 (FIPS 180-4). Public-domain reference construction; self-contained.

namespace tftp_test_harness::verify {

namespace {

inline std::uint32_t rotate_right(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
}

// FIPS 180-4 section 4.2.2 round constants (first 32 bits of the fractional
// parts of the cube roots of the first 64 primes).
constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

} // namespace

void Sha256::reset() {
    // FIPS 180-4 section 5.3.3 initial hash value.
    state_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    total_length_ = 0;
    buffer_length_ = 0;
}

void Sha256::process_block(const std::uint8_t* block) {
    std::uint32_t words[64];
    for (int i = 0; i < 16; ++i) {
        words[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                   (static_cast<std::uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotate_right(words[i - 15], 7) ^
                                 rotate_right(words[i - 15], 18) ^
                                 (words[i - 15] >> 3);
        const std::uint32_t s1 = rotate_right(words[i - 2], 17) ^
                                 rotate_right(words[i - 2], 19) ^
                                 (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 64; ++i) {
        const std::uint32_t big_s1 =
            rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + big_s1 + choose + kRoundConstants[i] +
                                    words[i];
        const std::uint32_t big_s0 =
            rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = big_s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t length) {
    total_length_ += length;
    while (length > 0) {
        const std::size_t take =
            std::min<std::size_t>(64 - buffer_length_, length);
        std::memcpy(buffer_.data() + buffer_length_, data, take);
        buffer_length_ += take;
        data += take;
        length -= take;
        if (buffer_length_ == 64) {
            process_block(buffer_.data());
            buffer_length_ = 0;
        }
    }
}

std::array<std::uint8_t, 32> Sha256::finalize() {
    // Append the 0x80 padding byte, then zeros, then the 64-bit bit length.
    const std::uint64_t bit_length = total_length_ * 8;
    std::uint8_t padding_byte = 0x80;
    update(&padding_byte, 1);
    std::uint8_t zero = 0;
    while (buffer_length_ != 56) {
        update(&zero, 1);
    }
    std::uint8_t length_bytes[8];
    for (int i = 0; i < 8; ++i) {
        length_bytes[i] =
            static_cast<std::uint8_t>((bit_length >> (56 - i * 8)) & 0xFF);
    }
    update(length_bytes, 8);

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFF);
    }
    return digest;
}

std::array<std::uint8_t, 32> sha256_digest(
    const std::vector<std::uint8_t>& data) {
    Sha256 hasher;
    hasher.update(data);
    return hasher.finalize();
}

std::string to_hex(const std::array<std::uint8_t, 32>& digest) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint8_t byte : digest) {
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 0x0F]);
    }
    return out;
}

std::string sha256_hex(const std::vector<std::uint8_t>& data) {
    return to_hex(sha256_digest(data));
}

} // namespace tftp_test_harness::verify
