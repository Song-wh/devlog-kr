/*
 * bench_api.h - 부하 모듈 공통 인터페이스 (C ABI)
 *
 * 모든 부하 모듈 DLL은 아래 네 함수를 export한다.
 *   bench_get_info  : 모듈 정보 (id, 이름, 버전, API 버전)
 *   bench_init      : 설정을 받아 컨텍스트 생성 (입력 데이터 준비는 여기서, 측정 시간에 포함되지 않음)
 *   bench_run       : duration_ms 동안 부하를 돌리고 결과 기록
 *   bench_shutdown  : 컨텍스트 해제
 *
 * checksum 규칙
 *   모듈은 "작업 단위(unit)" 하나를 반복 실행한다. 같은 seed면 작업 단위의 결과(digest)는
 *   항상 같아야 한다. 그래서 checksum으로 두 가지를 검증한다.
 *     1) 컴파일러가 계산을 제거하지 않았는지: 결과가 seed별 기대값과 일치해야 한다.
 *     2) 시스템이 안정적인지: 반복 중 digest가 한 번이라도 달라지면 mismatches가 증가한다
 *        (불안정한 오버클럭/XMP에서 생기는 계산 오류).
 *
 * 구조체는 struct_size를 첫 필드로 두어, 나중에 필드를 뒤에 추가해도 이전 러너/모듈과 호환되게 한다.
 *
 * 버전 기록
 *   1: 최초 버전
 *   2: 측정 종류(workload)와 작업 데이터 크기(working_set_bytes) 추가. 메모리 모듈처럼
 *      한 모듈이 여러 값을 재는 경우, 러너가 종류·크기를 바꿔 가며 여러 번 호출한다.
 *      BenchInfo.workloads, BenchConfig.workload/working_set_bytes,
 *      BenchResult.working_set_bytes/unit_name 추가 (모두 구조체 끝에 추가).
 */
#ifndef BENCH_API_H
#define BENCH_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BENCH_API_VERSION 2u

#if defined(_WIN32)
#  if defined(BENCH_MODULE_BUILD)
#    define BENCH_EXPORT __declspec(dllexport)
#  else
#    define BENCH_EXPORT __declspec(dllimport)
#  endif
#  define BENCH_CALL __cdecl
#else
#  define BENCH_EXPORT __attribute__((visibility("default")))
#  define BENCH_CALL
#endif

/* 반환 코드 */
enum {
    BENCH_OK              = 0,
    BENCH_E_INVALID_ARG   = -1,
    BENCH_E_OUT_OF_MEMORY = -2,
    BENCH_E_UNSUPPORTED   = -3,  /* 이 CPU에서 지원하지 않는 경로 */
    BENCH_E_CANCELLED     = -4,
    BENCH_E_CHECKSUM      = -5,  /* 반복 중 digest 불일치 발생 (결과는 채워짐) */
    BENCH_E_INTERNAL      = -6
};

/* BenchConfig.flags */
#define BENCH_FLAG_NONE       0u
#define BENCH_FLAG_SUSTAINED  (1u << 0)  /* 지속 부하 모드 (10~30분) */

/* 모듈이 워커 스레드를 만든 직후, 일을 시작하기 전에 그 스레드 안에서 호출된다.
 * 러너는 여기서 스레드를 특정 코어에 고정한다(SetThreadGroupAffinity).
 * threads == 1이면 bench_run을 호출한 스레드에서 worker 0으로 한 번 호출된다. */
typedef void (BENCH_CALL *BenchThreadInitFn)(uint32_t worker_index, void* user);

/* 진행률 콜백. fraction은 0.0~1.0. 워커 0번 스레드에서 대략 100ms 간격으로 호출된다. */
typedef void (BENCH_CALL *BenchProgressFn)(double fraction, void* user);

typedef struct BenchInfo {
    uint32_t    struct_size;
    uint32_t    api_version;   /* BENCH_API_VERSION */
    const char* id;            /* "int", "fp", "mem", "mt" */
    const char* name;          /* 사람이 읽는 이름 */
    const char* version;       /* 모듈 버전. 점수 체계 버전 관리에 사용 */
    const char* unit_name;     /* ops_per_sec의 op가 무엇인지 (예: "unit") */
    /* --- v2 --- */
    const char* workloads;     /* 지원하는 측정 종류, 쉼표로 구분, 첫 번째가 기본값. 없으면 NULL */
} BenchInfo;

typedef struct BenchConfig {
    uint32_t          struct_size;
    uint32_t          threads;      /* 1 = ST, N = MT */
    uint32_t          duration_ms;  /* 측정 1회 길이 */
    uint32_t          flags;        /* BENCH_FLAG_* */
    uint64_t          seed;         /* 입력 데이터 생성 seed */
    BenchThreadInitFn thread_init;  /* NULL 가능 */
    void*             user;         /* thread_init / progress 콜백에 그대로 전달 */
    /* --- v2 --- */
    const char*       workload;           /* NULL 또는 ""이면 기본값. 모르는 이름이면 BENCH_E_UNSUPPORTED */
    uint64_t          working_set_bytes;  /* 전체 스레드가 다루는 데이터 크기. 0이면 모듈 기본값 */
} BenchConfig;

typedef struct BenchResult {
    uint32_t struct_size;
    uint32_t threads_used;
    double   ops_per_sec;   /* 전체 스레드 합 */
    double   elapsed_s;     /* 모듈 내부에서 잰 실제 경과 시간 */
    uint64_t iterations;    /* 전체 스레드가 완료한 작업 단위 수 */
    uint64_t checksum;      /* 작업 단위 digest (seed가 같으면 항상 같음) */
    uint64_t mismatches;    /* digest가 기대값과 달랐던 작업 단위 수. 0이어야 정상 */
    char     variant[32];   /* 실제 사용한 코드 경로 (예: "scalar", "avx2") */
    /* --- v2 --- */
    uint64_t working_set_bytes;  /* 실제로 쓴 데이터 크기 (정렬 때문에 요청값보다 조금 작을 수 있음). 해당 없으면 0 */
    char     unit_name[16];      /* 이번 결과의 op 단위. 비어 있으면 BenchInfo.unit_name.
                                    "byte" = 대역폭, "load" = 의존 로드(지연) */
} BenchResult;

typedef struct BenchContext BenchContext;  /* 모듈 내부 구조, 러너는 내용을 모름 */

BENCH_EXPORT const BenchInfo* BENCH_CALL bench_get_info(void);

BENCH_EXPORT int32_t BENCH_CALL bench_init(const BenchConfig* config, BenchContext** out_ctx);

/* cancel이 가리키는 값이 0이 아니게 되면 가능한 빨리 BENCH_E_CANCELLED로 돌아온다. NULL 가능. */
BENCH_EXPORT int32_t BENCH_CALL bench_run(BenchContext* ctx,
                                          BenchResult* out_result,
                                          const volatile int32_t* cancel,
                                          BenchProgressFn progress,
                                          void* progress_user);

BENCH_EXPORT void BENCH_CALL bench_shutdown(BenchContext* ctx);

/* 러너가 GetProcAddress로 받을 함수 포인터 타입 */
typedef const BenchInfo* (BENCH_CALL *PFN_bench_get_info)(void);
typedef int32_t (BENCH_CALL *PFN_bench_init)(const BenchConfig*, BenchContext**);
typedef int32_t (BENCH_CALL *PFN_bench_run)(BenchContext*, BenchResult*, const volatile int32_t*,
                                            BenchProgressFn, void*);
typedef void (BENCH_CALL *PFN_bench_shutdown)(BenchContext*);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_API_H */
