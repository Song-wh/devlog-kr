// 메모리 모듈의 계산 커널. DLL 인터페이스와 분리해서 단위 테스트에서 직접 링크한다.
// 데이터·체인 생성 규칙의 Python 독립 구현: tools/mem_reference.py
#pragma once

#include <cstddef>
#include <cstdint>

namespace memk {

constexpr size_t kLine = 64;                       // 캐시 라인 크기
constexpr size_t kWordsPerLine = kLine / sizeof(uint64_t);
constexpr size_t kDigestSteps = 4096;              // 지연 체인 digest에 쓰는 걸음 수

uint64_t splitmix64(uint64_t& state);

// 대역폭 입력 데이터: i번째 uint64 = state=seed에서 시작한 splitmix64의 i번째 출력
void fill_stream(uint64_t* dst, size_t words, uint64_t seed);

// 쓰기 측정에 쓰는 패턴 (lo, hi 번갈아 기록)
void write_pattern(uint64_t seed, uint64_t& lo, uint64_t& hi);

// --- 대역폭 커널 (SSE2, 64바이트 단위. words는 kWordsPerLine의 배수, 포인터는 16바이트 정렬)
uint64_t sum_words(const uint64_t* p, size_t words);                 // 읽기
void fill_words(uint64_t* p, size_t words, uint64_t lo, uint64_t hi);  // 쓰기
void copy_words(uint64_t* dst, const uint64_t* src, size_t words);    // 복사

// 테스트용 단순 구현
uint64_t sum_words_scalar(const uint64_t* p, size_t words);

// --- 지연 (포인터 추적)
// base부터 64바이트 간격 노드 nodes개를 하나의 무작위 순환(Sattolo 알고리즘)으로 잇는다.
// 노드 k의 첫 8바이트에 다음 노드의 주소를 기록한다. order는 nodes개짜리 작업 공간.
void build_chain(void* base, size_t nodes, uint64_t seed, uint32_t* order);

// 노드 0에서 min(nodes, kDigestSteps)걸음 따라가며 방문한 노드 번호로 만든 digest
uint64_t chain_digest(const void* base, size_t nodes);

// p에서 steps번 다음 노드로 이동한 위치. 각 로드가 앞 로드 결과에 의존하므로 지연만큼 걸린다.
const void* chase(const void* p, size_t steps);

}  // namespace memk
