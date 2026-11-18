#include "envcheck.h"
#include "ramspec.h"
#include "smbios.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <powrprof.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#pragma comment(lib, "powrprof.lib")

namespace {

constexpr double kBackgroundWarnPercent = 3.0;

std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string guid_str(const GUID& g) {
    char b[40];
    std::snprintf(b, sizeof(b), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

uint64_t ft64(const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

// Windows 기본 전원 관리 옵션 GUID
const char* scheme_kind(const std::string& g) {
    if (g == "381b4222-f694-41f0-9685-ff5bb260df2e") return "balanced";
    if (g == "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c") return "high_performance";
    if (g == "a1841308-3541-4fab-bc81-f71556f20b4a") return "power_saver";
    if (g == "e9a42b02-d5df-448d-aa00-03f14749eb61") return "ultimate";
    return "custom";
}

// Windows 10 1709+ 전원 모드(오버레이). powrprof에 있지만 오래된 SDK/OS를 위해 동적으로 찾는다.
std::string overlay_mode() {
    using Fn = DWORD(WINAPI*)(GUID*);
    HMODULE m = GetModuleHandleW(L"powrprof.dll");
    auto fn = m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, "PowerGetEffectiveOverlayScheme"))) : nullptr;
    if (!fn) return "unknown";
    GUID g{};
    if (fn(&g) != ERROR_SUCCESS) return "unknown";
    const std::string s = guid_str(g);
    if (s == "961cc777-2547-4f9d-8174-7d86181b8a7a") return "best_efficiency";
    if (s == "ded574b5-45a0-4f42-8737-46345c09c238") return "best_performance";
    if (s == "00000000-0000-0000-0000-000000000000" || s == "3af9b8d9-7c97-431d-ad78-34a8bfea439f") return "balanced";
    return "unknown";
}

// GUID_PROCESSOR_SETTINGS_SUBGROUP / THROTTLE_MAXIMUM / THROTTLE_MINIMUM
const GUID kProcSub = {0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
const GUID kProcMax = {0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec}};
const GUID kProcMin = {0x893dee8e, 0x2bef, 0x41e0, {0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c}};

int read_proc_setting(const GUID* scheme, const GUID& setting, bool ac) {
    DWORD v = 0;
    DWORD rc = ac ? PowerReadACValueIndex(nullptr, scheme, &kProcSub, &setting, &v)
                  : PowerReadDCValueIndex(nullptr, scheme, &kProcSub, &setting, &v);
    return rc == ERROR_SUCCESS ? int(v) : -1;
}

struct ProcTimes {
    std::string name;
    uint64_t cpu = 0;  // kernel + user, 100ns
};

// 모든 프로세스의 CPU 시간. 작업 관리자처럼 NtQuerySystemInformation(SystemProcessInformation)을 쓴다.
// OpenProcess로 하나씩 열면 dwm, System, 백신(MsMpEng) 같은 보호된 프로세스는 열리지 않아 빠진다(버그 002).
std::map<uint32_t, ProcTimes> snapshot_processes() {
    std::map<uint32_t, ProcTimes> out;
    using NtQSI = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<NtQSI>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    if (!query) return out;
    constexpr ULONG kSystemProcessInformation = 5;
    std::vector<uint8_t> buf(1 << 20);
    for (int tries = 0; tries < 8; ++tries) {
        ULONG need = 0;
        LONG st = query(kSystemProcessInformation, buf.data(), ULONG(buf.size()), &need);
        if (st == 0) break;
        if (st != LONG(0xC0000004)) return out;  // STATUS_INFO_LENGTH_MISMATCH 말고는 실패
        buf.resize(std::max<size_t>(need + (64 << 10), buf.size() * 2));
        if (tries == 7) return out;
    }
    // SYSTEM_PROCESS_INFORMATION (winternl.h에 일부만 공개): 아래 오프셋은 x64 기준
    //   0 NextEntryOffset, 40 UserTime, 48 KernelTime, 56 ImageName(UNICODE_STRING), 80 UniqueProcessId
    static_assert(sizeof(void*) == 8, "offsets below are for x64");
    for (size_t off = 0;;) {
        const uint8_t* e = buf.data() + off;
        ULONG next;
        int64_t user, kernel;
        USHORT name_len;
        const wchar_t* name;
        ULONG_PTR pid;
        std::memcpy(&next, e, 4);
        std::memcpy(&user, e + 40, 8);
        std::memcpy(&kernel, e + 48, 8);
        std::memcpy(&name_len, e + 56, 2);
        std::memcpy(&name, e + 64, sizeof(name));
        std::memcpy(&pid, e + 80, sizeof(pid));
        if (pid != 0) {  // 0 = System Idle Process
            std::wstring w = name ? std::wstring(name, name_len / sizeof(wchar_t)) : L"System";
            out[uint32_t(pid)] = {narrow(w.c_str()), uint64_t(user) + uint64_t(kernel)};
        }
        if (!next) break;
        off += next;
        if (off >= buf.size()) break;
    }
    return out;
}

}  // namespace

