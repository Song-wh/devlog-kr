#include "throttle.h"

#include "clock.h"
#include "int_kernels.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>

namespace {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct alignas(64) WorkerData {
    std::vector<uint64_t> units;                  // 초별 완료 단위 수 (자기 스레드만 씀)
    std::vector<std::pair<double, double>> clock; // (경과 초, GHz)
    bool perf = true;
    std::atomic<uint64_t> live{0};                // 진행 표시용 누적 단위 수
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// 중앙값: 측정 도중 다른 프로그램에 잠깐 밀린 회차(낮게 나옴)에 덜 흔들린다
double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double avg(const std::vector<double>& v) {
    double s = 0;
    for (double x : v) s += x;
    return v.empty() ? 0.0 : s / double(v.size());
}

double pct(double from, double to) { return from > 0 ? 100.0 * (to - from) / from : 0.0; }

}  // namespace

ThrottleResult run_throttle(const Topology& topo, unsigned threads, unsigned seconds,
                            void (*progress)(unsigned, double, void*), void* user) {
    ThrottleResult r;
    r.threads = threads;
    r.seconds = seconds;
    r.hybrid = topo.hybrid;

    const std::vector<LogicalCpu> order = placement_order(topo);
    const intk::Input input = intk::make_input(1);

    LARGE_INTEGER freq, t0;
    QueryPerformanceFrequency(&freq);
    std::vector<WorkerData> data(threads);
    for (unsigned i = 0; i < threads; ++i) {
        data[i].units.assign(seconds + 2, 0);
        data[i].clock.reserve(size_t(seconds) * 5 + 8);
        data[i].perf = !topo.hybrid || order[i % order.size()].efficiency == topo.max_efficiency;
    }

    // 출발 신호는 이벤트로 (spin 대기 금지: 버그 001)
    HANDLE go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> stop{false};
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
        pool.emplace_back([&, i] {
            pin_current_thread(order[i % order.size()]);
            std::vector<uint32_t> scratch(intk::kSortCount);
            WaitForSingleObject(go, INFINITE);
            WorkerData& me = data[i];
            const int64_t slice = freq.QuadPart / 4;  // 0.25초마다 클럭 측정
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            int64_t next_clock = now.QuadPart + slice / 2 + int64_t(i) * slice / std::max(1u, threads);  // 측정 시점을 스레드마다 엇갈리게
            uint64_t sink = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                sink += intk::run_unit(input, scratch.data());
                QueryPerformanceCounter(&now);
                const int64_t el = now.QuadPart - t0.QuadPart;
                const size_t sec = size_t(el / freq.QuadPart);
                if (sec < me.units.size()) ++me.units[sec];
                me.live.fetch_add(1, std::memory_order_relaxed);
                if (now.QuadPart >= next_clock) {
                    const double ghz = measure_clock_ghz();
                    me.clock.emplace_back(double(el) / double(freq.QuadPart), ghz);
                    next_clock = now.QuadPart + slice;
                }
            }
            if (sink == 42) std::printf(" ");  // 계산 결과를 쓰는 척해서 제거되지 않게
        });
    }

    Sleep(50);  // 스레드가 코어 고정을 마칠 시간
    QueryPerformanceCounter(&t0);
    SetEvent(go);
    uint64_t last_total = 0;
    for (unsigned s = 1; s <= seconds; ++s) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const int64_t target = t0.QuadPart + int64_t(s) * freq.QuadPart;
        if (target > now.QuadPart) Sleep(DWORD((target - now.QuadPart) * 1000 / freq.QuadPart));
        uint64_t total = 0;
        for (const auto& d : data) total += d.live.load(std::memory_order_relaxed);
        if (progress) progress(s, double(total - last_total), user);
        last_total = total;
    }
    stop.store(true);
    for (auto& t : pool) t.join();
    CloseHandle(go);

    // 초별 집계
    r.timeline.resize(seconds);
    for (unsigned s = 0; s < seconds; ++s) {
        std::vector<double> p, e;
        double units = 0;
        for (const auto& d : data) {
            units += double(d.units[s]);
            for (const auto& [t, g] : d.clock)
                if (t >= s && t < s + 1) (d.perf ? p : e).push_back(g);
        }
        r.timeline[s] = {units, median(p), median(e)};
    }

    // 시작 구간: 2~7초 (처음 2초는 깨어나는 구간), 끝 구간: 마지막 1/3 (최대 10초)
    // 구간 클럭 = 그 구간 모든 표본의 중앙값, 처리량 = 초별 처리량의 평균
    auto window = [&](unsigned from, unsigned to) {
        ThrottleSecond w;
        std::vector<double> u, p, e;
        for (unsigned s = from; s < std::min(to, seconds); ++s) u.push_back(r.timeline[s].units_per_sec);
        for (const auto& d : data)
            for (const auto& [t, g] : d.clock)
                if (t >= from && t < std::min(to, seconds)) (d.perf ? p : e).push_back(g);
        w.units_per_sec = avg(u);
        w.p_ghz = median(p);
        w.e_ghz = median(e);
        return w;
    };
    const unsigned head_from = std::min(2u, seconds / 4), head_to = std::min(7u, std::max(head_from + 1, seconds / 3));
    const unsigned tail_len = std::max(1u, std::min(10u, seconds / 3));
    r.start = window(head_from, head_to);
    r.end = window(seconds - tail_len, seconds);
    r.throughput_change = pct(r.start.units_per_sec, r.end.units_per_sec);
    r.p_change = pct(r.start.p_ghz, r.end.p_ghz);
    r.e_change = pct(r.start.e_ghz, r.end.e_ghz);
    r.throttled = r.throughput_change < -kThrottleWarnPercent || r.p_change < -kThrottleWarnPercent;
    for (unsigned t = head_to; t < seconds; ++t) {
        if (r.p_drop_at < 0 && r.start.p_ghz > 0 && r.timeline[t].p_ghz > 0 && r.timeline[t].p_ghz < r.start.p_ghz * 0.97)
            r.p_drop_at = int(t);
        if (r.e_drop_at < 0 && r.start.e_ghz > 0 && r.timeline[t].e_ghz > 0 && r.timeline[t].e_ghz < r.start.e_ghz * 0.97)
            r.e_drop_at = int(t);
    }
    return r;
}
