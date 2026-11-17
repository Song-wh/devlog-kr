// cpu-bench-runner: 측정 전용 프로세스 (CLI)
//
//   cpu-bench-runner info [--json]
//   cpu-bench-runner check [--seconds N] [--json]   측정 전 환경 점검
//   cpu-bench-runner run <module id | dll 경로> [--workload W] [--size N] [--threads N | --mt]
//                        [--duration-ms N] [--runs N] [--seed N] [--no-pin] [--json]
//   cpu-bench-runner sweep <module> [--workload W] [--from N] [--to N] [측정 옵션...]
//
// 측정 절차: 프로세스 우선순위 HIGH → 워밍업 1회 → N회 측정 → 중앙값 채택, 변동계수(CV) 계산
// sweep은 작업 데이터 크기를 from부터 2배씩 to까지 늘려 가며 크기마다 같은 절차를 반복한다.
#include "bench_api.h"
#include "envcheck.h"
#include "smbios.h"
#include "topology.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr double kCvWarnPercent = 2.0;  // 재현성 목표: 반복 측정 편차 2% 이내

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += char(c);
                }
        }
    }
    return o + "\"";
}

const char* status_name(int32_t rc) {
    switch (rc) {
        case BENCH_OK: return "ok";
        case BENCH_E_INVALID_ARG: return "invalid_arg";
        case BENCH_E_OUT_OF_MEMORY: return "out_of_memory";
        case BENCH_E_UNSUPPORTED: return "unsupported";
        case BENCH_E_CANCELLED: return "cancelled";
        case BENCH_E_CHECKSUM: return "checksum_mismatch";
        default: return "internal_error";
    }
}

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  cpu-bench-runner info [--json]\n"
                 "  cpu-bench-runner check [--seconds N] [--json]\n"
                 "  cpu-bench-runner run <module id | dll path> [--workload W] [--size N] [--threads N | --mt]\n"
                 "                       [--duration-ms N] [--runs N] [--seed N] [--no-pin] [--json]\n"
                 "  cpu-bench-runner sweep <module id | dll path> [--workload W] [--from N] [--to N]\n"
                 "                       [--threads N | --mt] [--duration-ms N] [--runs N] [--seed N] [--no-pin] [--json]\n"
                 "sizes accept K/M/G suffixes (binary): 16K, 256M, 1G\n");
    return 2;
}

// ---------------------------------------------------------------- check