EnvReport check_environment(double sample_seconds, size_t top_n) {
    EnvReport r;

    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) {
        r.power_known = true;
        r.on_ac = ps.ACLineStatus != 0;  // 255(알 수 없음)도 AC로 본다 (데스크톱)
        r.has_battery = !(ps.BatteryFlag & 128) && ps.BatteryFlag != 255;
        if (r.has_battery && ps.BatteryLifePercent <= 100) r.battery_percent = ps.BatteryLifePercent;
    }

    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) == ERROR_SUCCESS && active) {
        r.scheme_guid = guid_str(*active);
        r.scheme_kind = scheme_kind(r.scheme_guid);
        DWORD n = 0;
        if (PowerReadFriendlyName(nullptr, active, nullptr, nullptr, nullptr, &n) == ERROR_SUCCESS && n > 0) {
            std::vector<UCHAR> buf(n);
            if (PowerReadFriendlyName(nullptr, active, nullptr, nullptr, buf.data(), &n) == ERROR_SUCCESS)
                r.scheme_name = narrow(reinterpret_cast<const wchar_t*>(buf.data()));
        }
        r.max_processor_state = read_proc_setting(active, kProcMax, r.on_ac);
        r.min_processor_state = read_proc_setting(active, kProcMin, r.on_ac);
        LocalFree(active);
    }
    r.overlay = overlay_mode();

    // 백그라운드 사용률: 측정 구간 동안 이 프로세스는 잠들어 있다
    FILETIME i0, k0, u0, i1, k1, u1;
    auto before = snapshot_processes();
    GetSystemTimes(&i0, &k0, &u0);
    Sleep(DWORD(sample_seconds * 1000));
    GetSystemTimes(&i1, &k1, &u1);
    auto after = snapshot_processes();
    r.sample_seconds = sample_seconds;

    const uint64_t idle = ft64(i1) - ft64(i0);
    const uint64_t total = (ft64(k1) - ft64(k0)) + (ft64(u1) - ft64(u0));  // kernel 시간에 idle이 포함됨
    if (total > 0) r.busy_percent = 100.0 * double(total - std::min(idle, total)) / double(total);

    const uint32_t self = GetCurrentProcessId();
    for (const auto& [pid, a] : after) {
        auto it = before.find(pid);
        if (it == before.end() || pid == self || a.cpu < it->second.cpu || total == 0) continue;
        const double pct = 100.0 * double(a.cpu - it->second.cpu) / double(total);
        r.process_percent += pct;
        if (pct >= 0.05) r.top.push_back({a.name, pid, pct});
    }
    std::sort(r.top.begin(), r.top.end(), [](const ProcessLoad& x, const ProcessLoad& y) { return x.cpu_percent > y.cpu_percent; });
    if (r.top.size() > top_n) r.top.resize(top_n);
    return r;
}

