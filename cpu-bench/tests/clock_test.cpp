// 유효 클럭 측정 기본 확인: 덧셈 체인이 정확한 횟수를 실행하고, 측정값이 현실적인 범위인지
#include "clock.h"

#include <cstdio>

int main() {
    int failed = 0;
    const uint64_t r = clock_chain(1000);
    std::printf("[%s] clock_chain(1000) == 100000 (got %llu)\n", r == 100000 ? "PASS" : "FAIL", static_cast<unsigned long long>(r));
    failed += r != 100000;
    const CoreClock c = measure_core_clock(10, 20);
    const bool ok = c.max_ghz > 0.3 && c.max_ghz < 8.0 && c.median_ghz > 0.3 && c.median_ghz <= c.max_ghz;
    std::printf("[%s] measured clock in a plausible range: max %.2f / median %.2f GHz\n", ok ? "PASS" : "FAIL", c.max_ghz, c.median_ghz);
    failed += !ok;
    std::printf("%s\n", failed ? "FAILED" : "ALL PASSED");
    return failed ? 1 : 0;
}
