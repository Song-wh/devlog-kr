// 짧은 쓰로틀링 검사: 모든(또는 N개) 논리 코어에 정수 부하를 걸어 두고, 시간에 따라
// 처리량과 유효 클럭이 떨어지는지 본다. 쿨러 부족·전원 제한(PL1/PL2)·VRM 과열이 있으면 클럭이 내려간다.
//
// 부하: 정수 모듈과 같은 작업 단위(intk::run_unit). 각 스레드는 약 0.25초마다 덧셈 체인으로
// 자기 코어의 클럭을 잰다(약 0.5 ms, 부하 사이에 끼워 넣으므로 코어는 계속 바쁜 상태).
#pragma once

#include "topology.h"

#include <string>
#include <vector>

struct ThrottleSecond {
    double units_per_sec = 0;  // 그 1초 동안 전체 스레드가 끝낸 작업 단위
    double p_ghz = 0;          // 성능 코어(또는 하이브리드가 아니면 전체) 평균 클럭, 표본이 없으면 0
    double e_ghz = 0;          // 효율 코어 평균 클럭 (하이브리드만)
};

struct ThrottleResult {
    unsigned threads = 0;
    unsigned seconds = 0;
    bool hybrid = false;
    std::vector<ThrottleSecond> timeline;  // 초 단위
    // 시작 구간(2~7초)과 끝 구간(마지막 1/3, 최대 10초) 평균
    ThrottleSecond start, end;
    double throughput_change = 0;  // % (음수 = 감소)
    double p_change = 0, e_change = 0;
    int p_drop_at = -1, e_drop_at = -1;  // 클럭이 시작 구간보다 3% 넘게 처음 내려간 초 (-1 = 없음)
    // 판정: 실제 성능(처리량)이나 성능 코어 클럭이 kThrottleWarnPercent 넘게 떨어지면 경고.
    // 효율 코어만 내려가는 건 CPU 전원 제한(일정 시간 뒤 PL2 → PL1) 같은 정상 동작일 수 있어 정보로만 알린다.
    bool throttled = false;
};

constexpr double kThrottleWarnPercent = 5.0;

// progress: 매초 호출 (경과 초, 그 초의 처리량). nullptr 가능.
ThrottleResult run_throttle(const Topology& topo, unsigned threads, unsigned seconds,
                            void (*progress)(unsigned sec, double units_per_sec, void* user) = nullptr,
                            void* user = nullptr);
