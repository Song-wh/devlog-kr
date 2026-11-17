// SMBIOS 메모리 정보 해석 테스트: 직접 만든 SMBIOS 표를 넣어 필드·채널 판정을 확인한다.
#include "smbios.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failed = 0;

static void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failed;
}

// Type 17 구조체 하나를 만든다. flen은 형식 영역 길이(버전에 따라 다름), 문자열은 1번부터.
static void add_type17(std::vector<uint8_t>& t, uint8_t flen, uint16_t size, uint32_t ext_size, uint8_t type,
                       uint16_t speed, uint16_t configured, const std::vector<std::string>& strs) {
    std::vector<uint8_t> s(flen, 0);
    s[0] = 17;
    s[1] = flen;
    auto w16 = [&](size_t off, uint16_t v) { if (off + 2 <= flen) { s[off] = uint8_t(v); s[off + 1] = uint8_t(v >> 8); } };
    auto w32 = [&](size_t off, uint32_t v) { for (int i = 0; i < 4; ++i) if (off + i < flen) s[off + i] = uint8_t(v >> (8 * i)); };
    w16(0x0C, size);
    s[0x0E] = 0x09;  // DIMM
    s[0x10] = 1;     // device locator
    s[0x11] = 2;     // bank locator
    s[0x12] = type;
    w16(0x15, speed);
    if (flen > 0x17) s[0x17] = 3;  // manufacturer
    if (flen > 0x18) s[0x18] = 4;  // serial: 파서가 읽으면 안 된다
    if (flen > 0x1A) s[0x1A] = 5;  // part number
    if (flen > 0x1B) s[0x1B] = 2;  // rank 2
    w32(0x1C, ext_size);
    w16(0x20, configured);
    t.insert(t.end(), s.begin(), s.end());
    for (const auto& x : strs) {
        t.insert(t.end(), x.begin(), x.end());
        t.push_back(0);
    }
    t.push_back(0);
    if (strs.empty()) t.push_back(0);
}

static std::vector<uint8_t> wrap(const std::vector<uint8_t>& table) {
    std::vector<uint8_t> raw = {0, 3, 6, 0, 0, 0, 0, 0};
    const uint32_t n = uint32_t(table.size());
    std::memcpy(raw.data() + 4, &n, 4);
    raw.insert(raw.end(), table.begin(), table.end());
    return raw;
}

int main() {
    // 채널 이름 읽기
    struct Case { const char* dev; const char* bank; const char* want; };
    const Case cases[] = {
        {"DDR5-A2", "BANK 0", "A"},              // 개발 PC (실제 값)
        {"DIMM_B1", "", "B"},
        {"ChannelA-DIMM0", "BANK 0", "A"},
        {"Controller1-ChannelB-DIMM0", "", "1B"},
        {"DIMM 0", "P0 CHANNEL A", "A"},
        {"A1", "", "A"},
        {"DIMM0", "BANK 0", ""},                 // 알 수 없음
        {"Bottom-Slot 1(top)", "BANK 2", ""},
    };
    for (const auto& c : cases) {
        std::string got = channel_from_locator(c.dev, c.bank);
        check(got == c.want, std::string("channel(\"") + c.dev + "\", \"" + c.bank + "\") == \"" + c.want + "\" (got \"" + got + "\")");
    }

    // SMBIOS 3.x: 빈 슬롯 1개 + DDR5 16GB 2개(A, B), 2.x 짧은 구조체 1개는 별도 표에서
    std::vector<uint8_t> t;
    add_type17(t, 0x5C, 0, 0, 0x02, 0, 0, {"DDR5-A1", "BANK 0"});
    add_type17(t, 0x5C, 16384, 0, 0x22, 5600, 5600, {"DDR5-A2", "BANK 0", "Samsung", "SERIAL123", "M323R2GA3EB0-CWMOD"});
    add_type17(t, 0x5C, 0x7FFF, 32768, 0x22, 4800, 6000, {"DDR5-B2", "BANK 0", "Unknown", "SERIAL456", "KF560C30-32"});
    t.insert(t.end(), {127, 4, 0, 0, 0, 0});  // 표의 끝
    std::vector<uint8_t> raw = wrap(t);
    MemoryInfo mem;
    std::string err;
    check(parse_smbios(raw.data(), raw.size(), mem, err), "parse SMBIOS 3.x table");
    check(mem.modules.size() == 3, "3 memory device entries");
    check(mem.populated() == 2, "2 populated");
    check(mem.total_bytes() == (48ull << 30), "total 48 GiB (16 + extended-size 32)");
    check(mem.channels() == 2, "dual channel from slot names");
    if (mem.modules.size() == 3) {
        const auto& a = mem.modules[1];
        const auto& b = mem.modules[2];
        check(a.max_speed == 5600 && a.configured_speed == 5600, "speeds of module A");
        check(b.max_speed == 4800 && b.configured_speed == 6000, "configured above max (XMP/EXPO on)");
        check(a.manufacturer == "Samsung" && a.part_number == "M323R2GA3EB0-CWMOD" && a.rank == 2, "strings and rank");
        check(b.manufacturer.empty(), "placeholder manufacturer \"Unknown\" becomes empty");
        bool leaked = false;
        for (const auto& m : mem.modules)
            leaked |= m.device_locator.find("SERIAL") != std::string::npos || m.part_number.find("SERIAL") != std::string::npos ||
                      m.manufacturer.find("SERIAL") != std::string::npos;
        check(!leaked, "serial number is never read");
    }

    // SMBIOS 2.3: 형식 영역이 0x1B바이트라 configured speed가 없다. 모듈 1개 = 싱글 채널
    std::vector<uint8_t> t2;
    add_type17(t2, 0x1B, 8192, 0, 0x1A, 3200, 0, {"DIMM0", "BANK 0", "Micron", "S", "MTA8ATF1G64AZ"});
    add_type17(t2, 0x1B, 0, 0, 0x02, 0, 0, {"DIMM1", "BANK 1"});
    std::vector<uint8_t> raw2 = wrap(t2);
    check(parse_smbios(raw2.data(), raw2.size(), mem, err) && mem.modules.size() == 2, "parse short (2.3) structures");
    check(mem.populated() == 1 && mem.channels() == 1, "one module = single channel even without channel names");
    check(mem.modules[0].configured_speed == 0 && mem.modules[0].max_speed == 3200, "missing configured speed stays 0");

    // 두 모듈인데 채널 이름을 못 읽으면 알 수 없음(0)
    std::vector<uint8_t> t3;
    add_type17(t3, 0x28, 8192, 0, 0x1A, 3200, 3200, {"DIMM0", "BANK 0"});
    add_type17(t3, 0x28, 8192, 0, 0x1A, 3200, 3200, {"DIMM1", "BANK 1"});
    std::vector<uint8_t> raw3 = wrap(t3);
    check(parse_smbios(raw3.data(), raw3.size(), mem, err) && mem.channels() == 0, "two modules, unknown slot names -> channels unknown");

    // 잘린 데이터에서 멈추지 않는다
    for (size_t cut = 0; cut < raw.size(); cut += 7) parse_smbios(raw.data(), cut, mem, err);
    check(true, "truncated input does not crash");

    std::printf("%s\n", g_failed ? "FAILED" : "ALL PASSED");
    return g_failed ? 1 : 0;
}
