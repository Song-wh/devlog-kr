// 정수(INT) 부하 모듈 DLL: bench_api.h 구현
#define BENCH_MODULE_BUILD
#include "bench_api.h"
#include "int_kernels.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

struct BenchContext {
    BenchConfig cfg{};
    intk::Input input;
    uint64_t expected = 0;  // bench_init에서 한 번 계산한 기준 digest
};

namespace {

const BenchInfo kInfo = {
    sizeof(BenchInfo), BENCH_API_VERSION, "int", "Integer (CRC32, SHA-256, sort, parse)", "0.1.0", "unit"};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // 정렬 때문에 생기는 패딩은 의도한 것
#endif
struct alignas(64) Worker {  // 스레드끼리 같은 캐시 라인을 쓰지 않게 64바이트 정렬
    std::vector<uint32_t> scratch;
    uint64_t units = 0;
    uint64_t mismatches = 0;
    int64_t end_tick = 0;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

inline int64_t now_ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

}  // namespace

extern "C" {

BENCH_EXPORT const BenchInfo* BENCH_CALL bench_get_info(void) { return &kInfo; }

BENCH_EXPORT int32_t BENCH_CALL bench_init(const BenchConfig* config, BenchContext** out_ctx) {
    if (!config || !out_ctx || config->struct_size < sizeof(BenchConfig) || config->threads == 0 ||
        config->duration_ms == 0)
        return BENCH_E_INVALID_ARG;
    *out_ctx = nullptr;
    try {
        auto* ctx = new BenchContext();
        ctx->cfg = *config;
        ctx->input = intk::make_input(config->seed);
        std::vector<uint32_t> scratch(intk::kSortCount);
        ctx->expected = intk::run_unit(ctx->input, scratch.data());
        *out_ctx = ctx;
        return BENCH_OK;
    } catch (const std::bad_alloc&) {
        return BENCH_E_OUT_OF_MEMORY;
    } catch (...) {
        return BENCH_E_INTERNAL;
    }
}

BENCH_EXPORT int32_t BENCH_CALL bench_run(BenchContext* ctx, BenchResult* out, const volatile int32_t* cancel,
                                          BenchProgressFn progress, void* progress_user) {
    if (!ctx || !out || out->struct_size < sizeof(BenchResult)) return BENCH_E_INVALID_ARG;

    const uint32_t n = ctx->cfg.threads;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const int64_t duration_ticks = int64_t(double(ctx->cfg.duration_ms) * double(freq.QuadPart) / 1000.0);
    const int64_t progress_every = freq.QuadPart / 10;  // 100ms

    std::vector<Worker> workers;
    std::vector<std::thread> threads;
    std::atomic<uint32_t> ready{0};
    std::atomic<int> go{0};
    std::atomic<int> stop{0};
    std::atomic<int> cancelled{0};
    int64_t start_tick = 0;

    try {
        workers.resize(n);
        for (auto& w : workers) w.scratch.resize(intk::kSortCount);
    } catch (const std::bad_alloc&) {
        return BENCH_E_OUT_OF_MEMORY;
    }

    // 1번 이후 워커 스레드: 코어 고정 → 준비 완료 알림 → 출발 신호 대기 → 작업
    auto spawned = [&](uint32_t idx, auto&& work) {
        if (ctx->cfg.thread_init) ctx->cfg.thread_init(idx, ctx->cfg.user);
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (!go.load(std::memory_order_acquire)) YieldProcessor();
        work(idx);
    };

    auto work = [&](uint32_t idx) {
        Worker& me = workers[idx];
        const intk::Input& in = ctx->input;
        const uint64_t expected = ctx->expected;
        int64_t next_progress = start_tick + progress_every;

        while (!stop.load(std::memory_order_relaxed)) {
            uint64_t d = intk::run_unit(in, me.scratch.data());
            if (d != expected) ++me.mismatches;
            ++me.units;

            if (cancel && *cancel) {
                cancelled.store(1);
                stop.store(1, std::memory_order_relaxed);
                break;
            }
            if (idx == 0) {  // 0번 워커가 시간을 보고 전체 종료를 알린다
                int64_t t = now_ticks();
                if (t - start_tick >= duration_ticks) {
                    stop.store(1, std::memory_order_relaxed);
                } else if (progress && t >= next_progress) {
                    progress(double(t - start_tick) / double(duration_ticks), progress_user);
                    next_progress = t + progress_every;
                }
            }
        }
        me.end_tick = now_ticks();
    };

    try {
        threads.reserve(n > 0 ? n - 1 : 0);
        for (uint32_t i = 1; i < n; ++i) threads.emplace_back([&, i] { spawned(i, work); });
    } catch (...) {
        stop.store(1);
        go.store(1, std::memory_order_release);
        for (auto& t : threads) t.join();
        return BENCH_E_INTERNAL;
    }

    // 0번 워커는 호출한 스레드에서 실행한다. 모든 워커가 준비되면 동시에 출발.
    if (ctx->cfg.thread_init) ctx->cfg.thread_init(0, ctx->cfg.user);
    while (ready.load(std::memory_order_acquire) < n - 1) YieldProcessor();
    start_tick = now_ticks();
    go.store(1, std::memory_order_release);
    work(0);
    for (auto& t : threads) t.join();

    int64_t last_end = start_tick;
    uint64_t units = 0, mismatches = 0;
    for (const auto& w : workers) {
        last_end = std::max(last_end, w.end_tick);
        units += w.units;
        mismatches += w.mismatches;
    }
    const double elapsed = double(last_end - start_tick) / double(freq.QuadPart);

    out->threads_used = n;
    out->elapsed_s = elapsed;
    out->iterations = units;
    out->ops_per_sec = elapsed > 0 ? double(units) / elapsed : 0.0;
    out->checksum = ctx->expected;
    out->mismatches = mismatches;
    std::memset(out->variant, 0, sizeof(out->variant));
    std::strncpy(out->variant, "scalar", sizeof(out->variant) - 1);

    if (progress) progress(1.0, progress_user);
    if (cancelled.load()) return BENCH_E_CANCELLED;
    if (mismatches) return BENCH_E_CHECKSUM;
    return BENCH_OK;
}

BENCH_EXPORT void BENCH_CALL bench_shutdown(BenchContext* ctx) { delete ctx; }

}  // extern "C"
