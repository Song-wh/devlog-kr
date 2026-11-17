#include "ramspec.h"

#include <cctype>
#include <regex>

namespace {

struct Rule {
    const char* brand;
    std::regex re;       // 첫 번째 캡처 그룹 = 속도 숫자
    uint32_t multiplier; // 숫자 × multiplier = MT/s (3자리 표기는 10)
    bool jedec_only;
};

const Rule* rules(size_t& n) {
    // 모두 대문자로 바꾼 파트 번호에 적용한다.
    static const Rule kRules[] = {
        // G.Skill: F5-6000J3038F16GX2-TZ5RK, F4-3200C16D-16GVKB
        {"G.Skill", std::regex(R"(^F[345]-(\d{4})[A-Z])"), 1, false},
        // Kingston FURY: KF560C36BBEK2-32 (DDR5-6000), KF432C16BB/8 (DDR4-3200) — 세대 숫자 + 속도/100
        // HyperX: HX432C16PB3K2/16 (DDR4-3200)
        {"Kingston FURY", std::regex(R"(^KF[345](\d{2})C\d{2})"), 100, false},
        {"HyperX", std::regex(R"(^HX[345](\d{2})C\d{2})"), 100, false},
        // Corsair: CMK32GX5M2B6000C36, CMW32GX4M2E3200C16
        {"Corsair", std::regex(R"(^CM[A-Z0-9]*?X[45]M\d[A-Z](\d{4})C\d{2})"), 1, false},
        // TeamGroup T-Force/Elite: FF3D532G6000HC38ADC01, TF10D416G3200HC16CDC01
        {"TeamGroup", std::regex(R"(^(?:FF|TF|TED|TLZ|TDP)[A-Z0-9]*?G(\d{4})HC\d{2})"), 1, false},
        // Crucial Ballistix: BL2K16G32C16U4B / Crucial Pro DDR5: CP2K16G56C46U5
        {"Crucial", std::regex(R"(^(?:BL|CP)(?:\dK)?\d+G(\d{2})C\d{2}[A-Z])"), 100, false},
        // ADATA XPG: AX5U6000C3016G-DCLARBK, AX4U320016G16A-ST50
        {"ADATA XPG", std::regex(R"(^AX[45]U(\d{4}))"), 1, false},
        // Patriot Viper: PVV532G600C36K, PVS416G320C6K (3자리 = 10 MT/s 단위)
        {"Patriot", std::regex(R"(^PV[A-Z]*\d{3}G(\d{3})C\d)"), 10, false},
        // Crucial 기본 모듈: CT16G48C40U5, CT8G4DFRA32A — JEDEC
        {"Crucial (JEDEC)", std::regex(R"(^CT\d+G(\d{2})C\d{2})"), 100, true},
        // 제조사 기본 모듈(JEDEC): Samsung M3xx, SK hynix HMxx, Micron MTxx. 속도는 SMBIOS 값을 쓴다
        {"Samsung (JEDEC)", std::regex(R"(^M[34]\d{2}[A-Z]\d)"), 0, true},
        {"SK hynix (JEDEC)", std::regex(R"(^HM[ACT][A-Z0-9]{3})"), 0, true},
        {"Micron (JEDEC)", std::regex(R"(^MT[ACT]\d)"), 0, true},
    };
    n = sizeof(kRules) / sizeof(kRules[0]);
    return kRules;
}

}  // namespace

RatedSpeed rated_speed_from_part(const std::string& part_number) {
    std::string p;
    for (char c : part_number)
        if (!std::isspace(static_cast<unsigned char>(c))) p += char(std::toupper(static_cast<unsigned char>(c)));
    size_t n = 0;
    const Rule* r = rules(n);
    for (size_t i = 0; i < n; ++i) {
        std::smatch m;
        if (!std::regex_search(p, m, r[i].re)) continue;
        RatedSpeed out;
        out.brand = r[i].brand;
        out.jedec_only = r[i].jedec_only;
        if (r[i].multiplier && m.size() > 1) out.mts = uint32_t(std::stoul(m[1].str())) * r[i].multiplier;
        return out;
    }
    return {};
}
