// 정수(INT) 부하 모듈 DLL: bench_api.h 구현
#define BENCH_MODULE_BUILD
#include "bench_api.h"
#include "bench_harness.h"
#include "int_kernels.h"

#include <new>
#include <vector>

struct BenchContext {
    BenchConfig cfg{};
    intk::Input input;
    uint64_t expected = 0;  // bench_init에서 한 번 계산한 기준 digest
    std::vector<std::vector<uint32_t>> scratch;  // 워커별 정렬 버퍼
};

namespace {

const BenchInfo kInfo = {sizeof(BenchInfo), BENCH_API_VERSION, "int", "Integer (CRC32, SHA-256, sort, parse)",
                         "0.1.0", "unit", nullptr};

}  // namespace

extern "C" {

BENCH_EXPORT const BenchInfo* BENCH_CALL bench_get_info(void) { return &kInfo; }

BENCH_EXPORT int32_t BENCH_CALL bench_init(const BenchConfig* config, BenchContext** out_ctx) {
    if (!out_ctx) return BENCH_E_INVALID_ARG;
    *out_ctx = nullptr;
    BenchConfig cfg;
    if (!bh::copy_config(config, cfg)) return BENCH_E_INVALID_ARG;
    // 정수 모듈은 측정 종류와 작업 크기가 고정이다
    if ((cfg.workload && cfg.workload[0]) || cfg.working_set_bytes) return BENCH_E_UNSUPPORTED;
    try {
        auto* ctx = new BenchContext();
        ctx->cfg = cfg;
        ctx->input = intk::make_input(cfg.seed);
        ctx->scratch.assign(cfg.threads, std::vector<uint32_t>(intk::kSortCount));
        ctx->expected = intk::run_unit(ctx->input, ctx->scratch[0].data());
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

    const intk::Input& in = ctx->input;
    const uint64_t expected = ctx->expected;
    auto step = [&](uint32_t idx, bh::WorkerStats& st) {
        uint64_t d = intk::run_unit(in, ctx->scratch[idx].data());
        if (d != expected) ++st.mismatches;
        ++st.ops;
    };

    bh::RunTotals t;
    int32_t rc = bh::run_workers(ctx->cfg, cancel, progress, progress_user, step, t);
    if (rc != BENCH_OK) return rc;
    bh::fill_result(out, ctx->cfg.threads, t, expected, "scalar", 0, "unit");
    return bh::status_of(t);
}

BENCH_EXPORT void BENCH_CALL bench_shutdown(BenchContext* ctx) { delete ctx; }

}  // extern "C"