int cmd_check(int argc, wchar_t** argv) {
    double seconds = 3.0;
    bool json = false;
    for (int i = 2; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--json") json = true;
        else if (a == L"--seconds" && i + 1 < argc) {
            wchar_t* end = nullptr;
            seconds = std::wcstod(argv[++i], &end);
            if (!end || *end || seconds < 0.5 || seconds > 60) return usage();
        } else return usage();
    }
    if (!json) std::printf("Checking the environment (sampling idle CPU use for %.1f s)...\n", seconds);
    const EnvReport r = check_environment(seconds, 5);
    const auto warnings = evaluate(r);
    if (json) {
        std::printf("{\"power\":{\"known\":%s,\"on_ac\":%s,\"has_battery\":%s,\"battery_percent\":%d},"
                    "\"power_plan\":{\"guid\":%s,\"name\":%s,\"kind\":%s,\"overlay\":%s,\"max_processor_state\":%d,"
                    "\"min_processor_state\":%d},\"background\":{\"sample_seconds\":%.1f,\"busy_percent\":%.2f,\"process_percent\":%.2f,\"top\":[",
                    r.power_known ? "true" : "false", r.on_ac ? "true" : "false", r.has_battery ? "true" : "false",
                    r.battery_percent, json_str(r.scheme_guid).c_str(), json_str(r.scheme_name).c_str(),
                    json_str(r.scheme_kind).c_str(), json_str(r.overlay).c_str(), r.max_processor_state,
                    r.min_processor_state, r.sample_seconds, r.busy_percent, r.process_percent);
        for (size_t i = 0; i < r.top.size(); ++i)
            std::printf("%s{\"name\":%s,\"pid\":%u,\"cpu_percent\":%.2f}", i ? "," : "", json_str(r.top[i].name).c_str(),
                        r.top[i].pid, r.top[i].cpu_percent);
        std::printf("]},\"warnings\":[");
        for (size_t i = 0; i < warnings.size(); ++i)
            std::printf("%s{\"id\":%s,\"message\":%s}", i ? "," : "", json_str(warnings[i].id).c_str(),
                        json_str(warnings[i].message).c_str());
        std::printf("]}\n");
        return 0;
    }
    if (r.power_known)
        std::printf("Power    : %s%s\n", r.on_ac ? "AC" : "battery",
                    r.has_battery ? (r.battery_percent >= 0 ? (", battery " + std::to_string(r.battery_percent) + "%").c_str() : ", battery") : "");
    std::printf("Plan     : %s (%s)\n", r.scheme_kind.c_str(), r.scheme_guid.c_str());
    std::printf("Mode     : %s\n", r.overlay.c_str());
    if (r.max_processor_state >= 0)
        std::printf("CPU state: min %d%%, max %d%% (%s)\n", r.min_processor_state, r.max_processor_state, r.on_ac ? "AC" : "battery");
    std::printf("Idle CPU : %.1f%% busy over %.1f s (processes %.1f%%, rest is interrupts/DPC)\n", r.busy_percent, r.sample_seconds, r.process_percent);
    for (const auto& p : r.top) std::printf("  %5.1f%%  %s (pid %u)\n", p.cpu_percent, p.name.c_str(), p.pid);
    if (warnings.empty()) std::printf("Result   : ready to measure\n");
    for (const auto& w : warnings) std::printf("Warning  : %s\n", w.message.c_str());
    return 0;
}

// ---------------------------------------------------------------- info

