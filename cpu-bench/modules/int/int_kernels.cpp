#include "int_kernels.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace intk {

uint64_t splitmix64(uint64_t& state) {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// ---------------------------------------------------------------- CRC-32

static const std::array<uint32_t, 256>& crc_table() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    return table;
}

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc) {
    const auto& t = crc_table();
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = t[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------- SHA-256

static const uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) |
               (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + kSha256K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t full = len / 64;
    for (size_t i = 0; i < full; ++i) sha256_block(h, data + 64 * i);

    uint8_t tail[128] = {};
    size_t rem = len - full * 64;
    std::memcpy(tail, data + full * 64, rem);
    tail[rem] = 0x80;
    size_t tail_len = (rem < 56) ? 64 : 128;
    uint64_t bits = uint64_t(len) * 8;
    for (int i = 0; i < 8; ++i) tail[tail_len - 1 - i] = uint8_t(bits >> (8 * i));
    sha256_block(h, tail);
    if (tail_len == 128) sha256_block(h, tail + 64);

    for (int i = 0; i < 8; ++i) {
        out[4 * i]     = uint8_t(h[i] >> 24);
        out[4 * i + 1] = uint8_t(h[i] >> 16);
        out[4 * i + 2] = uint8_t(h[i] >> 8);
        out[4 * i + 3] = uint8_t(h[i]);
    }
}

// ---------------------------------------------------------------- 파서

uint64_t parse_sum(const char* text, size_t len, uint32_t* count) {
    uint64_t sum = 0, cur = 0;
    uint32_t n = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
        } else if (c == ';') {
            sum += cur;
            cur = 0;
            ++n;
        }
    }
    if (count) *count = n;
    return sum;
}

// ---------------------------------------------------------------- 작업 단위

static inline uint64_t load_le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

static inline uint32_t load_le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

Input make_input(uint64_t seed) {
    Input in;
    in.buf.resize(kBufBytes);
    uint64_t state = seed;
    for (size_t i = 0; i < kBufBytes; i += 8) {
        uint64_t v = splitmix64(state);
        for (int b = 0; b < 8; ++b) in.buf[i + b] = uint8_t(v >> (8 * b));
    }
    in.text.reserve(kTextWords * 7);
    for (size_t i = 0; i < kTextWords; ++i) {
        in.text += std::to_string(load_le32(&in.buf[i * 4]) % 100000u);
        in.text += ';';
    }
    return in;
}

uint64_t run_unit(const Input& in, uint32_t* scratch) {
    const uint8_t* buf = in.buf.data();

    uint32_t crc = crc32(buf, kBufBytes);

    uint8_t digest[32];
    sha256(buf, kBufBytes, digest);
    uint64_t h = load_le64(digest);

    for (size_t i = 0; i < kSortCount; ++i) scratch[i] = load_le32(buf + i * 4);
    std::sort(scratch, scratch + kSortCount);
    uint8_t le[4];
    uint32_t s = 0;
    for (size_t i = 0; i < kSortCount; ++i) {
        le[0] = uint8_t(scratch[i]);
        le[1] = uint8_t(scratch[i] >> 8);
        le[2] = uint8_t(scratch[i] >> 16);
        le[3] = uint8_t(scratch[i] >> 24);
        s = crc32(le, 4, s);
    }

    uint64_t p = parse_sum(in.text.data(), in.text.size(), nullptr);

    return h ^ ((uint64_t(crc) << 32) | s) ^ (p * 0x9E3779B97F4A7C15ull);
}

}  // namespace intk
