// 파트 번호 → 정격 속도, 메모리 진단(싱글 채널, XMP/EXPO 꺼짐 의심) 테스트
#include "envcheck.h"
#include "ramspec.h"
#include "smbios.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failed = 0;

static void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failed;
}

static MemoryModule dimm(const char* slot, const char* ch, uint32_t max, uint32_t cfg, const char* part) {
    MemoryModule m;
    m.device_locator = slot;
    m.channel = ch;
    m.size_bytes = 16ull << 30;
    m.max_speed = max;
    m.configured_speed = cfg;
    m.part_number = part;
    return m;
}

static bool has(const std::vector<EnvWarning>& w, const char* id) {
    for (const auto& x : w)
        if (x.id == id) return true;
    return false;
}

int main() {
    struct Case { const char* part; uint32_t mts; bool jedec; };
    const Case cases[] = {
        {"F5-6000J3038F16GX2-TZ5RK", 6000, false},   // G.Skill DDR5
        {"F4-3200C16D-16GVKB", 3200, false},         // G.Skill DDR4
        {"KF560C36BBEK2-32", 6000, false},           // Kingston FURY DDR5-6000
        {"KF432C16BB/8", 3200, false},               // Kingston FURY DDR4-3200
        {"HX432C16PB3K2/16", 3200, false},           // HyperX
        {"CMK32GX5M2B6000C36", 6000, false},         // Corsair DDR5
        {"CMW32GX4M2E3200C16", 3200, false},         // Corsair DDR4
        {"FF3D532G6000HC38ADC01", 6000, false},      // TeamGroup DDR5
        {"TF10D416G3200HC16CDC01", 3200, false},     // TeamGroup DDR4
        {"BL2K16G32C16U4B", 3200, false},            // Crucial Ballistix
        {"CP2K16G56C46U5", 5600, false},             // Crucial Pro DDR5
        {"AX5U6000C3016G-DCLARBK", 6000, false},     // ADATA XPG DDR5
        {"PVV532G600C36K", 6000, false},             // Patriot Viper DDR5
        {"PVS416G320C6K", 3200, false},              // Patriot Viper DDR4
        {"CT16G48C40U5", 4800, true},                // Crucial 기본 모듈
        {"M323R2GA3EB0-CWMOD", 0, true},             // Samsung (개발 PC 실제 값)
        {"HMCG78AGBUA081N", 0, true},                // SK hynix
        {"MTC8C1084S1UC56BD1", 0, true},             // Micron
        {" kf560c36bbek2-32 ", 6000, false},         // 소문자·공백
        {"SOMETHING-ELSE-123", 0, false},            // 모르는 형식
        {"", 0, false},
    };
    for (const auto& c : cases) {
        RatedSpeed r = rated_speed_from_part(c.part);
        char what[160];
        std::snprintf(what, sizeof(what), "%-26s -> %u MT/s%s (got %u%s, %s)", c.part, c.mts, c.jedec ? " jedec" : "", r.mts,
                      r.jedec_only ? " jedec" : "", r.brand);
        check(r.mts == c.mts && r.jedec_only == c.jedec, what);
    }

    // 진단
    MemoryInfo dev;  // 개발 PC 구성: Samsung 5600 x2, A2/B2
    dev.modules = {dimm("DDR5-A2", "A", 5600, 5600, "M323R2GA3EB0-CWMOD"), dimm("DDR5-B2", "B", 5600, 5600, "M323R2GA3EB0-CWMOL")};
    check(evaluate_memory(dev).empty(), "dev PC: dual channel at rated speed -> no warning");

    MemoryInfo xmp_off;
    xmp_off.modules = {dimm("DIMM_A2", "A", 4800, 4800, "F5-6000J3038F16GX2-TZ5RK"), dimm("DIMM_B2", "B", 4800, 4800, "F5-6000J3038F16GX2-TZ5RK")};
    check(has(evaluate_memory(xmp_off), "xmp_off"), "6000 kit at 4800 -> xmp_off");

    MemoryInfo xmp_on = xmp_off;
    for (auto& m : xmp_on.modules) m.configured_speed = 6000;
    check(evaluate_memory(xmp_on).empty(), "6000 kit at 6000 (XMP on) -> no warning");

    MemoryInfo single;
    single.modules = {dimm("DIMM_A2", "A", 5600, 5600, "M323R2GA3EB0-CWMOD")};
    check(has(evaluate_memory(single), "single_channel"), "one module -> single_channel");

    MemoryInfo same;
    same.modules = {dimm("DIMM_A1", "A", 3200, 3200, "KF432C16BB/8"), dimm("DIMM_A2", "A", 3200, 3200, "KF432C16BB/8")};
    check(has(evaluate_memory(same), "same_channel"), "two modules in channel A -> same_channel");

    MemoryInfo slow;
    slow.modules = {dimm("DIMM_A2", "A", 5600, 4800, "M323R2GA3EB0-CWMOD"), dimm("DIMM_B2", "B", 5600, 4800, "M323R2GA3EB0-CWMOD")};
    check(has(evaluate_memory(slow), "below_jedec"), "JEDEC 5600 module at 4800 -> below_jedec");

    std::printf("%s\n", g_failed ? "FAILED" : "ALL PASSED");
    return g_failed ? 1 : 0;
}
