// 정수 모듈의 계산 커널. DLL 인터페이스와 분리해서 단위 테스트에서 직접 링크한다.
// 같은 계산의 Python 독립 구현: tools/int_reference.py
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace intk {

constexpr size_t kBufBytes  = 65536;  // 작업 단위 입력 크기 (L2에 들어가는 크기)
constexpr size_t kSortCount = 8192;   // 정렬할 uint32 개수 (입력 앞 32KB)
constexpr size_t kTextWords = 4096;   // 파서 입력 숫자 개수

struct Input {
    std::vector<uint8_t> buf;  // kBufBytes, seed로 생성
    std::string text;          // "12345;678;..." 형식, 파서 입력
};

uint64_t splitmix64(uint64_t& state);

// zlib과 같은 CRC-32 (다항식 0xEDB88320). crc 인자로 이어서 계산 가능.
uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0);

void sha256(const uint8_t* data, size_t len, uint8_t out[32]);

// ';'로 끝나는 10진수 목록을 읽어 합계를 돌려준다.
uint64_t parse_sum(const char* text, size_t len, uint32_t* count);

Input make_input(uint64_t seed);

// 작업 단위 1회: CRC-32 + SHA-256 + 정렬 + 파싱. scratch는 kSortCount 크기.
uint64_t run_unit(const Input& in, uint32_t* scratch);

}  // namespace intk
