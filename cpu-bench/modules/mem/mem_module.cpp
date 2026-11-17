// 메모리·캐시(MEM) 부하 모듈 DLL: bench_api.h 구현
//
// 측정 종류 (BenchConfig.workload)
//   read    : 데이터를 읽어 합계 (unit "byte", 읽은 바이트)
//   write   : 고정 패턴을 기록 (unit "byte", 쓴 바이트)
//   copy    : 원본 → 대상 복사 (unit "byte", STREAM처럼 읽기+쓰기 바이트를 모두 셈)
//   latency : 무작위 순환 포인터 추적 (unit "load", 의존 로드 횟수. 1회 시간 = 지연)
// working_set_bytes는 전체 스레드가 다루는 데이터 크기. 스레드마다 같은 크기의 구간을 맡는다.
// copy는 원본과 대상이 절반씩이다.
//
// checksum
//   read/copy : 입력 데이터 전체 합 (스레드 수와 무관). read는 매 회 구간 합을 확인하고,
//               copy는 측정이 끝난 뒤 대상 구간 합을 확인한다(측정 시간 밖).
//   write     : 기록한 패턴의 합. 측정이 끝난 뒤 다시 읽어 확인한다(측정 시간 밖).
//   latency   : 스레드별 체인 digest의 합 (스레드 수에 따라 달라짐). 한 바퀴를 돌 때마다
//               출발 노드로 돌아왔는지 확인한다.
#define BENCH_MODULE_BUILD
#include "bench_api.h"
#include "bench_harness.h"
#include "mem_kernels.h"

#include <cstring>
#include <new>
#include <vector>

