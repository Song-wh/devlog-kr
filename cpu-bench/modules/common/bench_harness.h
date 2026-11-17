// 부하 모듈 공통 측정 루프: 워커 스레드 생성, 동시 출발, 시간 측정, 진행률, 취소
//
//   - 워커 1..N-1은 새 스레드, 0번 워커는 bench_run을 호출한 스레드가 맡는다.
//   - 모든 워커가 thread_init(코어 고정)을 마치고 준비되면 동시에 출발한다.
//   - 0번 워커가 QPC로 시간을 보고 전체 종료 신호를 보낸다.
//   - step(idx, stats)는 작업 한 덩어리를 처리하고 stats.ops / stats.mismatches를 올린다.
//     덩어리 하나는 시간 확인 비용이 무시될 만큼 커야 한다(대략 수 µs 이상).
#pragma once

#include "bench_api.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "Synchronization.lib")  // WaitOnAddress

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

namespace bh {

// v1 러너가 준 작은 BenchConfig도 받는다. 없는 필드는 0으로 채운다.
inline bool copy_config(const BenchConfig* in, BenchConfig& out) {
    constexpr size_t kV1Size = offsetof(BenchConfig, workload);
    if (!in || in->struct_size < kV1Size) return false;
    out = BenchConfig{};
    std::memcpy(&out, in, std::min<size_t>(in->struct_size, sizeof(BenchConfig)));
    out.struct_size = sizeof(BenchConfig);
    return out.threads > 0 && out.duration_ms > 0;
}

inline bool result_size_ok(const BenchResult* out) {
    return out && out->struct_size >= offsetof(BenchResult, working_set_bytes);
}

inline int64_t now_ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // 정렬 때문에 생기는 패딩은 의도한 것
#endif
struct alignas(64) WorkerStats {  // 스레드끼리 같은 캐시 라인을 쓰지 않게 64바이트 정렬
    uint64_t ops = 0;
    uint64_t mismatches = 0;
    int64_t end_tick = 0;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

struct RunTotals {
    uint64_t ops = 0;
    uint64_t mismatches = 0;
    double elapsed_s = 0;
    bool cancelled = false;
};

template <class Step>
int32_t run_workers(const BenchConfig& cfg, const volatile int32_t* cancel, BenchProgressFn progress,
                    void* progress_user, Step&& step, RunTotals& totals) {
    const uint32_t n = cfg.threads;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const int64_t duration_ticks = int64_t(double(cfg.duration_ms) * double(freq.QuadPart) / 1000.0);
    const int64_t progress_every = freq.QuadPart / 10;  // 100ms

    std::vector<WorkerStats> stats;
    std::vector<std::thread> threads;
    // 출발 대기는 바쁜 대기(spin)가 아니라 WaitOnAddress로 잠들어서 기다린다.
    // 코어에 고정된 HIGH 우선순위 스레드가 spin하면 아직 실행되지 못한 워커가
    // 수십 초씩 CPU를 못 받는 일이 실제로 있었다(16스레드, 약 10회 중 1회).
    volatile LONG ready = 0;
    volatile LONG go = 0;
    std::atomic<int> stop{0};
    std::atomic<int> cancelled{0};
    int64_t start_tick = 0;

    try {
        stats.resize(n);
    } catch (...) {
        return BENCH_E_OUT_OF_MEMORY;
    }

    auto work = [&](uint32_t idx) {
        WorkerStats& me = stats[idx];
        int64_t next_progress = start_tick + progress_every;
        while (!stop.load(std::memory_order_relaxed)) {
            step(idx, me);
            if (cancel && *cancel) {
                cancelled.store(1);
                stop.store(1, std::memory_order_relaxed);
                break;
            }
            if (idx == 0) {
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

    auto release_all = [&] {
        InterlockedExchange(&go, 1);
        WakeByAddressAll(const_cast<LONG*>(&go));
    };

    // 1번 이후 워커: 코어 고정 → 준비 완료 알림 → 출발 신호 대기 → 작업
    auto spawned = [&](uint32_t idx) {
        if (cfg.thread_init) cfg.thread_init(idx, cfg.user);
        InterlockedIncrement(&ready);
        WakeByAddressSingle(const_cast<LONG*>(&ready));
        LONG zero = 0;
        while (InterlockedCompareExchange(&go, 0, 0) == 0)
            WaitOnAddress(const_cast<LONG*>(&go), &zero, sizeof(LONG), INFINITE);
        if (!stop.load(std::memory_order_relaxed)) work(idx);
    };

    try {
        threads.reserve(n - 1);
        for (uint32_t i = 1; i < n; ++i) threads.emplace_back([&, i] { spawned(i); });
    } catch (...) {
        stop.store(1);
        release_all();
        for (auto& t : threads) t.join();
        return BENCH_E_INTERNAL;
    }

    if (cfg.thread_init) cfg.thread_init(0, cfg.user);
    for (LONG cur; (cur = InterlockedCompareExchange(&ready, 0, 0)) < LONG(n - 1);)
        WaitOnAddress(const_cast<LONG*>(&ready), &cur, sizeof(LONG), INFINITE);
    start_tick = now_ticks();
    release_all();
    work(0);
    for (auto& t : threads) t.join();

    int64_t last_end = start_tick;
    totals = RunTotals{};
    for (const auto& s : stats) {
        last_end = std::max(last_end, s.end_tick);
        totals.ops += s.ops;
        totals.mismatches += s.mismatches;
    }
    totals.elapsed_s = double(last_end - start_tick) / double(freq.QuadPart);
    totals.cancelled = cancelled.load() != 0;
    if (progress) progress(1.0, progress_user);
    return BENCH_OK;
}

// 결과 기록. v1 러너(작은 BenchResult)에는 v2 필드를 쓰지 않는다.
inline void fill_result(BenchResult* out, uint32_t threads, const RunTotals& t, uint64_t checksum,
                        const char* variant, uint64_t working_set_bytes, const char* unit_name) {
    out->threads_used = threads;
    out->elapsed_s = t.elapsed_s;
    out->iterations = t.ops;
    out->ops_per_sec = t.elapsed_s > 0 ? double(t.ops) / t.elapsed_s : 0.0;
    out->checksum = checksum;
    out->mismatches = t.mismatches;
    std::memset(out->variant, 0, sizeof(out->variant));
    std::strncpy(out->variant, variant, sizeof(out->variant) - 1);
    if (out->struct_size >= sizeof(BenchResult)) {
        out->working_set_bytes = working_set_bytes;
        std::memset(out->unit_name, 0, sizeof(out->unit_name));
        if (unit_name) std::strncpy(out->unit_name, unit_name, sizeof(out->unit_name) - 1);
    }
}

inline int32_t status_of(const RunTotals& t) {
    if (t.cancelled) return BENCH_E_CANCELLED;
    if (t.mismatches) return BENCH_E_CHECKSUM;
    return BENCH_OK;
}

}  // namespace bh
