// 메모리 모듈 정확성 테스트
//   1) SSE2 커널이 단순 구현과 같은 결과를 내는지
//   2) 지연 체인이 모든 노드를 한 바퀴에 도는 하나의 순환인지
//   3) 모듈 DLL이 낸 checksum이 Python 독립 구현(tools/mem_reference.py)과 같은지
#include "bench_api.h"
#include "mem_kernels.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

static int g_failed = 0;

static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failed;
}

template <class T>
static T* aligned_alloc64(size_t count) {
    return static_cast<T*>(_aligned_malloc(count * sizeof(T), 64));
}

static void test_kernels() {
    const size_t words = 4096;
    uint64_t* a = aligned_alloc64<uint64_t>(words);
    uint64_t* b = aligned_alloc64<uint64_t>(words);
    memk::fill_stream(a, words, 7);
    check(memk::sum_words(a, words) == memk::sum_words_scalar(a, words), "sum_words(sse2) == scalar sum");

    memk::fill_words(b, words, 0x1111, 0x2222);
    bool pattern_ok = true;
    for (size_t i = 0; i < words; ++i) pattern_ok &= b[i] == (i % 2 ? 0x2222u : 0x1111u);
    check(pattern_ok, "fill_words writes lo/hi pattern");

    memk::copy_words(b, a, words);
    bool copy_ok = true;
    for (size_t i = 0; i < words; ++i) copy_ok &= a[i] == b[i];
    check(copy_ok, "copy_words copies every word");
    _aligned_free(a);
    _aligned_free(b);

    const size_t nodes = 1000;
    char* base = aligned_alloc64<char>(nodes * memk::kLine);
    std::vector<uint32_t> order(nodes);
    memk::build_chain(base, nodes, 3, order.data());
    std::vector<char> seen(nodes, 0);
    const void* p = base;
    size_t visited = 0;
    for (size_t s = 0; s < nodes; ++s) {
        p = memk::chase(p, 1);
        size_t idx = size_t(static_cast<const char*>(p) - base) / memk::kLine;
        if (idx < nodes && !seen[idx]) {
            seen[idx] = 1;
            ++visited;
        }
    }
    check(visited == nodes && p == base, "chain visits every node once and returns to start");
    check(memk::chase(base, nodes * 3) == base, "chase(3 cycles) returns to start");
    _aligned_free(base);
}

static void test_module_checksums() {
    // 기대값: python tools/mem_reference.py <workload> <seed> <bytes> <threads>
    struct Golden { const char* workload; uint64_t seed, bytes; uint32_t threads; uint64_t checksum; };
    const Golden golden[] = {
        {"read", 1, 1u << 20, 1, 0xef0e8b0de780106dull},
        {"read", 42, 1u << 20, 4, 0x33beac291503c620ull},
        {"copy", 1, 2u << 20, 1, 0xef0e8b0de780106dull},
        {"write", 1, 1u << 20, 3, 0x18b0c08a4e8e18b4ull},
        {"latency", 1, 16384, 1, 0xcbe7d9a3495f9da5ull},
        {"latency", 1, 1u << 20, 1, 0x7c2f4d032a46de90ull},
        {"latency", 42, 1u << 20, 4, 0x50a12d625cb372d4ull},
    };
    for (const auto& g : golden) {
        BenchConfig cfg{};
        cfg.struct_size = sizeof(cfg);
        cfg.threads = g.threads;
        cfg.duration_ms = 50;
        cfg.seed = g.seed;
        cfg.workload = g.workload;
        cfg.working_set_bytes = g.bytes;
        BenchContext* ctx = nullptr;
        int32_t rc = bench_init(&cfg, &ctx);
        BenchResult res{};
        res.struct_size = sizeof(res);
        if (rc == BENCH_OK) rc = bench_run(ctx, &res, nullptr, nullptr, nullptr);
        if (ctx) bench_shutdown(ctx);
        char what[160];
        std::snprintf(what, sizeof(what), "%s seed %llu, %llu bytes, %u threads: rc %d, checksum 0x%016llx, %llu mismatches",
                      g.workload, static_cast<unsigned long long>(g.seed), static_cast<unsigned long long>(g.bytes),
                      g.threads, rc, static_cast<unsigned long long>(res.checksum),
                      static_cast<unsigned long long>(res.mismatches));
        check(rc == BENCH_OK && res.checksum == g.checksum && res.mismatches == 0 && res.iterations > 0, what);
    }

    BenchConfig cfg{};
    cfg.struct_size = sizeof(cfg);
    cfg.threads = 1;
    cfg.duration_ms = 50;
    cfg.workload = "nope";
    BenchContext* ctx = nullptr;
    check(bench_init(&cfg, &ctx) == BENCH_E_UNSUPPORTED && !ctx, "unknown workload -> BENCH_E_UNSUPPORTED");
    cfg.workload = "read";
    cfg.working_set_bytes = 1024;
    check(bench_init(&cfg, &ctx) == BENCH_E_INVALID_ARG && !ctx, "working set below 4 KiB per thread -> INVALID_ARG");
}

int main() {
    test_kernels();
    test_module_checksums();
    std::printf("%s\n", g_failed ? "FAILED" : "ALL PASSED");
    return g_failed ? 1 : 0;
}