std::vector<EnvWarning> evaluate(const EnvReport& r) {
    std::vector<EnvWarning> w;
    if (r.power_known && !r.on_ac)
        w.push_back({"on_battery", "Running on battery. Plug in the charger: laptops lower CPU clocks on battery."});
    if (r.scheme_kind == "power_saver")
        w.push_back({"power_saver", "Power plan is Power saver. Switch to Balanced or High performance."});
    if (r.overlay == "best_efficiency")
        w.push_back({"efficiency_mode", "Windows power mode is 'Best power efficiency'. Set it to Balanced or Best performance."});
    if (r.max_processor_state >= 0 && r.max_processor_state < 100) {
        char b[160];
        std::snprintf(b, sizeof(b), "Maximum processor state is %d%% (below 100%% usually disables turbo boost).",
                      r.max_processor_state);
        w.push_back({"max_state", b});
    }
    if (r.busy_percent > kBackgroundWarnPercent) {
        char b[200];
        std::snprintf(b, sizeof(b), "Background CPU use is %.1f%% while idle. Multi-thread scores can drop by about that much.",
                      r.busy_percent);
        w.push_back({"background", b});
    }
    return w;
}

std::vector<EnvWarning> evaluate_memory(const MemoryInfo& m) {
    std::vector<EnvWarning> w;
    const size_t n = m.populated();
    if (n == 0) return w;
    char b[256];
    if (n == 1) {
        w.push_back({"single_channel", "Only one memory module is installed, so memory runs in single channel "
                                       "(about half the bandwidth of dual channel)."});
    } else if (m.channels() == 1) {
        std::string ch;
        for (const auto& x : m.modules)
            if (x.size_bytes) ch = x.channel;
        std::snprintf(b, sizeof(b), "All %zu modules are in channel %s. Move them to the slots the motherboard manual "
                                    "recommends (usually A2 and B2) for dual channel.", n, ch.c_str());
        w.push_back({"same_channel", b});
    }
    for (const auto& x : m.modules) {
        if (!x.size_bytes || !x.configured_speed) continue;
        const RatedSpeed r = rated_speed_from_part(x.part_number);
        if (r.mts > x.configured_speed) {
            std::snprintf(b, sizeof(b), "%s (%s) is rated %u MT/s but runs at %u MT/s. XMP/EXPO may be off in the BIOS.",
                          x.part_number.c_str(), r.brand, r.mts, x.configured_speed);
            w.push_back({"xmp_off", b});
            break;  // 같은 키트면 한 번만
        }
        if (x.max_speed && x.configured_speed < x.max_speed) {
            std::snprintf(b, sizeof(b), "%s runs at %u MT/s, below its own %u MT/s (mixed modules or a platform limit).",
                          x.device_locator.c_str(), x.configured_speed, x.max_speed);
            w.push_back({"below_jedec", b});
            break;
        }
    }
    return w;
}

ProcessSnapshot take_process_snapshot() {
    ProcessSnapshot s;
    FILETIME i, k, u;
    if (GetSystemTimes(&i, &k, &u)) s.total = ft64(k) + ft64(u);
    for (auto& [pid, p] : snapshot_processes()) s.procs[pid] = {std::move(p.name), p.cpu};
    return s;
}

std::vector<ProcessLoad> top_between(const ProcessSnapshot& a, const ProcessSnapshot& b, size_t top_n) {
    std::vector<ProcessLoad> top;
    const uint64_t total = b.total - a.total;
    if (total == 0) return top;
    const uint32_t self = GetCurrentProcessId();
    for (const auto& [pid, p] : b.procs) {
        auto it = a.procs.find(pid);
        if (it == a.procs.end() || pid == self || p.second < it->second.second) continue;
        const double pct = 100.0 * double(p.second - it->second.second) / double(total);
        if (pct >= 0.05) top.push_back({p.first, pid, pct});
    }
    std::sort(top.begin(), top.end(), [](const ProcessLoad& x, const ProcessLoad& y) { return x.cpu_percent > y.cpu_percent; });
    if (top.size() > top_n) top.resize(top_n);
    return top;
}
