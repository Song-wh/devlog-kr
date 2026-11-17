#include "smbios.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

namespace {

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    size_t e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// 구조체의 문자열 영역에서 n번째(1부터) 문자열. 0이면 빈 문자열.
std::string string_at(const std::vector<std::string>& strs, uint8_t n) {
    return n && n <= strs.size() ? trim(strs[n - 1]) : std::string();
}

// 제조사가 비워 둔 값 대신 넣는 문구는 빈 값으로 본다
std::string clean(const std::string& s) {
    const std::string u = upper(s);
    static const char* kPlaceholders[] = {"UNKNOWN", "NOT SPECIFIED", "TO BE FILLED BY O.E.M.", "NONE", "N/A", "UNDEFINED"};
    for (const char* p : kPlaceholders)
        if (u == p) return {};
    return s;
}

// s[i]부터 구분자( 공백 _ - : )를 건너뛴 위치
size_t skip_sep(const std::string& s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '_' || s[i] == '-' || s[i] == ':')) ++i;
    return i;
}

}  // namespace

std::string channel_from_locator(const std::string& device, const std::string& bank) {
    for (const std::string& raw : {device, bank}) {
        const std::string s = upper(raw);
        std::string ctrl;
        size_t c = s.find("CONTROLLER");
        if (c != std::string::npos) {
            size_t i = skip_sep(s, c + 10);
            if (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ctrl = s.substr(i, 1);
        }
        // "CHANNEL A", "CHANNELB", "P0 CHANNEL A"
        size_t ch = s.find("CHANNEL");
        if (ch != std::string::npos) {
            size_t i = skip_sep(s, ch + 7);
            if (i < s.size() && std::isalnum(static_cast<unsigned char>(s[i]))) return ctrl + s.substr(i, 1);
        }
        // "DIMM_A1", "DIMM A2", "DIMMB1"
        size_t d = s.find("DIMM");
        if (d != std::string::npos) {
            size_t i = skip_sep(s, d + 4);
            if (i + 1 < s.size() && std::isalpha(static_cast<unsigned char>(s[i])) &&
                std::isdigit(static_cast<unsigned char>(s[i + 1])))
                return ctrl + s.substr(i, 1);
        }
    }
    // 슬롯 이름이 글자+숫자로 끝나는 경우: "A1", "DDR5-A2", "XMM_B1"
    const std::string s = upper(trim(device));
    const size_t n = s.size();
    if (n >= 2 && std::isalpha(static_cast<unsigned char>(s[n - 2])) && std::isdigit(static_cast<unsigned char>(s[n - 1])) &&
        (n == 2 || s[n - 3] == '-' || s[n - 3] == '_' || s[n - 3] == ' '))
        return s.substr(n - 2, 1);
    return {};
}

size_t MemoryInfo::populated() const {
    return size_t(std::count_if(modules.begin(), modules.end(), [](const MemoryModule& m) { return m.size_bytes > 0; }));
}

uint64_t MemoryInfo::total_bytes() const {
    uint64_t t = 0;
    for (const auto& m : modules) t += m.size_bytes;
    return t;
}

unsigned MemoryInfo::channels() const {
    std::set<std::string> ch;
    size_t n = 0;
    for (const auto& m : modules) {
        if (!m.size_bytes) continue;
        ++n;
        if (m.channel.empty()) return n == 1 && populated() == 1 ? 1 : 0;
        ch.insert(m.channel);
    }
    if (n == 1) return 1;
    return unsigned(ch.size());
}

bool parse_smbios(const uint8_t* raw, size_t len, MemoryInfo& out, std::string& error) {
    out = MemoryInfo{};
    // RawSMBIOSData: Used20CallingMethod, Major, Minor, DmiRevision, DWORD Length, 표
    if (len < 8) {
        error = "SMBIOS data too short";
        return false;
    }
    out.smbios_major = raw[1];
    out.smbios_minor = raw[2];
    const size_t table_len = std::min<size_t>(rd32(raw + 4), len - 8);
    const uint8_t* p = raw + 8;
    const uint8_t* end = p + table_len;

    while (p + 4 <= end) {
        const uint8_t type = p[0], flen = p[1];
        if (flen < 4 || p + flen > end) break;
        // 문자열 영역: 0으로 끝나는 문자열들, 마지막은 0 두 개
        std::vector<std::string> strs;
        const uint8_t* s = p + flen;
        while (s < end && *s) {
            const uint8_t* z = static_cast<const uint8_t*>(std::memchr(s, 0, size_t(end - s)));
            if (!z) break;
            strs.emplace_back(reinterpret_cast<const char*>(s), size_t(z - s));
            s = z + 1;
        }
        const uint8_t* next = (s < end && !*s && s == p + flen) ? s + 2 : s + 1;  // 문자열이 없으면 0 두 개

        if (type == 17 && flen >= 0x15) {
            MemoryModule m;
            auto has = [&](size_t off, size_t n) { return off + n <= flen; };
            const uint16_t size = rd16(p + 0x0C);
            if (size == 0x7FFF && has(0x1C, 4)) m.size_bytes = uint64_t(rd32(p + 0x1C) & 0x7FFFFFFF) << 20;
            else if (size != 0 && size != 0xFFFF) m.size_bytes = (size & 0x8000) ? uint64_t(size & 0x7FFF) << 10 : uint64_t(size) << 20;
            m.form_factor = p[0x0E];
            m.device_locator = string_at(strs, p[0x10]);
            m.bank_locator = string_at(strs, p[0x11]);
            m.memory_type = p[0x12];
            if (has(0x15, 2)) {
                const uint16_t sp = rd16(p + 0x15);
                m.max_speed = (sp == 0xFFFF && has(0x54, 4)) ? rd32(p + 0x54) : (sp == 0xFFFF ? 0 : sp);
            }
            if (has(0x17, 1)) m.manufacturer = clean(string_at(strs, p[0x17]));
            // 0x18 시리얼 번호, 0x19 자산 태그: 읽지 않는다
            if (has(0x1A, 1)) m.part_number = clean(string_at(strs, p[0x1A]));
            if (has(0x1B, 1)) m.rank = p[0x1B] & 0x0F;
            if (has(0x20, 2)) {
                const uint16_t cs = rd16(p + 0x20);
                m.configured_speed = (cs == 0xFFFF && has(0x58, 4)) ? rd32(p + 0x58) : (cs == 0xFFFF ? 0 : cs);
            }
            m.channel = channel_from_locator(m.device_locator, m.bank_locator);
            out.modules.push_back(std::move(m));
        }
        if (type == 127) break;  // 표의 끝
        p = next;
    }
    return true;
}

bool query_memory(MemoryInfo& out, std::string& error) {
    const DWORD sig = (DWORD('R') << 24) | (DWORD('S') << 16) | (DWORD('M') << 8) | DWORD('B');
    UINT n = GetSystemFirmwareTable(sig, 0, nullptr, 0);
    if (n == 0) {
        error = "GetSystemFirmwareTable(RSMB) failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    std::vector<uint8_t> buf(n);
    if (GetSystemFirmwareTable(sig, 0, buf.data(), n) != n) {
        error = "GetSystemFirmwareTable(RSMB) returned a different size";
        return false;
    }
    return parse_smbios(buf.data(), buf.size(), out, error);
}

const char* memory_type_name(uint8_t code) {
    switch (code) {
        case 0x18: return "DDR3";
        case 0x1A: return "DDR4";
        case 0x1B: return "LPDDR";
        case 0x1C: return "LPDDR2";
        case 0x1D: return "LPDDR3";
        case 0x1E: return "LPDDR4";
        case 0x22: return "DDR5";
        case 0x23: return "LPDDR5";
        default: return "unknown";
    }
}

const char* form_factor_name(uint8_t code) {
    switch (code) {
        case 0x09: return "DIMM";
        case 0x0D: return "SODIMM";
        case 0x0B: return "Row of chips";
        case 0x10: return "FB-DIMM";
        case 0x11: return "Die";
        default: return "unknown";
    }
}
