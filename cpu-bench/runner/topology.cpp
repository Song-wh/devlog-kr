#include "topology.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <tuple>

static std::string cpu_brand() {
    int regs[4] = {};
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000004u) return "unknown";
    char brand[49] = {};
    for (int i = 0; i < 3; ++i) {
        __cpuid(regs, 0x80000002 + i);
        std::memcpy(brand + 16 * i, regs, 16);
    }
    std::string s(brand);
    size_t b = s.find_first_not_of(' ');
    size_t e = s.find_last_not_of(' ');
    return b == std::string::npos ? "unknown" : s.substr(b, e - b + 1);
}

bool query_topology(Topology& out, std::string& error) {
    out = Topology{};
    out.brand = cpu_brand();

    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        error = "GetLogicalProcessorInformationEx size query failed";
        return false;
    }
    std::vector<uint8_t> buf(len);
    auto* base = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (!GetLogicalProcessorInformationEx(RelationAll, base, &len)) {
        error = "GetLogicalProcessorInformationEx failed";
        return false;
    }

    std::map<std::tuple<int, uint32_t, int>, CacheInfo> caches;  // (level, size, type) -> info
    uint32_t core_index = 0;
    for (DWORD off = 0; off < len;) {
        auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (info->Relationship == RelationProcessorCore) {
            const PROCESSOR_RELATIONSHIP& p = info->Processor;
            bool first = true;
            for (WORD g = 0; g < p.GroupCount; ++g) {
                KAFFINITY mask = p.GroupMask[g].Mask;
                for (uint8_t bit = 0; bit < sizeof(KAFFINITY) * 8; ++bit) {
                    if (!(mask & (KAFFINITY(1) << bit))) continue;
                    LogicalCpu c;
                    c.group = p.GroupMask[g].Group;
                    c.number = bit;
                    c.core = core_index;
                    c.efficiency = p.EfficiencyClass;
                    c.first_in_core = first;
                    first = false;
                    out.cpus.push_back(c);
                }
            }
            ++core_index;
        } else if (info->Relationship == RelationCache) {
            const CACHE_RELATIONSHIP& c = info->Cache;
            int type = static_cast<int>(c.Type);
            auto key = std::make_tuple(int(c.Level), uint32_t(c.CacheSize), type);
            CacheInfo& ci = caches[key];
            ci.level = c.Level;
            ci.size_bytes = c.CacheSize;
            ci.type = c.Type == CacheData ? "data" : c.Type == CacheInstruction ? "instruction" : "unified";
            ++ci.count;
        } else if (info->Relationship == RelationGroup) {
            out.groups = info->Group.ActiveGroupCount;
        }
        off += info->Size;
    }

    out.cores = core_index;
    for (auto& kv : caches)
        if (std::strcmp(kv.second.type, "instruction") != 0) out.caches.push_back(kv.second);

    uint8_t min_eff = 255;
    for (const auto& c : out.cpus) {
        out.max_efficiency = std::max(out.max_efficiency, c.efficiency);
        min_eff = std::min(min_eff, c.efficiency);
    }
    out.hybrid = !out.cpus.empty() && min_eff != out.max_efficiency;
    for (const auto& c : out.cpus)
        if (c.first_in_core && c.efficiency == out.max_efficiency) ++out.perf_cores;

    if (out.cpus.empty()) {
        error = "no logical processors reported";
        return false;
    }
    return true;
}

std::vector<LogicalCpu> placement_order(const Topology& topo) {
    std::vector<LogicalCpu> order = topo.cpus;
    std::stable_sort(order.begin(), order.end(), [](const LogicalCpu& a, const LogicalCpu& b) {
        if (a.first_in_core != b.first_in_core) return a.first_in_core;  // 물리 코어 먼저, SMT 형제는 뒤
        return a.efficiency > b.efficiency;                               // 성능 코어 먼저
    });
    return order;
}

bool pin_current_thread(const LogicalCpu& cpu) {
    GROUP_AFFINITY ga{};
    ga.Group = cpu.group;
    ga.Mask = KAFFINITY(1) << cpu.number;
    return SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr) != 0;
}