namespace {

const BenchInfo kInfo = {sizeof(BenchInfo), BENCH_API_VERSION,     "mem", "Memory (bandwidth, latency)",
                         "0.1.0",           "byte",                "read,write,copy,latency"};

enum class Workload { Read, Write, Copy, Latency };

constexpr uint64_t kDefaultWorkingSet = 256ull << 20;  // L3보다 충분히 커서 DRAM을 재는 크기
constexpr size_t kMinSliceBytes = 4096;
constexpr size_t kBytesPerStep = 1u << 20;             // 대역폭: 시간 확인 1회당 최소 처리량
constexpr size_t kChaseChunk = 16384;                  // 지연: 시간 확인 1회당 걸음 수

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct alignas(64) ChaseState {  // 워커별 추적 위치 (캐시 라인 공유 방지)
    const void* p = nullptr;
    size_t pos = 0;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace

struct BenchContext {
    BenchConfig cfg{};
    Workload wl = Workload::Read;
    void* mem = nullptr;
    size_t mem_bytes = 0;
    uint64_t* src = nullptr;  // read/copy 원본, write 대상
    uint64_t* dst = nullptr;  // copy 대상
    size_t slice_words = 0;   // 대역폭: 스레드당 uint64 개수
    size_t repeats = 1;       // 대역폭: step 1회에 구간을 몇 번 처리하는지
    std::vector<uint64_t> expected;  // 스레드 구간별 기대 합
    uint64_t lo = 0, hi = 0;  // write 패턴
    size_t nodes = 0;         // 지연: 스레드당 노드 수
    size_t cycles_per_step = 0;  // 지연: 체인이 짧으면 step 1회에 여러 바퀴
    uint64_t checksum = 0;
    uint64_t ws_used = 0;

    ~BenchContext() {
        if (mem) VirtualFree(mem, 0, MEM_RELEASE);
    }
    char* slice_base(uint32_t t) const { return static_cast<char*>(mem) + size_t(t) * nodes * memk::kLine; }
};

namespace {

bool parse_workload(const char* s, Workload& wl) {
    if (!s || !*s || !std::strcmp(s, "read")) wl = Workload::Read;
    else if (!std::strcmp(s, "write")) wl = Workload::Write;
    else if (!std::strcmp(s, "copy")) wl = Workload::Copy;
    else if (!std::strcmp(s, "latency")) wl = Workload::Latency;
    else return false;
    return true;
}

int32_t setup(BenchContext& c) {
    const uint32_t n = c.cfg.threads;
    const uint64_t ws = c.cfg.working_set_bytes ? c.cfg.working_set_bytes : kDefaultWorkingSet;
    const uint64_t part = c.wl == Workload::Copy ? ws / 2 : ws;
    const uint64_t slice_bytes = part / n / memk::kLine * memk::kLine;
    if (slice_bytes < kMinSliceBytes) return BENCH_E_INVALID_ARG;
    if (c.wl == Workload::Latency && slice_bytes / memk::kLine > 0xFFFFFFFFull) return BENCH_E_INVALID_ARG;

    c.ws_used = slice_bytes * n * (c.wl == Workload::Copy ? 2 : 1);
    c.mem_bytes = size_t(c.ws_used);
    c.mem = VirtualAlloc(nullptr, c.mem_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!c.mem) return BENCH_E_OUT_OF_MEMORY;

    if (c.wl == Workload::Latency) {
        c.nodes = size_t(slice_bytes / memk::kLine);
        c.cycles_per_step = c.nodes <= kChaseChunk ? kChaseChunk / c.nodes : 0;
        std::vector<uint32_t> order(c.nodes);
        for (uint32_t t = 0; t < n; ++t) {
            void* base = c.slice_base(t);
            memk::build_chain(base, c.nodes, c.cfg.seed ^ (uint64_t(t) * 0xD1B54A32D192ED03ull), order.data());
            c.checksum += memk::chain_digest(base, c.nodes);
        }
        return BENCH_OK;
    }

    c.slice_words = size_t(slice_bytes / sizeof(uint64_t));
    c.repeats = slice_bytes >= kBytesPerStep ? 1 : size_t(kBytesPerStep / slice_bytes);
    c.src = static_cast<uint64_t*>(c.mem);
    const size_t words = c.slice_words * n;
    c.expected.resize(n);
    if (c.wl == Workload::Write) {
        memk::write_pattern(c.cfg.seed, c.lo, c.hi);
        std::memset(c.src, 0, words * sizeof(uint64_t));  // 페이지를 미리 확보 (첫 접근 비용 제외)
        for (uint32_t t = 0; t < n; ++t) c.expected[t] = (c.lo + c.hi) * (c.slice_words / 2);
    } else {
        memk::fill_stream(c.src, words, c.cfg.seed);
        for (uint32_t t = 0; t < n; ++t)
            c.expected[t] = memk::sum_words_scalar(c.src + size_t(t) * c.slice_words, c.slice_words);
        if (c.wl == Workload::Copy) {
            c.dst = c.src + words;
            std::memset(c.dst, 0, words * sizeof(uint64_t));
        }
    }
    for (uint64_t e : c.expected) c.checksum += e;
    return BENCH_OK;
}

}  // namespace

extern "C" {

BENCH_EXPORT const BenchInfo* BENCH_CALL bench_get_info(void) { return &kInfo; }

BENCH_EXPORT int32_t BENCH_CALL bench_init(const BenchConfig* config, BenchContext** out_ctx) {
    if (!out_ctx) return BENCH_E_INVALID_ARG;
    *out_ctx = nullptr;
    BenchConfig cfg;
    if (!bh::copy_config(config, cfg)) return BENCH_E_INVALID_ARG;
    Workload wl;
    if (!parse_workload(cfg.workload, wl)) return BENCH_E_UNSUPPORTED;
    cfg.workload = nullptr;  // 호출자 문자열을 붙잡아 두지 않는다
    try {
        auto* ctx = new BenchContext();
        ctx->cfg = cfg;
        ctx->wl = wl;
        int32_t rc = setup(*ctx);
        if (rc != BENCH_OK) {
            delete ctx;
            return rc;
        }
        *out_ctx = ctx;
        return BENCH_OK;
    } catch (const std::bad_alloc&) {
        return BENCH_E_OUT_OF_MEMORY;
    } catch (...) {
        return BENCH_E_INTERNAL;
    }
}

BENCH_EXPORT int32_t BENCH_CALL bench_run(BenchContext* ctx, BenchResult* out, const volatile int32_t* cancel,
                                          BenchProgressFn progress, void* progress_user) {
    if (!ctx || !bh::result_size_ok(out)) return BENCH_E_INVALID_ARG;
    BenchContext& c = *ctx;
    const uint32_t n = c.cfg.threads;
    const size_t sw = c.slice_words;
    const uint64_t slice_bytes = uint64_t(sw) * sizeof(uint64_t);

    bh::RunTotals t;
    int32_t rc = BENCH_OK;
    const char* variant = "sse2";
    const char* unit = "byte";

    switch (c.wl) {
        case Workload::Read: {
            auto step = [&](uint32_t idx, bh::WorkerStats& st) {
                const uint64_t* p = c.src + size_t(idx) * sw;
                for (size_t r = 0; r < c.repeats; ++r) {
                    if (memk::sum_words(p, sw) != c.expected[idx]) ++st.mismatches;
                    st.ops += slice_bytes;
                }
            };
            rc = bh::run_workers(c.cfg, cancel, progress, progress_user, step, t);
            break;
        }
        case Workload::Write: {
            auto step = [&](uint32_t idx, bh::WorkerStats& st) {
                uint64_t* p = c.src + size_t(idx) * sw;
                for (size_t r = 0; r < c.repeats; ++r) {
                    memk::fill_words(p, sw, c.lo, c.hi);
                    st.ops += slice_bytes;
                }
            };
            rc = bh::run_workers(c.cfg, cancel, progress, progress_user, step, t);
            if (rc == BENCH_OK)  // 측정 시간 밖에서 기록 결과 확인
                for (uint32_t i = 0; i < n; ++i)
                    if (memk::sum_words(c.src + size_t(i) * sw, sw) != c.expected[i]) ++t.mismatches;
            break;
        }
        case Workload::Copy: {
            auto step = [&](uint32_t idx, bh::WorkerStats& st) {
                const size_t off = size_t(idx) * sw;
                for (size_t r = 0; r < c.repeats; ++r) {
                    memk::copy_words(c.dst + off, c.src + off, sw);
                    st.ops += 2 * slice_bytes;
                }
            };
            rc = bh::run_workers(c.cfg, cancel, progress, progress_user, step, t);
            if (rc == BENCH_OK)
                for (uint32_t i = 0; i < n; ++i)
                    if (memk::sum_words(c.dst + size_t(i) * sw, sw) != c.expected[i]) ++t.mismatches;
            break;
        }
        case Workload::Latency: {
            variant = "scalar";
            unit = "load";
            std::vector<ChaseState> state;
            try {
                state.resize(n);
            } catch (...) {
                return BENCH_E_OUT_OF_MEMORY;
            }
            for (uint32_t i = 0; i < n; ++i) state[i].p = c.slice_base(i);
            auto step = [&](uint32_t idx, bh::WorkerStats& st) {
                ChaseState& s = state[idx];
                const void* start = c.slice_base(idx);
                if (c.cycles_per_step) {  // 짧은 체인: 여러 바퀴를 돌고 출발점 확인
                    const size_t steps = c.cycles_per_step * c.nodes;
                    s.p = memk::chase(s.p, steps);
                    if (s.p != start) {
                        ++st.mismatches;
                        s.p = start;
                    }
                    st.ops += steps;
                    return;
                }
                const size_t steps = std::min(kChaseChunk, c.nodes - s.pos);
                s.p = memk::chase(s.p, steps);
                s.pos += steps;
                st.ops += steps;
                if (s.pos == c.nodes) {  // 한 바퀴 완료: 출발점으로 돌아와야 한다
                    if (s.p != start) {
                        ++st.mismatches;
                        s.p = start;
                    }
                    s.pos = 0;
                }
            };
            rc = bh::run_workers(c.cfg, cancel, progress, progress_user, step, t);
            break;
        }
    }
    if (rc != BENCH_OK) return rc;
    bh::fill_result(out, n, t, c.checksum, variant, c.ws_used, unit);
    return bh::status_of(t);
}

BENCH_EXPORT void BENCH_CALL bench_shutdown(BenchContext* ctx) { delete ctx; }

}  // extern "C"
