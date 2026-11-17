// SMBIOS 읽기: 메모리 모듈(Type 17). 관리자 권한·드라이버 없이 GetSystemFirmwareTable('RSMB')로 읽는다.
//
// 개인정보: 시리얼 번호(Type 17 offset 0x18), 자산 태그, 시스템 UUID는 읽지도 저장하지도 않는다.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct MemoryModule {
    std::string device_locator;  // 슬롯 이름 (예: "DIMM_A2", "Controller0-ChannelA-DIMM0")
    std::string bank_locator;    // 예: "BANK 0", "P0 CHANNEL A"
    std::string manufacturer;
    std::string part_number;
    uint64_t size_bytes = 0;     // 0 = 비어 있는 슬롯
    uint8_t memory_type = 0;     // SMBIOS 코드 (0x1A DDR4, 0x22 DDR5 ...)
    uint8_t form_factor = 0;     // SMBIOS 코드 (0x09 DIMM, 0x0D SODIMM ...)
    uint8_t rank = 0;            // 0 = 알 수 없음
    uint32_t max_speed = 0;      // MT/s, SMBIOS "Speed": 모듈이 낼 수 있는 최대 속도(보통 JEDEC 기준). 0 = 알 수 없음
    uint32_t configured_speed = 0;  // MT/s, 지금 설정된 속도. 0 = 알 수 없음
    std::string channel;         // 슬롯 이름에서 읽은 채널 (예: "A", "0B"). 못 읽으면 빈 문자열
};

struct MemoryInfo {
    std::vector<MemoryModule> modules;  // 비어 있는 슬롯 포함
    uint8_t smbios_major = 0, smbios_minor = 0;

    size_t populated() const;
    uint64_t total_bytes() const;
    // 채널 수: 모듈이 1개면 1, 모든 모듈의 채널을 슬롯 이름에서 읽었으면 서로 다른 채널 개수, 아니면 0(알 수 없음)
    unsigned channels() const;
};

// RawSMBIOSData 전체(헤더 8바이트 + 표)를 해석한다. 단위 테스트에서 직접 부른다.
bool parse_smbios(const uint8_t* raw, size_t len, MemoryInfo& out, std::string& error);

bool query_memory(MemoryInfo& out, std::string& error);

// "CHANNEL A", "ChannelB", "DIMM_A1", "A2", "Controller1-ChannelA-DIMM0" 같은 이름에서 채널을 읽는다.
std::string channel_from_locator(const std::string& device, const std::string& bank);

const char* memory_type_name(uint8_t code);
const char* form_factor_name(uint8_t code);
