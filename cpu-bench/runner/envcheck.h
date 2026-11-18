// 측정 전 환경 점검: 전원(AC/배터리), 전원 관리 옵션, 최대 프로세서 상태, 유휴 상태 백그라운드 CPU 사용률.
// 읽기 전용이다. 설정을 바꾸거나 프로세스를 종료하지 않는다. 결과는 PC 밖으로 보내지 않는다.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct MemoryInfo;

struct ProcessLoad {
    std::string name;     // 실행 파일 이름만 (경로·명령줄은 읽지 않음)
    uint32_t pid = 0;
    double cpu_percent = 0;  // 전체 논리 코어 대비 %
};

struct EnvReport {
    // 전원
    bool power_known = false;
    bool on_ac = true;
    bool has_battery = false;
    int battery_percent = -1;
    // 전원 관리 옵션
    std::string scheme_guid;
    std::string scheme_name;   // 표시 이름 (Windows 언어로 나옴 → JSON에만, 콘솔에는 영어 분류)
    std::string scheme_kind;   // "balanced", "high_performance", "power_saver", "ultimate", "custom"
    std::string overlay;       // Windows 10/11 전원 모드: "best_efficiency", "balanced", "best_performance", "unknown"
    int max_processor_state = -1;  // 현재 전원(AC/DC)의 최대 프로세서 상태 %, 100 미만이면 터보가 꺼질 수 있음
    int min_processor_state = -1;
    // 백그라운드 사용률
    double sample_seconds = 0;
    double busy_percent = -1;  // 측정 구간 동안 전체 CPU 사용률 (이 프로세스 제외 근사)
    std::vector<ProcessLoad> top;  // 사용률 상위 프로세스
    double process_percent = 0;    // 모든 프로세스 합. busy_percent와의 차이는 인터럽트·DPC 등 프로세스에 속하지 않는 시간
};

struct EnvWarning {
    std::string id;       // "on_battery", "power_saver", "efficiency_mode", "max_state", "background",
                          // "single_channel", "same_channel", "xmp_off", "below_jedec", "pcie_width", "pcie_gen"
    std::string message;  // 영어 (콘솔 코드 페이지 문제)
};

EnvReport check_environment(double sample_seconds, size_t top_n);
std::vector<EnvWarning> evaluate(const EnvReport& r);

// 메모리 진단: 싱글 채널, 같은 채널에 몰린 모듈, 정격(파트 번호)보다 낮은 설정 속도(XMP/EXPO 꺼짐 의심)
std::vector<EnvWarning> evaluate_memory(const MemoryInfo& m);

// 측정 한 회차 동안 다른 프로그램이 CPU를 얼마나 썼는지 프로세스별로 보기 위한 스냅샷.
// take_process_snapshot()을 회차 앞뒤로 부르고 top_between()으로 상위 n개를 얻는다(이 프로세스는 제외).
struct ProcessSnapshot {
    std::map<uint32_t, std::pair<std::string, uint64_t>> procs;  // pid -> (이름, CPU 시간 100ns)
    uint64_t total = 0;  // 시스템 전체 CPU 시간(모든 논리 코어, kernel+user), 100ns
};
ProcessSnapshot take_process_snapshot();
std::vector<ProcessLoad> top_between(const ProcessSnapshot& a, const ProcessSnapshot& b, size_t top_n);
