// cpu-bench-runner: 측정 전용 프로세스 (CLI)
//
//   cpu-bench-runner info [--json]
//   cpu-bench-runner run <module id | dll 경로> [--threads N | --mt] [--duration-ms N]
//                        [--runs N] [--seed N] [--no-pin] [--json]
//
// 측정 절차: 프로세스 우선순위 HIGH → 워밍업 1회 → N회 측정 → 중앙값 채택, 변동계수(CV) 계산
#include "bench_api.h"
#include "topology.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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
                 "  cpu-bench-runner run <module id | dll path> [--threads N | --mt] [--duration-ms N]\n"
                 "                       [--runs N] [--seed N] [--no-pin] [--json]\n");
    return 2;
}

// ---------------------------------------------------------------- info

int cmd_info(bool json) {
    Topology topo;
    std::string err;
    if (!query_topology(topo, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (json) {
        std::printf("{\"cpu\":%s,\"logical\":%zu,\"cores\":%u,\"groups\":%u,\"hybrid\":%s,\"perf_cores\":%u,\"caches\":[",
                    json_str(topo.brand).c_str(), topo.cpus.size(), topo.cores, topo.groups,
                    topo.hybrid ? "true" : "false", topo.perf_cores);
        for (size_t i = 0; i < topo.caches.size(); ++i) {
            const auto& c = topo.caches[i];
            std::printf("%s{\"level\":%u,\"type\":\"%s\",\"size\":%u,\"count\":%u}", i ? "," : "", c.level, c.type,
                        c.size_bytes, c.count);
        }
        std::printf("]}\n");
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
    return 0;
}

// ---------------------------------------------------------------- run

struct Module {
    HMODULE dll = nullptr;
    PFN_bench_get_info get_info = nullptr;
    PFN_bench_init init = nullptr;
    PFN_bench_run run = nullptr;
    PFN_bench_shutdown shutdown = nullptr;
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
    const BenchInfo* info = m.get_info();
    if (!info || info->api_version != BENCH_API_VERSION) {
        err = "module API version mismatch";
        return false;
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

int cmd_run(int argc, wchar_t** argv) {
    if (argc < 3) return usage();
    std::wstring module_arg = argv[2];
    uint32_t threads = 1, duration_ms = 2000, runs = 5;
    uint64_t seed = 1;
    bool mt_all = false, pin = true, json = false;

    for (int i = 3; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next_u64 = [&](uint64_t& v) {
            if (i + 1 >= argc) return false;
            wchar_t* end = nullptr;
            v = std::wcstoull(argv[++i], &end, 0);
            return end && *end == 0;
        };
        uint64_t v = 0;
        if (a == L"--threads" && next_u64(v) && v > 0) threads = uint32_t(v);
        else if (a == L"--mt") mt_all = true;
        else if (a == L"--duration-ms" && next_u64(v) && v > 0) duration_ms = uint32_t(v);
        else if (a == L"--runs" && next_u64(v) && v > 0) runs = uint32_t(v);
        else if (a == L"--seed" && next_u64(v)) seed = v;
        else if (a == L"--no-pin") pin = false;
        else if (a == L"--json") json = true;
        else return usage();
    }

    Topology topo;
    std::string err;
    if (!query_topology(topo, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (mt_all) threads = uint32_t(topo.cpus.size());

    Module mod;
    if (!load_module(resolve_module(module_arg), mod, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    const BenchInfo* info = mod.get_info();

    // REALTIME은 쓰지 않는다 (시스템이 멈출 수 있음). HIGH만 사용.
    bool high_priority = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != 0;

    PinPlan plan;
    plan.enabled = pin;
    plan.order = placement_order(topo);

    BenchConfig cfg{};
    cfg.struct_size = sizeof(cfg);
    cfg.threads = threads;
    cfg.duration_ms = duration_ms;
    cfg.flags = BENCH_FLAG_NONE;
    cfg.seed = seed;
    cfg.thread_init = &on_thread_init;
    cfg.user = &plan;

    BenchContext* ctx = nullptr;
    int32_t rc = mod.init(&cfg, &ctx);
    if (rc != BENCH_OK) {
        std::fprintf(stderr, "error: bench_init failed (%s)\n", status_name(rc));
        return 1;
    }

    if (!json) {
        std::printf("Module   : %s %s\n", info->name, info->version);
        std::printf("CPU      : %s\n", topo.brand.c_str());
        std::printf("Threads  : %u%s, %u ms x %u runs (+1 warm-up), seed %llu\n", threads,
                    pin ? (threads == 1 && topo.hybrid ? ", pinned to a performance core" : ", pinned") : "",
                    duration_ms, runs, static_cast<unsigned long long>(seed));
    }

    std::vector<BenchResult> results;
    int32_t worst_rc = BENCH_OK;
    for (uint32_t r = 0; r <= runs; ++r) {  // r == 0은 워밍업
        BenchResult res{};
        res.struct_size = sizeof(res);
        rc = mod.run(ctx, &res, nullptr, nullptr, nullptr);
        if (rc != BENCH_OK && rc != BENCH_E_CHECKSUM) {
            std::fprintf(stderr, "error: bench_run failed (%s)\n", status_name(rc));
            mod.shutdown(ctx);
            return 1;
        }
        if (rc == BENCH_E_CHECKSUM) worst_rc = rc;
        if (r == 0) continue;
        results.push_back(res);
        if (!json)
            std::printf("  run %u  : %12.1f %s/s  (%llu %ss, %.3f s)%s\n", r, res.ops_per_sec, info->unit_name,
                        static_cast<unsigned long long>(res.iterations), info->unit_name, res.elapsed_s,
                        res.mismatches ? "  CHECKSUM MISMATCH" : "");
    }
    mod.shutdown(ctx);

    std::vector<double> ops;
    uint64_t mismatches = 0;
    bool checksum_stable = true;
    for (const auto& r : results) {
        ops.push_back(r.ops_per_sec);
        mismatches += r.mismatches;
        if (r.checksum != results.front().checksum) checksum_stable = false;
    }
    double mean = 0;
    for (double o : ops) mean += o;
    mean /= double(ops.size());
    double var = 0;
    for (double o : ops) var += (o - mean) * (o - mean);
    double stdev = ops.size() > 1 ? std::sqrt(var / double(ops.size() - 1)) : 0.0;
    double cv = mean > 0 ? 100.0 * stdev / mean : 0.0;
    double med = median(ops);
    bool ok = mismatches == 0 && checksum_stable && worst_rc == BENCH_OK;

    if (json) {
        std::printf("{\"module\":%s,\"module_version\":%s,\"variant\":%s,\"cpu\":%s,\"threads\":%u,"
                    "\"pinned\":%s,\"pin_failures\":%d,\"high_priority\":%s,\"duration_ms\":%u,\"seed\":%llu,"
                    "\"runs\":[",
                    json_str(info->id).c_str(), json_str(info->version).c_str(),
                    json_str(results.front().variant).c_str(), json_str(topo.brand).c_str(), threads,
                    pin ? "true" : "false", plan.failures.load(), high_priority ? "true" : "false", duration_ms,
                    static_cast<unsigned long long>(seed));
        for (size_t i = 0; i < results.size(); ++i)
            std::printf("%s{\"ops_per_sec\":%.3f,\"iterations\":%llu,\"elapsed_s\":%.6f,\"mismatches\":%llu}",
                        i ? "," : "", results[i].ops_per_sec,
                        static_cast<unsigned long long>(results[i].iterations), results[i].elapsed_s,
                        static_cast<unsigned long long>(results[i].mismatches));
        std::printf("],\"median_ops_per_sec\":%.3f,\"cv_percent\":%.3f,\"checksum\":\"0x%016llx\","
                    "\"checksum_ok\":%s}\n",
                    med, cv, static_cast<unsigned long long>(results.front().checksum), ok ? "true" : "false");
    } else {
        std::printf("Median   : %.1f %s/s  (CV %.2f%%)\n", med, info->unit_name, cv);
        std::printf("Variant  : %s\n", results.front().variant);
        std::printf("Checksum : 0x%016llx %s\n", static_cast<unsigned long long>(results.front().checksum),
                    ok ? "(stable)" : "(MISMATCH - results are not trustworthy)");
        if (cv > kCvWarnPercent)
            std::printf("Warning  : run-to-run variation is above %.0f%%. Close background apps and retry.\n",
                        kCvWarnPercent);
        if (!high_priority) std::printf("Warning  : could not raise process priority\n");
        if (plan.failures) std::printf("Warning  : %d thread(s) could not be pinned\n", plan.failures.load());
    }
    return ok ? 0 : 3;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return usage();
    std::wstring cmd = argv[1];
    if (cmd == L"info") return cmd_info(argc > 2 && std::wstring(argv[2]) == L"--json");
    if (cmd == L"run") return cmd_run(argc, argv);
    return usage();
}
