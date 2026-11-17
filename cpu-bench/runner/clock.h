// 유효 클럭 추정: 1사이클짜리 의존 덧셈 체인(clockchain.asm)의 실행 시간으로 지금 이 코어의 실제 클럭을 잰다.
// 드라이버 없이 동작한다. 터보·절전·쓰로틀링이 모두 반영된 "지금 실제로 도는 속도"다.
#pragma once

#include <cstdint>

extern "C" uint64_t clock_chain(uint64_t iterations);  // 100 adds × iterations

// adds개의 덧셈을 실행하고 GHz를 돌려준다. 기본 200만 개 ≈ 0.4 ms (5 GHz 기준).
double measure_clock_ghz(uint64_t adds = 2000000);

struct CoreClock {
    double max_ghz = 0;     // 측정 구간 최댓값 (그 순간 낼 수 있는 최고 터보)
    double median_ghz = 0;  // 측정 구간 중앙값 (보통 상태)
};

// 지금 스레드가 도는 코어의 클럭. 먼저 warm_ms 동안 부하를 걸어 절전 상태에서 깨운 뒤
// sample_ms 동안 짧은 측정을 반복한다.
// 쉬던 코어는 부하가 걸린 뒤 최고 클럭까지 5~13 ms가 걸린다(버그 003: 0.4 ms 워밍업으로는 올라가는 중간을 쟀다).
CoreClock measure_core_clock(double warm_ms = 30, double sample_ms = 100);