int cmd_info(bool json) {
    Topology topo;
    std::string err;
    if (!query_topology(topo, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    MemoryInfo mem;
    std::string mem_err;
    const bool mem_ok = query_memory(mem, mem_err);
    if (json) {
        std::printf("{\"cpu\":%s,\"logical\":%zu,\"cores\":%u,\"groups\":%u,\"hybrid\":%s,\"perf_cores\":%u,\"caches\":[",
                    json_str(topo.brand).c_str(), topo.cpus.size(), topo.cores, topo.groups,
                    topo.hybrid ? "true" : "false", topo.perf_cores);
        for (size_t i = 0; i < topo.caches.size(); ++i) {
            const auto& c = topo.caches[i];
            std::printf("%s{\"level\":%u,\"type\":\"%s\",\"size\":%u,\"count\":%u}", i ? "," : "", c.level, c.type,
                        c.size_bytes, c.count);
        }
        std::printf("]");
        if (mem_ok) {
            std::printf(",\"memory\":{\"smbios\":\"%u.%u\",\"total_bytes\":%llu,\"populated\":%zu,\"channels\":%u,\"modules\":[",
                        mem.smbios_major, mem.smbios_minor, static_cast<unsigned long long>(mem.total_bytes()),
                        mem.populated(), mem.channels());
            bool first = true;
            for (const auto& m : mem.modules) {
                std::printf("%s{\"slot\":%s,\"bank\":%s,\"channel\":%s,\"size_bytes\":%llu,\"type\":\"%s\",\"form\":\"%s\","
                            "\"rank\":%u,\"max_speed\":%u,\"configured_speed\":%u,\"manufacturer\":%s,\"part_number\":%s}",
                            first ? "" : ",", json_str(m.device_locator).c_str(), json_str(m.bank_locator).c_str(),
                            json_str(m.channel).c_str(), static_cast<unsigned long long>(m.size_bytes),
                            memory_type_name(m.memory_type), form_factor_name(m.form_factor), m.rank, m.max_speed,
                            m.configured_speed, json_str(m.manufacturer).c_str(), json_str(m.part_number).c_str());
                first = false;
            }
            std::printf("]}");
        }
        std::printf("}\n");
        return 0;
    }
    std::printf("CPU      : %s\n", topo.brand.c_str());
    std::printf("Logical  : %zu  (cores %u, groups %u)\n", topo.cpus.size(), topo.cores, topo.groups);
    if (topo.hybrid)
        std::printf("Hybrid   : yes, %u performance cores / %u total\n", topo.perf_cores, topo.cores);
    else
        std::printf("Hybrid   : no\n");
    for (const auto& c : topo.caches)
        std::printf("L%u %-11s: %6u KB x %u\n", c.level, c.type, c.size_bytes / 1024, c.count);
    if (!mem_ok) {
        std::printf("Memory   : unavailable (%s)\n", mem_err.c_str());
        return 0;
    }
    const unsigned ch = mem.channels();
    std::printf("Memory   : %.0f GiB, %zu of %zu slots used, %s\n", double(mem.total_bytes()) / double(1ull << 30),
                mem.populated(), mem.modules.size(),
                ch == 1 ? "single channel" : ch == 2 ? "dual channel" : ch ? (std::to_string(ch) + " channels").c_str()
                                                                           : "channel layout unknown");
    for (const auto& m : mem.modules) {
        if (!m.size_bytes) {
            std::printf("  %-22s: empty\n", m.device_locator.c_str());
            continue;
        }
        std::printf("  %-22s: %.0f GiB %s %s, max %u MT/s, configured %u MT/s%s%s%s%s\n", m.device_locator.c_str(),
                    double(m.size_bytes) / double(1ull << 30), memory_type_name(m.memory_type),
                    form_factor_name(m.form_factor), m.max_speed, m.configured_speed,
                    m.channel.empty() ? "" : ", channel ", m.channel.c_str(),
                    m.part_number.empty() ? "" : ", ", m.part_number.c_str());
    }
    return 0;
}

// ---------------------------------------------------------------- run / sweep

struct Module {
    HMODULE dll = nullptr;
    PFN_bench_get_info get_info = nullptr;
    PFN_bench_init init = nullptr;
    PFN_bench_run run = nullptr;
    PFN_bench_shutdown shutdown = nullptr;
    const BenchInfo* info = nullptr;
    std::vector<std::string> workloads;  // v2 모듈이 알려 준 측정 종류. 첫 번째가 기본값
};

std::wstring resolve_module(const std::wstring& arg) {
    // 확장자가 없으면 모듈 id로 보고 실행 파일 옆의 bench_<id>.dll을 찾는다
    if (arg.find(L'.') == std::wstring::npos && arg.find(L'\\') == std::wstring::npos &&
        arg.find(L'/') == std::wstring::npos) {
        wchar_t exe[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe, n);
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
        return dir + L"bench_" + arg + L".dll";
    }
    wchar_t full[MAX_PATH];
    DWORD n = GetFullPathNameW(arg.c_str(), MAX_PATH, full, nullptr);
    return (n > 0 && n < MAX_PATH) ? std::wstring(full, n) : arg;
}

// FARPROC -> 실제 함수 타입. void*를 거쳐 clang-cl의 함수 타입 불일치 경고를 피한다.
template <class T>
T get_proc(HMODULE dll, const char* name) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
}

bool load_module(const std::wstring& path, Module& m, std::string& err) {
    m.dll = LoadLibraryExW(path.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m.dll) {
        err = "cannot load " + narrow(path) + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    m.get_info = get_proc<PFN_bench_get_info>(m.dll, "bench_get_info");
    m.init = get_proc<PFN_bench_init>(m.dll, "bench_init");
    m.run = get_proc<PFN_bench_run>(m.dll, "bench_run");
    m.shutdown = get_proc<PFN_bench_shutdown>(m.dll, "bench_shutdown");
    if (!m.get_info || !m.init || !m.run || !m.shutdown) {
        err = "module does not export the bench_* functions";
        return false;
    }
    // 구조체는 뒤에만 필드를 추가하므로 이전 버전 모듈도 읽을 수 있다
    m.info = m.get_info();
    if (!m.info || m.info->api_version < 1 || m.info->api_version > BENCH_API_VERSION) {
        err = "module API version mismatch";
        return false;
    }
    if (m.info->struct_size >= offsetof(BenchInfo, workloads) + sizeof(m.info->workloads) && m.info->workloads) {
        std::string cur;
        for (const char* p = m.info->workloads;; ++p) {
            if (*p && *p != ',') {
                cur += *p;
                continue;
            }
            if (!cur.empty()) m.workloads.push_back(cur);
            cur.clear();
            if (!*p) break;
        }
    }
    return true;
}

struct PinPlan {
    std::vector<LogicalCpu> order;
    bool enabled = true;
    std::atomic<int> failures{0};
};

void BENCH_CALL on_thread_init(uint32_t worker, void* user) {
    auto* plan = static_cast<PinPlan*>(user);
    if (!plan->enabled || plan->order.empty()) return;
    const LogicalCpu& cpu = plan->order[worker % plan->order.size()];
    if (!pin_current_thread(cpu)) plan->failures.fetch_add(1);
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// "16K", "256M", "1G", "4096" (K/M/G는 1024 단위)
bool parse_size(const std::wstring& s, uint64_t& out) {
    wchar_t* end = nullptr;
    uint64_t v = std::wcstoull(s.c_str(), &end, 0);
    if (!end || end == s.c_str()) return false;
    int shift = 0;
    if (*end == L'K' || *end == L'k') shift = 10;
    else if (*end == L'M' || *end == L'm') shift = 20;
    else if (*end == L'G' || *end == L'g') shift = 30;
    if (shift) ++end;
    if (*end || v == 0 || v > (~0ull >> shift)) return false;
    out = v << shift;
    return true;
}

std::string format_size(uint64_t b) {
    char buf[32];
    const unsigned long long u = b;
    if (b >= (1ull << 30) && b % (1ull << 30) == 0) std::snprintf(buf, sizeof(buf), "%llu GiB", u >> 30);
    else if (b >= (1ull << 20) && b % (1ull << 20) == 0) std::snprintf(buf, sizeof(buf), "%llu MiB", u >> 20);
    else if (b >= (1ull << 10) && b % (1ull << 10) == 0) std::snprintf(buf, sizeof(buf), "%llu KiB", u >> 10);
    else if (b >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.1f MiB", double(b) / double(1ull << 20));
    else if (b >= (1ull << 10)) std::snprintf(buf, sizeof(buf), "%.1f KiB", double(b) / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%llu B", u);
    return buf;
}

// 결과 단위에 맞춘 표시: byte -> GB/s, load -> 로드 1회 지연(ns), 그 외 -> <unit>/s
struct Display {
    std::string raw_unit;  // 모듈이 준 op 단위
    std::string unit;      // 표시 단위
};

Display display_of(const BenchResult& r, const BenchInfo* info) {
    Display d;
    d.raw_unit = r.unit_name[0] ? r.unit_name : info->unit_name;
    if (d.raw_unit == "byte") d.unit = "GB/s";
    else if (d.raw_unit == "load") d.unit = "ns";
    else d.unit = d.raw_unit + "/s";
    return d;
}

double display_value(const BenchResult& r, const Display& d) {
    if (d.raw_unit == "byte") return r.ops_per_sec / 1e9;
    if (d.raw_unit == "load") return r.ops_per_sec > 0 ? double(r.threads_used) * 1e9 / r.ops_per_sec : 0.0;
    return r.ops_per_sec;
}

struct Options {
    std::wstring module;
    std::string workload;  // 비어 있으면 모듈 기본값
    uint64_t size = 0;     // 0이면 모듈 기본값
    uint64_t from = 16ull << 10, to = 512ull << 20;
    uint32_t threads = 1, duration_ms = 2000, runs = 5;
    uint64_t seed = 1;
    bool mt_all = false, pin = true, json = false;
};

bool parse_options(int argc, wchar_t** argv, bool sweep, Options& o) {
    if (argc < 3) return false;
    o.module = argv[2];
    if (sweep) {
        o.duration_ms = 500;
        o.runs = 3;
    }
    for (int i = 3; i < argc; ++i) {
        std::wstring a = argv[i];
        const bool has_value = i + 1 < argc;
        auto next_u64 = [&](uint64_t& v) {
            if (!has_value) return false;
            wchar_t* end = nullptr;
            v = std::wcstoull(argv[++i], &end, 0);
            return end && *end == 0;
        };
        auto next_size = [&](uint64_t& v) { return has_value && parse_size(argv[++i], v); };
        uint64_t v = 0;
        if (a == L"--threads" && next_u64(v) && v > 0 && v <= 4096) o.threads = uint32_t(v);
        else if (a == L"--mt") o.mt_all = true;
        else if (a == L"--duration-ms" && next_u64(v) && v > 0 && v <= 3600000) o.duration_ms = uint32_t(v);
        else if (a == L"--runs" && next_u64(v) && v > 0 && v <= 1000) o.runs = uint32_t(v);
        else if (a == L"--seed" && next_u64(v)) o.seed = v;
        else if (a == L"--no-pin") o.pin = false;
        else if (a == L"--json") o.json = true;
        else if (a == L"--workload" && has_value) o.workload = narrow(argv[++i]);
        else if (!sweep && a == L"--size" && next_size(v)) o.size = v;
        else if (sweep && a == L"--from" && next_size(v)) o.from = v;
        else if (sweep && a == L"--to" && next_size(v)) o.to = v;
        else return false;
    }
    return !sweep || o.from <= o.to;
}

// 측정 한 세트: bench_init → 워밍업 1회 → N회 → bench_shutdown
struct Measurement {
    std::vector<BenchResult> results;
    std::vector<double> values;  // 표시 단위 값
    Display display;
    double median = 0, cv = 0;
    uint64_t mismatches = 0;
    bool ok = false;  // 회차 간 checksum 같음 + 불일치 0
};

template <class OnRun>
int32_t measure(const Module& mod, const BenchConfig& cfg, uint32_t runs, Measurement& m, OnRun&& on_run) {
    BenchContext* ctx = nullptr;
    int32_t rc = mod.init(&cfg, &ctx);
    if (rc != BENCH_OK) return rc;
    int32_t worst = BENCH_OK;
    for (uint32_t r = 0; r <= runs; ++r) {  // r == 0은 워밍업
        BenchResult res{};
        res.struct_size = sizeof(res);
        rc = mod.run(ctx, &res, nullptr, nullptr, nullptr);
        if (rc != BENCH_OK && rc != BENCH_E_CHECKSUM) {
            mod.shutdown(ctx);
            return rc;
        }
        if (rc == BENCH_E_CHECKSUM) worst = rc;
        if (r == 0) continue;
        if (m.results.empty()) m.display = display_of(res, mod.info);
        m.results.push_back(res);
        m.values.push_back(display_value(res, m.display));
        on_run(r, res, m.values.back());
    }
    mod.shutdown(ctx);

    bool stable = true;
    for (const auto& r : m.results) {
        m.mismatches += r.mismatches;
        if (r.checksum != m.results.front().checksum) stable = false;
    }
    double mean = 0;
    for (double v : m.values) mean += v;
    mean /= double(m.values.size());
    double var = 0;
    for (double v : m.values) var += (v - mean) * (v - mean);
    double stdev = m.values.size() > 1 ? std::sqrt(var / double(m.values.size() - 1)) : 0.0;
    m.cv = mean > 0 ? 100.0 * stdev / mean : 0.0;
    m.median = median(m.values);
    m.ok = m.mismatches == 0 && stable && worst == BENCH_OK;
    return BENCH_OK;
}

// run / sweep 공통 준비: 토폴로지, 모듈 로드, 우선순위, 코어 고정 계획, 기본 설정
struct Session {
    Topology topo;
    Module mod;
    PinPlan plan;
    bool high_priority = false;
    std::string workload;  // 표시용 (지정하지 않으면 모듈의 첫 번째 종류)
    BenchConfig cfg{};
};

bool open_session(const Options& o, Session& s) {
    std::string err;
    if (!query_topology(s.topo, err) || !load_module(resolve_module(o.module), s.mod, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return false;
    }
    if (s.mod.workloads.empty() && (!o.workload.empty() || o.size)) {
        std::fprintf(stderr, "error: module '%s' does not support --workload/--size\n", s.mod.info->id);
        return false;
    }
    if (!o.workload.empty() &&
        std::find(s.mod.workloads.begin(), s.mod.workloads.end(), o.workload) == s.mod.workloads.end()) {
        std::string list;
        for (const auto& w : s.mod.workloads) list += (list.empty() ? "" : ", ") + w;
        std::fprintf(stderr, "error: unknown workload '%s' (available: %s)\n", o.workload.c_str(), list.c_str());
        return false;
    }
    s.workload = !o.workload.empty() ? o.workload : (s.mod.workloads.empty() ? "" : s.mod.workloads.front());

    // REALTIME은 쓰지 않는다 (시스템이 멈출 수 있음). HIGH만 사용.
    s.high_priority = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != 0;
    s.plan.enabled = o.pin;
    s.plan.order = placement_order(s.topo);

    s.cfg.struct_size = sizeof(s.cfg);
    s.cfg.threads = o.mt_all ? uint32_t(s.topo.cpus.size()) : o.threads;
    s.cfg.duration_ms = o.duration_ms;
    s.cfg.flags = BENCH_FLAG_NONE;
    s.cfg.seed = o.seed;
    s.cfg.thread_init = &on_thread_init;
    s.cfg.user = &s.plan;
    s.cfg.workload = o.workload.empty() ? nullptr : o.workload.c_str();
    s.cfg.working_set_bytes = o.size;
    return true;
}

void report_error(int32_t rc, const Session& s) {
    if (rc == BENCH_E_INVALID_ARG && s.cfg.working_set_bytes)
        std::fprintf(stderr, "error: working set %s is too small for %u threads (min 4 KiB per thread)\n",
                     format_size(s.cfg.working_set_bytes).c_str(), s.cfg.threads);
    else
        std::fprintf(stderr, "error: benchmark failed (%s)\n", status_name(rc));
}

const char* pin_text(const Options& o, const Session& s) {
    if (!o.pin) return "";
    return s.cfg.threads == 1 && s.topo.hybrid ? ", pinned to a performance core" : ", pinned";
}

void print_session_warnings(const Session& s) {
    if (!s.high_priority) std::printf("Warning  : could not raise process priority\n");
    if (s.plan.failures) std::printf("Warning  : %d thread(s) could not be pinned\n", s.plan.failures.load());
}

int cmd_run(int argc, wchar_t** argv) {
    Options o;
    if (!parse_options(argc, argv, false, o)) return usage();
    Session s;
    if (!open_session(o, s)) return 1;
    const BenchInfo* info = s.mod.info;

    if (!o.json) {
        std::printf("Module   : %s %s\n", info->name, info->version);
        std::printf("CPU      : %s\n", s.topo.brand.c_str());
        std::printf("Threads  : %u%s, %u ms x %u runs (+1 warm-up), seed %llu\n", s.cfg.threads, pin_text(o, s),
                    o.duration_ms, o.runs, static_cast<unsigned long long>(o.seed));
    }

    Measurement m;
    int32_t rc = measure(s.mod, s.cfg, o.runs, m, [&](uint32_t r, const BenchResult& res, double v) {
        if (o.json) return;
        if (r == 1 && !s.workload.empty())
            std::printf("Workload : %s, %s\n", s.workload.c_str(), format_size(res.working_set_bytes).c_str());
        std::printf("  run %u  : %12.2f %-6s (%llu %ss, %.3f s)%s\n", r, v, m.display.unit.c_str(),
                    static_cast<unsigned long long>(res.iterations), m.display.raw_unit.c_str(), res.elapsed_s,
                    res.mismatches ? "  CHECKSUM MISMATCH" : "");
    });
    if (rc != BENCH_OK) {
        report_error(rc, s);
        return 1;
    }

    const BenchResult& first = m.results.front();
    if (o.json) {
        std::printf("{\"module\":%s,\"module_version\":%s,\"workload\":%s,\"working_set_bytes\":%llu,"
                    "\"variant\":%s,\"cpu\":%s,\"threads\":%u,\"pinned\":%s,\"pin_failures\":%d,"
                    "\"high_priority\":%s,\"duration_ms\":%u,\"seed\":%llu,\"unit\":%s,\"value_unit\":%s,\"runs\":[",
                    json_str(info->id).c_str(), json_str(info->version).c_str(), json_str(s.workload).c_str(),
                    static_cast<unsigned long long>(first.working_set_bytes), json_str(first.variant).c_str(),
                    json_str(s.topo.brand).c_str(), s.cfg.threads, o.pin ? "true" : "false",
                    s.plan.failures.load(), s.high_priority ? "true" : "false", o.duration_ms,
                    static_cast<unsigned long long>(o.seed), json_str(m.display.raw_unit).c_str(),
                    json_str(m.display.unit).c_str());
        std::vector<double> ops;
        for (size_t i = 0; i < m.results.size(); ++i) {
            const BenchResult& r = m.results[i];
            ops.push_back(r.ops_per_sec);
            std::printf("%s{\"ops_per_sec\":%.3f,\"value\":%.4f,\"iterations\":%llu,\"elapsed_s\":%.6f,"
                        "\"mismatches\":%llu}",
                        i ? "," : "", r.ops_per_sec, m.values[i], static_cast<unsigned long long>(r.iterations),
                        r.elapsed_s, static_cast<unsigned long long>(r.mismatches));
        }
        std::printf("],\"median_ops_per_sec\":%.3f,\"median_value\":%.4f,\"cv_percent\":%.3f,"
                    "\"checksum\":\"0x%016llx\",\"checksum_ok\":%s}\n",
                    median(ops), m.median, m.cv, static_cast<unsigned long long>(first.checksum),
                    m.ok ? "true" : "false");
    } else {
        std::printf("Median   : %.2f %s  (CV %.2f%%)\n", m.median, m.display.unit.c_str(), m.cv);
        std::printf("Variant  : %s\n", first.variant);
        std::printf("Checksum : 0x%016llx %s\n", static_cast<unsigned long long>(first.checksum),
                    m.ok ? "(stable)" : "(MISMATCH - results are not trustworthy)");
        if (m.cv > kCvWarnPercent)
            std::printf("Warning  : run-to-run variation is above %.0f%%. Close background apps and retry.\n",
                        kCvWarnPercent);
        print_session_warnings(s);
    }
    return m.ok ? 0 : 3;
}

int cmd_sweep(int argc, wchar_t** argv) {
    Options o;
    if (!parse_options(argc, argv, true, o)) return usage();
    Session s;
    if (!open_session(o, s)) return 1;
    const BenchInfo* info = s.mod.info;
    if (s.mod.workloads.empty()) {
        std::fprintf(stderr, "error: module '%s' has a fixed working set; use 'run'\n", info->id);
        return 1;
    }

    if (o.json) {
        std::printf("{\"module\":%s,\"module_version\":%s,\"workload\":%s,\"cpu\":%s,\"threads\":%u,\"pinned\":%s,"
                    "\"duration_ms\":%u,\"runs\":%u,\"seed\":%llu,\"points\":[",
                    json_str(info->id).c_str(), json_str(info->version).c_str(), json_str(s.workload).c_str(),
                    json_str(s.topo.brand).c_str(), s.cfg.threads, o.pin ? "true" : "false", o.duration_ms, o.runs,
                    static_cast<unsigned long long>(o.seed));
    } else {
        std::printf("Module   : %s %s\n", info->name, info->version);
        std::printf("CPU      : %s\n", s.topo.brand.c_str());
        std::printf("Workload : %s\n", s.workload.c_str());
        std::printf("Threads  : %u%s, %u ms x %u runs per size (+1 warm-up), seed %llu\n", s.cfg.threads,
                    pin_text(o, s), o.duration_ms, o.runs, static_cast<unsigned long long>(o.seed));
        std::string caches;
        for (const auto& c : s.topo.caches) {
            if (std::string(c.type) == "instruction") continue;
            char b[64];
            std::snprintf(b, sizeof(b), "%sL%u %s x %u", caches.empty() ? "" : ", ", c.level,
                          format_size(c.size_bytes).c_str(), c.count);
            caches += b;
        }
        std::printf("Caches   : %s\n", caches.c_str());
        std::printf("\n%12s %14s %9s  %s\n", "Size", "Median", "CV", "Checksum");
    }

    bool all_ok = true;
    for (uint64_t size = o.from;; size *= 2) {
        s.cfg.working_set_bytes = size;
        Measurement m;
        int32_t rc = measure(s.mod, s.cfg, o.runs, m, [](uint32_t, const BenchResult&, double) {});
        if (rc != BENCH_OK) {
            if (o.json) std::printf("]}\n");
            report_error(rc, s);
            return 1;
        }
        all_ok = all_ok && m.ok;
        const BenchResult& first = m.results.front();
        if (o.json) {
            std::printf("%s{\"working_set_bytes\":%llu,\"median_value\":%.4f,\"value_unit\":%s,\"cv_percent\":%.3f,"
                        "\"checksum\":\"0x%016llx\",\"checksum_ok\":%s}",
                        size == o.from ? "" : ",", static_cast<unsigned long long>(first.working_set_bytes),
                        m.median, json_str(m.display.unit).c_str(), m.cv,
                        static_cast<unsigned long long>(first.checksum), m.ok ? "true" : "false");
        } else {
            char val[32];
            std::snprintf(val, sizeof(val), "%.2f %s", m.median, m.display.unit.c_str());
            std::printf("%12s %14s %8.2f%%  %s%s\n", format_size(first.working_set_bytes).c_str(), val, m.cv,
                        m.ok ? "ok" : "MISMATCH", m.cv > kCvWarnPercent ? "  (unstable)" : "");
            std::fflush(stdout);
        }
        if (size > o.to / 2) break;
    }
    if (o.json) {
        std::printf("],\"checksum_ok\":%s}\n", all_ok ? "true" : "false");
    } else {
        print_session_warnings(s);
        if (!all_ok) std::printf("Checksum mismatch detected - results are not trustworthy\n");
    }
    return all_ok ? 0 : 3;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return usage();
    std::wstring cmd = argv[1];
    if (cmd == L"info") return cmd_info(argc > 2 && std::wstring(argv[2]) == L"--json");
    if (cmd == L"run") return cmd_run(argc, argv);
    if (cmd == L"sweep") return cmd_sweep(argc, argv);
    if (cmd == L"check") return cmd_check(argc, argv);
    return usage();
}
