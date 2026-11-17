#include "clock.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <vector>

double measure_clock_ghz(uint64_t adds) {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    const uint64_t iterations = std::max<uint64_t>(1, adds / 100);
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    volatile uint64_t sink = clock_chain(iterations);  // 결과를 써서 호출이 지워지지 않게
    (void)sink;
    QueryPerformanceCounter(&t1);
    const double sec = double(t1.QuadPart - t0.QuadPart) / freq;
    return sec > 0 ? double(iterations * 100) / sec / 1e9 : 0.0;
}

CoreClock measure_core_clock(double warm_ms, double sample_ms) {
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    auto ms_since = [&](const LARGE_INTEGER& from) {
        QueryPerformanceCounter(&t);
        return double(t.QuadPart - from.QuadPart) * 1000.0 / double(f.QuadPart);
    };
    QueryPerformanceCounter(&t0);
    while (ms_since(t0) < warm_ms) measure_clock_ghz();
    std::vector<double> v;
    QueryPerformanceCounter(&t0);
    while (ms_since(t0) < sample_ms) v.push_back(measure_clock_ghz());
    CoreClock c;
    if (v.empty()) return c;
    std::sort(v.begin(), v.end());
    c.max_ghz = v.back();
    c.median_ghz = v[v.size() / 2];
    return c;
}
