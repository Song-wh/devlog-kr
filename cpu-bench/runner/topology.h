// CPU 토폴로지: 논리 프로세서, 물리 코어, P/E 코어(EfficiencyClass), 캐시, 프로세서 그룹
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct LogicalCpu {
    uint16_t group = 0;        // 프로세서 그룹 (64개 초과 시스템)
    uint8_t number = 0;        // 그룹 안에서의 번호
    uint32_t core = 0;         // 물리 코어 인덱스
    uint8_t efficiency = 0;    // 클수록 성능 코어. 하이브리드가 아니면 모두 0
    bool first_in_core = false;  // SMT 형제 중 첫 번째
};

struct CacheInfo {
    uint8_t level = 0;
    uint32_t size_bytes = 0;
    const char* type = "";     // "data", "instruction", "unified"
    uint32_t count = 0;        // 같은 종류 캐시 개수
};

struct Topology {
    std::string brand;         // CPUID 브랜드 문자열
    std::vector<LogicalCpu> cpus;
    std::vector<CacheInfo> caches;
    uint32_t cores = 0;
    uint32_t groups = 0;
    uint8_t max_efficiency = 0;
    bool hybrid = false;       // EfficiencyClass가 둘 이상
    uint32_t perf_cores = 0;   // max_efficiency인 물리 코어 수
};

bool query_topology(Topology& out, std::string& error);

// 워커 배치 순서: 성능 코어의 첫 논리 CPU → 나머지 코어의 첫 논리 CPU → SMT 형제
std::vector<LogicalCpu> placement_order(const Topology& topo);

bool pin_current_thread(const LogicalCpu& cpu);
