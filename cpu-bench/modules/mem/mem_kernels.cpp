#include "mem_kernels.h"

#include <emmintrin.h>

#include <utility>

namespace memk {

uint64_t splitmix64(uint64_t& state) {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void fill_stream(uint64_t* dst, size_t words, uint64_t seed) {
    uint64_t state = seed;
    for (size_t i = 0; i < words; ++i) dst[i] = splitmix64(state);
}

void write_pattern(uint64_t seed, uint64_t& lo, uint64_t& hi) {
    uint64_t state = seed ^ 0x5752495445ull;  // "WRITE"
    lo = splitmix64(state);
    hi = splitmix64(state);
}

// ---------------------------------------------------------------- 대역폭

uint64_t sum_words(const uint64_t* p, size_t words) {
    // 누산기 4개로 덧셈 의존 사슬을 끊어 로드가 병목이 되게 한다
    __m128i a0 = _mm_setzero_si128(), a1 = a0, a2 = a0, a3 = a0;
    const __m128i* v = reinterpret_cast<const __m128i*>(p);
    const size_t n = words / 2;
    for (size_t i = 0; i < n; i += 4) {
        a0 = _mm_add_epi64(a0, _mm_load_si128(v + i));
        a1 = _mm_add_epi64(a1, _mm_load_si128(v + i + 1));
        a2 = _mm_add_epi64(a2, _mm_load_si128(v + i + 2));
        a3 = _mm_add_epi64(a3, _mm_load_si128(v + i + 3));
    }
    __m128i s = _mm_add_epi64(_mm_add_epi64(a0, a1), _mm_add_epi64(a2, a3));
    alignas(16) uint64_t lanes[2];
    _mm_store_si128(reinterpret_cast<__m128i*>(lanes), s);
    return lanes[0] + lanes[1];
}

void fill_words(uint64_t* p, size_t words, uint64_t lo, uint64_t hi) {
    const __m128i x = _mm_set_epi64x(static_cast<long long>(hi), static_cast<long long>(lo));
    __m128i* v = reinterpret_cast<__m128i*>(p);
    const size_t n = words / 2;
    for (size_t i = 0; i < n; i += 4) {
        _mm_store_si128(v + i, x);
        _mm_store_si128(v + i + 1, x);
        _mm_store_si128(v + i + 2, x);
        _mm_store_si128(v + i + 3, x);
    }
}

void copy_words(uint64_t* dst, const uint64_t* src, size_t words) {
    const __m128i* s = reinterpret_cast<const __m128i*>(src);
    __m128i* d = reinterpret_cast<__m128i*>(dst);
    const size_t n = words / 2;
    for (size_t i = 0; i < n; i += 4) {
        __m128i x0 = _mm_load_si128(s + i);
        __m128i x1 = _mm_load_si128(s + i + 1);
        __m128i x2 = _mm_load_si128(s + i + 2);
        __m128i x3 = _mm_load_si128(s + i + 3);
        _mm_store_si128(d + i, x0);
        _mm_store_si128(d + i + 1, x1);
        _mm_store_si128(d + i + 2, x2);
        _mm_store_si128(d + i + 3, x3);
    }
}

uint64_t sum_words_scalar(const uint64_t* p, size_t words) {
    uint64_t s = 0;
    for (size_t i = 0; i < words; ++i) s += p[i];
    return s;
}

// ---------------------------------------------------------------- 지연

void build_chain(void* base, size_t nodes, uint64_t seed, uint32_t* order) {
    // Sattolo: 모든 노드를 한 바퀴에 도는 무작위 순환 순열. next[k] = order[k]
    for (size_t i = 0; i < nodes; ++i) order[i] = uint32_t(i);
    uint64_t state = seed;
    for (size_t i = nodes - 1; i > 0; --i) {
        size_t j = size_t(splitmix64(state) % i);
        std::swap(order[i], order[j]);
    }
    char* b = static_cast<char*>(base);
    for (size_t k = 0; k < nodes; ++k) *reinterpret_cast<void**>(b + k * kLine) = b + size_t(order[k]) * kLine;
}

uint64_t chain_digest(const void* base, size_t nodes) {
    const char* b = static_cast<const char*>(base);
    const void* p = base;
    uint64_t d = 0xCBF29CE484222325ull;
    const size_t steps = nodes < kDigestSteps ? nodes : kDigestSteps;
    for (size_t s = 0; s < steps; ++s) {
        p = *static_cast<const void* const*>(p);
        uint64_t idx = uint64_t(static_cast<const char*>(p) - b) / kLine;
        d = (d ^ idx) * 0x100000001B3ull;
    }
    return d;
}

const void* chase(const void* p, size_t steps) {
#define CB_NEXT p = *static_cast<const void* const*>(p)
    size_t n = steps / 8;
    while (n--) {
        CB_NEXT; CB_NEXT; CB_NEXT; CB_NEXT;
        CB_NEXT; CB_NEXT; CB_NEXT; CB_NEXT;
    }
    for (size_t r = steps % 8; r; --r) CB_NEXT;
#undef CB_NEXT
    return p;
}

}  // namespace memk
