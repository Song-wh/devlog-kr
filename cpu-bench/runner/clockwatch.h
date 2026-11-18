// 측정 "도중" 한 코어의 평균 유효 클럭: Windows 성능 카운터(작업 관리자가 쓰는 것과 같은 값).
//   유효 클럭(GHz) = Processor Frequency(MHz, 기준) × % Processor Performance / 100 / 1000
// % Processor Performance는 비율 카운터라 begin()과 end() 두 번 읽은 사이의 평균이 나온다.
// 덧셈 체인(clock.h)은 측정이 끝난 "뒤"에만 잴 수 있었다(조사 006). 이건 측정과 같은 구간을 본다.
// 드라이버·관리자 권한 불필요. 카운터를 못 열면 0을 돌려준다.
#pragma once

#include <cstdint>

class ClockWatch {
public:
    ClockWatch() = default;
    ClockWatch(const ClockWatch&) = delete;
    ClockWatch& operator=(const ClockWatch&) = delete;
    ~ClockWatch();

    // 프로세서 그룹·번호의 카운터를 연다(측정 전에 한 번, 시간이 조금 걸린다)
    bool open(uint16_t group, uint8_t number);
    void begin();      // 회차 시작 직전
    double end_ghz();  // 회차 끝 직후: begin() 이후 평균 유효 클럭, 실패하면 0

private:
    void* query_ = nullptr;
    void* perf_ = nullptr;
    void* freq_ = nullptr;
};
